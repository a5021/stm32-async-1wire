/**
 * @file app.c
 * @brief Shared application layer implementation (UART TX, system clock, LED)
 */

#include "app.h"
#if defined(OW_PORT_FAMILY_F0)
#include "stm32f0xx.h"
#elif defined(OW_PORT_FAMILY_G0)
#include "stm32g0xx.h"
#elif defined(OW_PORT_FAMILY_F4)
#include "stm32f4xx.h"
#else
#include "stm32f1xx.h"
#endif

// ======== USART1 TX ring buffer ========
static uint32_t uart_tx_head = 0; // write index - points to next free slot
static uint32_t uart_tx_tail = 0; // read index - points to oldest data
static uint8_t uart_tx_buf[UART_TX_BUF_SIZE]; // circular buffer for UART transmission

/**
 * @brief Advance USART1 transmission by at most one byte (non-blocking)
 * @note Must be called periodically to feed the UART from the ring buffer
 */
void uart_poll_tx(void) {
#if defined(OW_PORT_FAMILY_G0)
    // G0 uses the modern USART naming: TXE/TXFNF lives in ISR, data in TDR
    if ((USART1->ISR & USART_ISR_TXE_TXFNF) && (uart_tx_tail != uart_tx_head)) {
        uint8_t b = uart_tx_buf[uart_tx_tail];
        uart_tx_tail = (uart_tx_tail + 1u) & UART_TX_IDX_MASK;
        USART1->TDR = b;
    }
#elif defined(OW_PORT_FAMILY_F0)
    // F0 unified USART naming: TXE lives in ISR, data goes to TDR
    if ((USART1->ISR & USART_ISR_TXE) && (uart_tx_tail != uart_tx_head)) {
        // Get byte from buffer at tail position
        uint8_t b = uart_tx_buf[uart_tx_tail];
        // Advance tail pointer with wrap-around
        uart_tx_tail = (uart_tx_tail + 1u) & UART_TX_IDX_MASK;
        // Write byte to UART data register for transmission
        USART1->TDR = b;
    }
#else
    // Check if UART is ready to transmit (TXE flag set) and buffer not empty.
    // Optional OW_UART_USART3 selects USART3/PB10 instead of USART1 (TX on PB6).
#if defined(OW_UART_USART3)
    if ((USART3->SR & USART_SR_TXE) && (uart_tx_tail != uart_tx_head)) {
        uint8_t b = uart_tx_buf[uart_tx_tail];
        uart_tx_tail = (uart_tx_tail + 1u) & UART_TX_IDX_MASK;
        USART3->DR = b;
    }
#else
    if ((USART1->SR & USART_SR_TXE) && (uart_tx_tail != uart_tx_head)) {
        // Get byte from buffer at tail position
        uint8_t b = uart_tx_buf[uart_tx_tail];
        // Advance tail pointer with wrap-around
        uart_tx_tail = (uart_tx_tail + 1u) & UART_TX_IDX_MASK;
        // Write byte to UART data register for transmission
        USART1->DR = b;
    }
#endif
#endif
}

/**
 * @brief Block until every enqueued byte has been transmitted
 * @note Blocking, intended for diagnostic/blocking code paths only; the
 *       demos keep the non-blocking uart_poll_tx() discipline.
 */
void uart_flush(void) {
    while (uart_tx_tail != uart_tx_head) {
        uart_poll_tx();
    }
}

/**
 * @brief Enqueue a single byte into the UART transmit buffer (non-blocking)
 * @param[in] b Byte to enqueue
 * @return 1 if enqueued, 0 if the buffer is full (byte dropped)
 * @note Never blocks: when the buffer is full the byte is dropped so the
 *       caller's code path stays non-blocking.
 */
int uart_tx_enqueue_byte(int b) {
    uint32_t head = uart_tx_head;
    // Calculate next head position with wrap-around using power-of-two mask
    uint32_t next = (head + 1u) & UART_TX_IDX_MASK;
    if (next != uart_tx_tail) { // Room is available
        uart_tx_buf[head] = (uint8_t)b; // Store byte at current head position
        uart_tx_head = next; // Update head pointer
        return 1;
    }
    return 0; // Buffer full - drop the byte to stay non-blocking
}

