/**
 * @file board.c
 * @brief Minimal H503 board support (clock, console, tick) - see board.h
 */

#include "board.h"

#include "stm32h5xx.h"

/* The clock this template is wired for. 64MHz raw HSI is the H5 bring-up clock:
 * no crystal, no PLL to lock, no wait that can hang, and it already matches the
 * OW_PORT_SYSCLK_MHZ the CMake project passes to the library. Switching to the
 * 250MHz HSE+PLL clock is a matter of extending board_clock_init() the way
 * examples/app/app.c does and passing -DOW_SYSCLK_MHZ=250 with the right
 * -DOW_HSE_MHZ; the 1-Wire port is already timing-correct for any of them.
 */
#ifndef OW_PORT_SYSCLK_MHZ
#error "OW_PORT_SYSCLK_MHZ must be defined (the CMake project sets it from OW_SYSCLK_MHZ)"
#endif
#if (OW_PORT_SYSCLK_MHZ) != 64
#error "This template has only the 64MHz raw-HSI clock implemented; see board.c"
#endif

#define CONSOLE_BAUD 115200u
#define CONSOLE_BRR(pclk_hz) (((pclk_hz) + (CONSOLE_BAUD / 2u)) / CONSOLE_BAUD)

static uint32_t ms_count;

/**
 * @brief 64MHz from the internal RC, with the flash latency set first
 * @note The H5 HSI runs at 64MHz out of reset but the reset divider selects
 *       HSIDIV=/2, so the core actually boots at 32MHz. Clearing the divider is
 *       the whole clock. FLASH_ACR is raised before that: 3 wait states cover
 *       64MHz in every voltage range, and prefetch is required once WS > 0.
 */
static void board_clock_init(void) {
    FLASH->ACR = FLASH_ACR_PRFTEN | FLASH_ACR_LATENCY_3WS;
    RCC->CR &= (uint32_t)~RCC_CR_HSIDIV;
}

/**
 * @brief USART1 TX on PA9 (AF7), 115200 8N1, TX only
 */
static void board_console_init(void) {
    RCC->AHB2ENR |= RCC_AHB2ENR_GPIOAEN;
    RCC->APB2ENR |= RCC_APB2ENR_USART1EN;

    /* PA9 alternate function push-pull, AF7 = USART1_TX (pin 9 is in AFR[1]). */
    GPIOA->MODER = (GPIOA->MODER & ~GPIO_MODER_MODE9) | GPIO_MODER_MODE9_1;
    GPIOA->OTYPER &= ~GPIO_OTYPER_OT9;
    GPIOA->AFR[1] = (GPIOA->AFR[1] & ~GPIO_AFRH_AFSEL9) | (7u << GPIO_AFRH_AFSEL9_Pos);

    /* USART1 is on APB2, left at /1, so PCLK2 = SYSCLK. */
    USART1->BRR = CONSOLE_BRR((uint32_t)(OW_PORT_SYSCLK_MHZ) * 1000000u);
    USART1->CR1 = USART_CR1_TE | USART_CR1_UE;
}

/**
 * @brief 1 kHz SysTick, interrupt deliberately disabled
 * @note COUNTFLAG is polled by board_millis(); no handler is installed and no
 *       NVIC bit is enabled, so the build carries no interrupt at all (the
 *       driver's low-power mode wakes on the timer event, not an ISR).
 */
static void board_tick_init(void) {
    ms_count = 0;
    SysTick->LOAD = ((uint32_t)(OW_PORT_SYSCLK_MHZ) * 1000u) - 1u;
    SysTick->VAL = 0;
    SysTick->CTRL = SysTick_CTRL_CLKSOURCE_Msk | SysTick_CTRL_ENABLE_Msk;
}

void board_init(void) {
    board_clock_init();
    board_console_init();
    board_tick_init();
}

uint32_t board_millis(void) {
    if (SysTick->CTRL & SysTick_CTRL_COUNTFLAG_Msk) {
        (void)SysTick->CTRL; /* reading CTRL clears COUNTFLAG */
        ms_count++;
    }
    return ms_count;
}

void board_putc(char c) {
    while (!(USART1->ISR & USART_ISR_TXE_TXFNF)) {
    }
    USART1->TDR = (uint8_t)c;
}

void board_puts(const char* s) {
    while (*s) {
        board_putc(*s++);
    }
}

void board_put_int(int32_t value) {
    char buf[12]; /* enough for -2147483648 and '\0' */
    char* p = buf + sizeof(buf) - 1;
    *p = '\0';

    if (value == 0) {
        *(--p) = '0';
    } else {
        int negative = 0;
        unsigned int uvalue;
        if (value < 0) {
            negative = 1;
            uvalue = (unsigned int)-(value + 1) + 1u;
        } else {
            uvalue = (unsigned int)value;
        }
        do {
            *(--p) = (char)('0' + (uvalue % 10u));
            uvalue /= 10u;
        } while (uvalue);
        if (negative) {
            *(--p) = '-';
        }
    }
    board_puts(p);
}

/* No _write retarget here: the template uses board_puts()/board_put_int() rather
 * than stdio, so -lnosys's own _write stub is never called. To use printf(),
 * add a strong _write() (or link --specs=nosys.specs plus your own retarget set)
 * and drop -lnosys from the link. */