/**
 * @brief Enqueue an entire null-terminated string (non-blocking)
 * @param[in] s Null-terminated string to enqueue
 * @return Number of characters actually enqueued (may be less than strlen)
 */
int uart_write_str(const char* s) {
    const char* start = s;
    while (*s) {
        if (uart_tx_enqueue_byte(*s)) {
            s++;
        } else {
            break; // Buffer full - stop to stay non-blocking
        }
    }
    return (int)(s - start);
}

/**
 * @brief Convert integer to string and enqueue for UART transmission
 * @param[in] value Integer value to convert and transmit
 * @return Number of characters enqueued
 * @note Buffer holds 12 chars, enough for the full int32 range (-2147483648)
 */
int uart_write_int(int value) {
    char buf[12]; // enough for -2147483648 and '\0'
    char* p = buf + sizeof(buf) - 1;
    *p = '\0';

    if (value == 0) { // Special case for zero
        *(--p) = '0';
    } else {
        int is_negative = 0;
        unsigned int uvalue;

        if (value < 0) { // Handle negative numbers
            is_negative = 1;
            uvalue = (unsigned int)-(value + 1) + 1;
        } else {
            uvalue = (unsigned int)value;
        }

        do { // Convert digits from least significant to most significant
            *(--p) = '0' + (uvalue % 10);
            uvalue /= 10;
        } while (uvalue);

        if (is_negative) *(--p) = '-'; // Add negative sign if needed
    }
    return uart_write_str(p);
}

/**
 * @brief Enqueue one byte as two uppercase hexadecimal digits (non-blocking)
 * @param[in] b Byte to convert and transmit
 * @return Number of characters actually enqueued (0, 1 or 2)
 */
int uart_write_hex(uint8_t b) {
    static const char hex[] = "0123456789ABCDEF";
    int n = 0;
    n += uart_tx_enqueue_byte(hex[(b >> 4) & 0x0F]);
    n += uart_tx_enqueue_byte(hex[b & 0x0F]);
    return n;
}

/* ---- ow_stats output callbacks (strong definitions) ---- */

void ow_stats_putchar(char c) {
    uart_tx_enqueue_byte((int)c);
}

void ow_stats_puts(const char* s) {
    uart_write_str(s);
}

void ow_stats_print_int(int32_t v) {
    uart_write_int((int)v);
}

void ow_stats_print_hex(uint8_t v) {
    uart_write_hex(v);
}

void ow_stats_tx_enqueue(char c) {
    uart_tx_enqueue_byte((int)c);
}

/* Hardware bring-up (system clock, USART1 TX, LED GPIO) with full register
 *-level access.  Excluded from the host test build, which only exercises the
 * non-blocking UART ring buffer above — except configure_system_clock(),
 * which the F4 harness compiles (and test_timing drives) against the RCC
 * and FLASH mocks. hardware_init/app_init/ds18b20_busy stay target-only. */

#if !defined(DS18B20_TEST_HARNESS) || defined(OW_PORT_FAMILY_F4)
/**
 * @brief Configure system clock
 * @note The source is derived from OW_PORT_SYSCLK_MHZ (see onewire.h).
 *       F1: 72MHz via HSE+PLL x9, or raw HSI at 8MHz. F030x6 has no HSE:
 *       48MHz via HSI/2+PLL x12, or raw HSI at 8MHz. G031x6 has no HSE:
 *       64MHz via HSI16+PLL (M=1, N=8, R=2), or raw HSI16 at 16MHz.
 *       F407 (STM32F4DISCOVERY): 168MHz via 8MHz HSE + PLL (M=8, N=336,
 *       P=2), raw HSI at 16MHz, or raw HSE at 8MHz.
 *       F401 (84MHz cap): 84MHz via 8MHz HSE + PLL (M=8, N=168, P=2),
 *       raw HSI at 16MHz, or raw HSE at 8MHz.
 */
void configure_system_clock(void) {
#if defined(OW_PORT_FAMILY_G0)
#if (OW_PORT_SYSCLK_MHZ) == 64
    // HSI16 is on and stable right after reset. PLL source must be selected
    // explicitly: on this family PLLSRC=00 means "no clock sent to the PLL",
    // HSI16 encodes as 10 (RCC_PLLCFGR_PLLSRC_HSI). M(=1) xN(=8) R(=2) then
    // gives 64MHz; PLLREN enables the PLLR output the SYSCLK mux uses.
    RCC->PLLCFGR = RCC_PLLCFGR_PLLSRC_HSI | RCC_PLLCFGR_PLLN_3 |
                   RCC_PLLCFGR_PLLR_0 | RCC_PLLCFGR_PLLREN;
    RCC->CR |= RCC_CR_PLLON;
    while (!(RCC->CR & RCC_CR_PLLRDY))
        ;
    // Flash latency: 2 wait states above 48MHz (RM0444)
    FLASH->ACR = FLASH_ACR_PRFTEN | FLASH_ACR_LATENCY_1;
    // Switch system clock to PLLRCLK
    RCC->CFGR = (RCC->CFGR & ~RCC_CFGR_SW) | RCC_CFGR_SW_PLLRCLK;
    while ((RCC->CFGR & RCC_CFGR_SWS) != RCC_CFGR_SWS_PLLRCLK)
        ;
#elif (OW_PORT_SYSCLK_MHZ) == 16
    // Raw HSI16: the MCU already runs on the internal 16MHz RC after reset —
    // nothing to configure
#else
#error "Unsupported OW_PORT_SYSCLK_MHZ for G0: use 64 (HSI16+PLL) or 16 (raw HSI16)"
#endif
#elif defined(OW_PORT_FAMILY_F0)
#if (OW_PORT_SYSCLK_MHZ) == 48
    // PLL input is HSI/2 = 4MHz; x12 gives 48MHz. Configure the multiplier
    // before enabling the PLL so it locks on a valid clock (per RM0360).
    RCC->CFGR = RCC_CFGR_PLLMUL12;
    RCC->CR |= RCC_CR_PLLON;
    while (!(RCC->CR & RCC_CR_PLLRDY))
        ;
    // Flash latency for 48MHz operation (1 wait state)
    FLASH->ACR = FLASH_ACR_PRFTBE | FLASH_ACR_LATENCY;
    // Switch system clock to PLL
    RCC->CFGR |= RCC_CFGR_SW_PLL;
    while ((RCC->CFGR & RCC_CFGR_SWS) != RCC_CFGR_SWS_PLL)
        ;
#elif (OW_PORT_SYSCLK_MHZ) == 8
    // Raw HSI: the MCU already runs on the internal 8MHz RC after reset —
    // nothing to configure
#else
#error "Unsupported OW_PORT_SYSCLK_MHZ for F0: use 48 (HSI+PLL) or 8 (raw HSI)"
#endif
#elif defined(OW_PORT_FAMILY_F4)
#if (OW_PORT_SYSCLK_MHZ) == 168
    // STM32F4DISCOVERY (MB997C): HSE = 8MHz crystal -> PLL.
    //   PLL input = 8/8 = 1MHz (valid 1-2MHz), VCO = 1 * 336 = 336MHz
    //   (valid 100-432MHz), SYSCLK = 336/2 = 168MHz, USB = 336/7 = 48MHz.
    // Q=7 feeds USB (unused here) but keeps the tree CubeMX-canonical.
    RCC->CR |= RCC_CR_HSEON;
    // Wait for HSE to stabilize - HSERDY is the hardware stabilization
    // indicator, so no fixed delay is required
    while (!(RCC->CR & RCC_CR_HSERDY))
        ;
    RCC->PLLCFGR = RCC_PLLCFGR_PLLSRC_HSE | (8u << RCC_PLLCFGR_PLLM_Pos) |
                   (336u << RCC_PLLCFGR_PLLN_Pos) | (0u << RCC_PLLCFGR_PLLP_Pos) |
                   (7u << RCC_PLLCFGR_PLLQ_Pos);
    RCC->CR |= RCC_CR_PLLON;
    // Wait for the PLL to lock
    while (!(RCC->CR & RCC_CR_PLLRDY))
        ;
    // Flash latency: 5 wait states for 150 < HCLK <= 168MHz (RM0090).
    // 168MHz is the max without over-drive, so over-drive is not enabled.
    FLASH->ACR = FLASH_ACR_PRFTEN | FLASH_ACR_ICEN | FLASH_ACR_DCEN |
                 FLASH_ACR_LATENCY_5WS;
    // APB1 /4 = 42MHz, APB2 /2 = 84MHz (both at their datasheet limits). TIM1
    // is on APB2: with a prescaler != 1 the timer clock is doubled, so
    // TIM1 = 2 * 84 = 168MHz = SYSCLK (the ow_port 1us-tick invariant).
    RCC->CFGR = (RCC->CFGR & ~(RCC_CFGR_PPRE1 | RCC_CFGR_PPRE2)) |
                RCC_CFGR_PPRE1_DIV4 | RCC_CFGR_PPRE2_DIV2;
    // Switch system clock to PLL
    RCC->CFGR = (RCC->CFGR & ~RCC_CFGR_SW) | RCC_CFGR_SW_PLL;
    while ((RCC->CFGR & RCC_CFGR_SWS) != RCC_CFGR_SWS_PLL)
        ;
#elif (OW_PORT_SYSCLK_MHZ) == 84
    // STM32F401 (84MHz cap): HSE = 8MHz crystal -> PLL.
    //   PLL input = 8/8 = 1MHz (valid 1-2MHz), VCO = 1 * 168 = 168MHz
    //   (valid 100-432MHz), SYSCLK = 168/2 = 84MHz.
    RCC->CR |= RCC_CR_HSEON;
    while (!(RCC->CR & RCC_CR_HSERDY))
        ;
    RCC->PLLCFGR = RCC_PLLCFGR_PLLSRC_HSE | (8u << RCC_PLLCFGR_PLLM_Pos) |
                   (168u << RCC_PLLCFGR_PLLN_Pos) | (0u << RCC_PLLCFGR_PLLP_Pos) |
                   (7u << RCC_PLLCFGR_PLLQ_Pos);
    RCC->CR |= RCC_CR_PLLON;
    // Wait for the PLL to lock
    while (!(RCC->CR & RCC_CR_PLLRDY))
        ;
    // Flash latency: 2 wait states for 48 < HCLK <= 84MHz (RM0368).
    FLASH->ACR = FLASH_ACR_PRFTEN | FLASH_ACR_ICEN | FLASH_ACR_DCEN |
                 FLASH_ACR_LATENCY_2WS;
    // APB1 /2 = 42MHz, APB2 /2 = 42MHz (both at their datasheet limits). TIM1
    // is on APB2: with a prescaler != 1 the timer clock is doubled, so
    // TIM1 = 2 * 42 = 84MHz = SYSCLK (the ow_port 1us-tick invariant).
    RCC->CFGR = (RCC->CFGR & ~(RCC_CFGR_PPRE1 | RCC_CFGR_PPRE2)) |
                RCC_CFGR_PPRE1_DIV2 | RCC_CFGR_PPRE2_DIV2;
    // Switch system clock to PLL
    RCC->CFGR = (RCC->CFGR & ~RCC_CFGR_SW) | RCC_CFGR_SW_PLL;
    while ((RCC->CFGR & RCC_CFGR_SWS) != RCC_CFGR_SWS_PLL)
        ;
#elif (OW_PORT_SYSCLK_MHZ) == 16
    // Raw HSI: the MCU already runs on the internal 16MHz RC after reset —
    // nothing to configure (APB2 stays /1, so TIM1 = HSI = 16MHz)
#elif (OW_PORT_SYSCLK_MHZ) == 8
    // Raw HSE: run SYSCLK directly from the 8MHz crystal, no PLL, so the core
    // clock is the crystal itself and PCLK2 = 8MHz.
    RCC->CR |= RCC_CR_HSEON;
    while (!(RCC->CR & RCC_CR_HSERDY))
        ;
    // Flash latency: 0 wait states (<= 30MHz); caches on for deterministic timing.
    FLASH->ACR = FLASH_ACR_PRFTEN | FLASH_ACR_ICEN | FLASH_ACR_DCEN;
    // APB1/APB2 stay /1, so TIM1 = SYSCLK = HSE = 8MHz.
    RCC->CFGR = (RCC->CFGR & ~RCC_CFGR_SW) | RCC_CFGR_SW_HSE;
    while ((RCC->CFGR & RCC_CFGR_SWS) != RCC_CFGR_SWS_HSE)
        ;
#else
#error "Unsupported OW_PORT_SYSCLK_MHZ for F4: use 168 (8MHz HSE+PLL, F405/F407), 84 (8MHz HSE+PLL, F401), 16 (raw HSI) or 8 (raw HSE)"
#endif
#else /* F1 */
#if (OW_PORT_SYSCLK_MHZ) == 72
    // Enable HSI and HSE oscillators
    RCC->CR = RCC_CR_HSION | RCC_CR_HSEON;
    // Wait for HSE to stabilize - HSERDY is the hardware stabilization
    // indicator, so no fixed delay is required
    while (!(RCC->CR & RCC_CR_HSERDY))
        ;
    // Configure PLL: HSE source, multiply by 9, APB1 prescaler /2
    RCC->CFGR = RCC_CFGR_PLLSRC | RCC_CFGR_PLLMULL9 | RCC_CFGR_PPRE1_DIV2;
    // Enable PLL only after HSE is confirmed stable, so the PLL locks on a
    // valid clock (per RM0008: HSE must be ready before enabling the PLL)
    RCC->CR |= RCC_CR_PLLON;
    // Wait for the PLL to lock
    while (!(RCC->CR & RCC_CR_PLLRDY))
        ;
    // Configure flash latency for 72MHz operation
    FLASH->ACR = FLASH_ACR_PRFTBE | FLASH_ACR_LATENCY_2;
    // Switch system clock to PLL
    RCC->CFGR = RCC_CFGR_PLLSRC | RCC_CFGR_PLLMULL9 | RCC_CFGR_PPRE1_DIV2 | RCC_CFGR_SW_PLL;
    // Wait for system clock switch to PLL
    while ((RCC->CFGR & RCC_CFGR_SWS_PLL) != RCC_CFGR_SWS_PLL)
        ;
    // Disable HSI oscillator
    RCC->CR &= ~RCC_CR_HSION;
#elif (OW_PORT_SYSCLK_MHZ) == 8
    // Raw HSI: the MCU already runs on the internal 8MHz RC after reset —
    // nothing to configure
#else
#error "Unsupported OW_PORT_SYSCLK_MHZ for F1: use 72 (HSE+PLL) or 8 (raw HSI)"
#endif
#endif
}
#endif /* !DS18B20_TEST_HARNESS || OW_PORT_FAMILY_F4 */

#if !defined(DS18B20_TEST_HARNESS)
/**
 * @brief Initialize microcontroller peripherals for UART communication and LED control
 * @note F1: USART1 TX on PA9 (AF push-pull), LED on PC13. F0: same PA9 UART
 *       via MODER/AFR, LED on PA4 (no GPIOC on F030x6). G0: UART TX on
 *       logical PA9 (PA11 pad after the SYSCFG remap, see ow_port_g0.h),
 *       LED on PA4 (no PC13 bonded out on TSSOP20). F4: USART1 TX on PB6
 *       (AF7; the F4DISCOVERY has no USART1-to-ST-LINK route on PA9), LED on
 *       PD12, with an optional OW_UART_USART3 path on PB10.
 */
__STATIC_FORCEINLINE void hardware_init(void) {
#if defined(OW_PORT_FAMILY_F4)
    // Enable AHB1 GPIOA/GPIOD/GPIOB and APB2 USART1 clocks (GPIOA/DMA2/TIM1 clock is
    // enabled by ow_port_init())
    RCC->AHB1ENR |= RCC_AHB1ENR_GPIOAEN | RCC_AHB1ENR_GPIODEN | RCC_AHB1ENR_GPIOBEN;

    // APB bus clocks for the console UART, derived from the active clock
    // config (see configure_system_clock): 168MHz -> APB1 /4 = 42, APB2 /2
    // = 84; 84MHz -> APB1 /2 = 42, APB2 /2 = 42; raw 16/8MHz -> APB1/APB2 /1
    // = SYSCLK.
#define OW_PORT_PCLK1_MHZ ((OW_PORT_SYSCLK_MHZ) == 168 ? 42 : (OW_PORT_SYSCLK_MHZ) == 84 ? 42 \
                                                                                         : (OW_PORT_SYSCLK_MHZ))
#define OW_PORT_PCLK2_MHZ ((OW_PORT_SYSCLK_MHZ) == 168 ? 84 : (OW_PORT_SYSCLK_MHZ) == 84 ? 42 \
                                                                                         : (OW_PORT_SYSCLK_MHZ))

    // STM32F4DISCOVERY: LD4 (green) on PD12, active high (pin -> LED -> GND)
    GPIOD->MODER = (GPIOD->MODER & ~GPIO_MODER_MODER12) | GPIO_MODER_MODER12_0;

    // Optional OW_UART_USART3 selects USART3 TX on PB10 (AF7) instead of
    // USART1 on PB6 (the STM32F4DISCOVERY has no USART1-to-ST-LINK route).
    // USART3 is on APB1.
#if defined(OW_UART_USART3)
    RCC->AHB1ENR |= RCC_AHB1ENR_GPIOBEN;
    RCC->APB1ENR |= RCC_APB1ENR_USART3EN;
    GPIOB->MODER = (GPIOB->MODER & ~GPIO_MODER_MODER10) | GPIO_MODER_MODER10_1;
    GPIOB->OTYPER &= ~GPIO_OTYPER_OT_10;
    GPIOB->AFR[1] = (GPIOB->AFR[1] & ~GPIO_AFRH_AFSEL10) | (7u << GPIO_AFRH_AFSEL10_Pos); /* AF7 = USART3 */
    // PCLK1: APB1 /4 at 168MHz (42MHz), /2 at 84MHz (42MHz), /1 otherwise.
    USART3->BRR = USART_BRR_CALC((OW_PORT_PCLK1_MHZ) * 1000000u, 115200);
    USART3->CR1 = USART_CR1_TE | USART_CR1_UE; // Enable USART3; TX enable only
#else
    // Configure PB6 as alternate function push-pull output (AF7 = USART1_TX).
    // The F4DISCOVERY has no USART1-to-ST-LINK route on PA9, so the console
    // rides USART1 on PB6 (AF7); PA9 stays free.
    RCC->APB2ENR |= RCC_APB2ENR_USART1EN;
    GPIOB->MODER = (GPIOB->MODER & ~GPIO_MODER_MODER6) | GPIO_MODER_MODER6_1;
    GPIOB->OTYPER &= ~GPIO_OTYPER_OT_6;
    GPIOB->AFR[0] = (GPIOB->AFR[0] & ~GPIO_AFRL_AFSEL6) | (7u << GPIO_AFRL_AFSEL6_Pos); /* AF7 = USART1 */
    // USART1 is on APB2: /2 at 168MHz (84MHz) and at 84MHz (42MHz), /1 otherwise.
    USART1->BRR = USART_BRR_CALC((OW_PORT_PCLK2_MHZ) * 1000000u, 115200);
    USART1->CR1 = USART_CR1_TE | USART_CR1_UE; // Enable USART1; TX enable only
#endif
#elif defined(OW_PORT_FAMILY_F0) || defined(OW_PORT_FAMILY_G0)
    // Enable clock for GPIOA and USART1 (G0: GPIO on IOPENR, USART1 on APBENR2)
#if defined(OW_PORT_FAMILY_G0)
    RCC->IOPENR |= RCC_IOPENR_GPIOAEN;
    RCC->APBENR2 |= RCC_APBENR2_USART1EN;
#else
    RCC->AHBENR |= RCC_AHBENR_GPIOAEN;
    RCC->APB2ENR |= RCC_APB2ENR_USART1EN;
#endif

    // Configure PA9 as alternate function push-pull output (F0: AF1, G0: AF1
    // = USART1_TX; on G0 the signal lands on the PA11 pad via SYSCFG remap)
#if defined(OW_PORT_FAMILY_G0)
    GPIOA->MODER = (GPIOA->MODER & ~GPIO_MODER_MODE9) | GPIO_MODER_MODE9_1;
#else
    GPIOA->MODER = (GPIOA->MODER & ~GPIO_MODER_MODER9) | GPIO_MODER_MODER9_1;
#endif
    GPIOA->AFR[1] = (GPIOA->AFR[1] & ~GPIO_AFRH_AFSEL9) | (1u << GPIO_AFRH_AFSEL9_Pos);

    // Configure PA4 as general purpose output for LED control
#if defined(OW_PORT_FAMILY_G0)
    GPIOA->MODER = (GPIOA->MODER & ~GPIO_MODER_MODE4) | GPIO_MODER_MODE4_0;
#else
    GPIOA->MODER = (GPIOA->MODER & ~GPIO_MODER_MODER4) | GPIO_MODER_MODER4_0;
#endif

    // Configure USART1: 115200 baud, 8 data bits, no parity, 1 stop bit, TX only
    USART1->BRR = USART_BRR_CALC((OW_PORT_SYSCLK_MHZ) * 1000000u, 115200); // PCLK = SYSCLK
    USART1->CR1 = USART_CR1_TE | USART_CR1_UE; // Enable USART1; TX enable only
#else
    // Enable clock for GPIOA, USART1, and GPIOC peripherals
    RCC->APB2ENR |= (RCC_APB2ENR_IOPAEN | RCC_APB2ENR_USART1EN | RCC_APB2ENR_IOPCEN);

    // Configure PA9 as alternate function push-pull output, 2MHz speed
    // Clear existing configuration bits
    GPIOA->CRH &= ~(GPIO_CRH_MODE9 | GPIO_CRH_CNF9);
    // Set alternate function push-pull output mode, 2MHz speed
    GPIOA->CRH |= (GPIO_CRH_MODE9_1 | GPIO_CRH_CNF9_1);

    // Configure PC13 as general purpose output, 2MHz speed for LED control
    GPIOC->CRH &= ~(GPIO_CRH_MODE13 | GPIO_CRH_CNF13);
    GPIOC->CRH |= GPIO_CRH_MODE13_1;

    // Configure USART1: 115200 baud, 8 data bits, no parity, 1 stop bit, TX only
    USART1->BRR = USART_BRR_CALC((OW_PORT_SYSCLK_MHZ) * 1000000u, 115200); // PCLK2 = SYSCLK
    USART1->CR1 = USART_CR1_TE | USART_CR1_UE; // Enable USART1; TX enable only
#endif
}

/**
 * @brief Initialize system clock, USART1 TX and the busy LED GPIO
 */
void app_init(void) {
    configure_system_clock();
    hardware_init();
}

/* ---- SysTick-based millisecond clock (application time base) ----
 *
 * The driver measures only when the application asks it to
 * (ds18b20_start_measure()), so the cadence between measurements - and any
 * idle interval - belongs to the application. This tick is that time base:
 * the examples set a deadline in ds18b20_complete() and call
 * ds18b20_start_measure() once app_millis() reaches it, which keeps the whole
 * demo non-blocking (no delay loop, no busy-wait). SysTick runs from HCLK, so
 * the reload is derived from the configured system clock. */

static volatile uint32_t app_tick_ms;
static uint32_t app_tick_step_ms = 1u;

/**
 * @brief SysTick interrupt: advance the millisecond counter
 * @note Also the wake-up source for app_sleep_until(); the handler is
 *       deliberately empty otherwise.
 */
void SysTick_Handler(void) { app_tick_ms += app_tick_step_ms; }

/**
 * @brief Start the millisecond time base
 * @param[in] hz Requested tick frequency in Hz (e.g. 1000 for 1 ms, 10 for 100 ms)
 * @note Call once after app_init(). A low-power demo should use a low rate
 *       (10-100 Hz) so the idle wait between measurements costs few wake-ups.
 *       The counter advances by the *real* tick period, so app_millis() stays
 *       milliseconds at any rate; rates above 1000 Hz are clamped to 1 ms per
 *       tick. SysTick reloads are 24-bit, so on a fast core a very low
 *       requested rate is clamped to the slowest tick the core can produce
 *       (16.8 ms at 168 MHz) - the millisecond counter follows whatever period
 *       is actually programmed.
 */
void app_tick_init(uint32_t hz) {
    uint32_t ticks = ((uint32_t)(OW_PORT_SYSCLK_MHZ) * 1000000u) / hz;
    if (ticks > 0x00FFFFFFu) {
        ticks = 0x00FFFFFFu;
    }
    /* Step the counter by the period the reload actually programs, not by the
     * requested rate: a 24-bit clamp makes the tick slower than asked, and a
     * counter that counts ticks instead of milliseconds would stretch every
     * deadline by the same factor. period = ticks / (SYSCLK in kHz). */
    uint32_t clock_khz = (uint32_t)(OW_PORT_SYSCLK_MHZ) * 1000u;
    app_tick_step_ms = (ticks + clock_khz / 2u) / clock_khz;
    if (app_tick_step_ms == 0u) {
        app_tick_step_ms = 1u; /* > 1 kHz: count every tick as 1 ms */
    }
    app_tick_ms = 0;
    SysTick_Config(ticks - 1u);
}

/**
 * @brief Milliseconds since app_tick_init()
 * @return Free-running millisecond counter (wraps after ~49 days)
 */
uint32_t app_millis(void) { return app_tick_ms; }

/**
 * @brief Sleep in WFE until the given millisecond deadline
 * @param[in] deadline_ms Absolute app_millis() deadline
 * @note Blocking, but idle: the core waits for the SysTick event instead of
 *       spinning, so a long wait between measurements costs almost no
 *       current. Safe to call with a deadline that has already passed.
 */
void app_sleep_until(uint32_t deadline_ms) {
    while ((int32_t)(deadline_ms - app_tick_ms) > 0) {
        __WFE();
    }
}

/**
 * @brief Busy indicator - toggles LED during measurement
 * @param[in] action 0 = idle, non-zero = busy
 * @note Non-blocking LED control using atomic BSRR register operations.
 *       Strong definition overrides the weak one in the DS18B20 driver.
 *       F1: LED on PC13 (active low). F0: LED on PA4 (active low assumed).
 *       F4 (STM32F4DISCOVERY): LD4 green on PD12 (active high).
 */
void ds18b20_busy(unsigned action) {
#if defined(OW_PORT_FAMILY_F0) || defined(OW_PORT_FAMILY_G0)
    if (action) {
        // Turn LED on (PA4 low)
#if defined(OW_PORT_FAMILY_G0)
        GPIOA->BSRR = GPIO_BSRR_BR4;
#else
        GPIOA->BSRR = GPIO_BSRR_BR_4;
#endif
    } else {
        // Turn LED off (PA4 high)
#if defined(OW_PORT_FAMILY_G0)
        GPIOA->BSRR = GPIO_BSRR_BS4;
#else
        GPIOA->BSRR = GPIO_BSRR_BS_4;
#endif
    }
#elif defined(OW_PORT_FAMILY_F4)
    if (action) {
        // Turn LED on (PD12 high)
        GPIOD->BSRR = GPIO_BSRR_BS12;
    } else {
        // Turn LED off (PD12 low)
        GPIOD->BSRR = GPIO_BSRR_BR12;
    }
#else
    if (action) {
        // Turn LED on (PC13 low due to pull-up LED configuration)
        // BSRR BR register: atomic bit reset operation
        GPIOC->BSRR = GPIO_BSRR_BR13;
    } else {
        // Turn LED off (PC13 high)
        // BSRR BS register: atomic bit set operation
        GPIOC->BSRR = GPIO_BSRR_BS13;
    }
#endif
}
#endif /* !DS18B20_TEST_HARNESS */
