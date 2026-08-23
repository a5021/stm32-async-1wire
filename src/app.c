/**
 * @file app.c
 * @brief Shared application layer implementation (UART TX, system clock, LED)
 */

#include "app.h"
#if defined(OW_PORT_TARGET_F0)
#include "stm32f0xx.h"
#elif defined(OW_PORT_TARGET_G0)
#include "stm32g0xx.h"
#else
#include "stm32f1xx.h"
#endif

// ======== USART1 TX ring buffer ========
static uint8_t uart_tx_head = 0; // write index - points to next free slot
static uint8_t uart_tx_tail = 0; // read index - points to oldest data
static uint8_t uart_tx_buf[UART_TX_BUF_SIZE]; // circular buffer for UART transmission

/**
 * @brief Advance USART1 transmission by at most one byte (non-blocking)
 * @note Must be called periodically to feed the UART from the ring buffer
 */
void uart_poll_tx(void) {
#if defined(OW_PORT_TARGET_G0)
    // G0 uses the modern USART naming: TXE/TXFNF lives in ISR, data in TDR
    if ((USART1->ISR & USART_ISR_TXE_TXFNF) && (uart_tx_tail != uart_tx_head)) {
        uint8_t b = uart_tx_buf[uart_tx_tail];
        uart_tx_tail = (uint8_t)((uart_tx_tail + 1u) & UART_TX_IDX_MASK);
        USART1->TDR = b;
    }
#elif defined(OW_PORT_TARGET_F0)
    // F0 unified USART naming: TXE lives in ISR, data goes to TDR
    if ((USART1->ISR & USART_ISR_TXE) && (uart_tx_tail != uart_tx_head)) {
        // Get byte from buffer at tail position
        uint8_t b = uart_tx_buf[uart_tx_tail];
        // Advance tail pointer with wrap-around
        uart_tx_tail = (uint8_t)((uart_tx_tail + 1u) & UART_TX_IDX_MASK);
        // Write byte to UART data register for transmission
        USART1->TDR = b;
    }
#else
    // Check if UART is ready to transmit (TXE flag set) and buffer not empty
    if ((USART1->SR & USART_SR_TXE) && (uart_tx_tail != uart_tx_head)) {
        // Get byte from buffer at tail position
        uint8_t b = uart_tx_buf[uart_tx_tail];
        // Advance tail pointer with wrap-around
        uart_tx_tail = (uint8_t)((uart_tx_tail + 1u) & UART_TX_IDX_MASK);
        // Write byte to UART data register for transmission
        USART1->DR = b;
    }
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
uint8_t uart_tx_enqueue_byte(uint8_t b) {
    uint8_t head = uart_tx_head;
    // Calculate next head position with wrap-around using power-of-two mask
    uint8_t next = (uint8_t)((head + 1u) & UART_TX_IDX_MASK);
    if (next != uart_tx_tail) { // Room is available
        uart_tx_buf[head] = b; // Store byte at current head position
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
        if (uart_tx_enqueue_byte((uint8_t)*s)) {
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
    return (int)uart_tx_enqueue_byte((uint8_t)hex[(b >> 4) & 0x0F]) +
           (int)uart_tx_enqueue_byte((uint8_t)hex[b & 0x0F]);
}

/**
 * @brief Configure system clock
 * @note The source is derived from OW_PORT_SYSCLK_MHZ (see onewire.h).
 *       F1: 72MHz via HSE+PLL x9, or raw HSI at 8MHz. F030x6 has no HSE:
 *       48MHz via HSI/2+PLL x12, or raw HSI at 8MHz. G031x6 has no HSE:
 *       64MHz via HSI16+PLL (M=1, N=8, R=2), or raw HSI16 at 16MHz.
 */
__STATIC_FORCEINLINE void configure_system_clock(void) {
#if defined(OW_PORT_TARGET_G0)
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
    FLASH->ACR = FLASH_ACR_PRFTEN | FLASH_ACR_LATENCY_1 | FLASH_ACR_LATENCY_0;
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
#elif defined(OW_PORT_TARGET_F0)
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

/**
 * @brief Initialize microcontroller peripherals for UART communication and LED control
 * @note F1: USART1 TX on PA9 (AF push-pull), LED on PC13. F0: same PA9 UART
 *       via MODER/AFR, LED on PA4 (no GPIOC on F030x6). G0: UART TX on
 *       logical PA9 (PA11 pad after the SYSCFG remap, see ow_port_g0.h),
 *       LED on PA4 (no PC13 bonded out on TSSOP20).
 */
__STATIC_FORCEINLINE void hardware_init(void) {
#if defined(OW_PORT_TARGET_F0) || defined(OW_PORT_TARGET_G0)
    // Enable clock for GPIOA and USART1 (G0: GPIO on IOPENR, USART1 on APBENR2)
#if defined(OW_PORT_TARGET_G0)
    RCC->IOPENR |= RCC_IOPENR_GPIOAEN;
    RCC->APBENR2 |= RCC_APBENR2_USART1EN;
#else
    RCC->AHBENR |= RCC_AHBENR_GPIOAEN;
    RCC->APB2ENR |= RCC_APB2ENR_USART1EN;
#endif

    // Configure PA9 as alternate function push-pull output (F0: AF1, G0: AF1
    // = USART1_TX; on G0 the signal lands on the PA11 pad via SYSCFG remap)
#if defined(OW_PORT_TARGET_G0)
    GPIOA->MODER = (GPIOA->MODER & ~GPIO_MODER_MODE9) | GPIO_MODER_MODE9_1;
#else
    GPIOA->MODER = (GPIOA->MODER & ~GPIO_MODER_MODER9) | GPIO_MODER_MODER9_1;
#endif
    GPIOA->AFR[1] = (GPIOA->AFR[1] & ~GPIO_AFRH_AFSEL9) | (1u << GPIO_AFRH_AFSEL9_Pos);

    // Configure PA4 as general purpose output for LED control
#if defined(OW_PORT_TARGET_G0)
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

/**
 * @brief Busy indicator - toggles LED during measurement
 * @param[in] action 0 = idle, non-zero = busy
 * @note Non-blocking LED control using atomic BSRR register operations.
 *       Strong definition overrides the weak one in the DS18B20 driver.
 *       F1: LED on PC13 (active low). F0: LED on PA4 (active low assumed).
 */
void ds18b20_busy(unsigned action) {
#if defined(OW_PORT_TARGET_F0) || defined(OW_PORT_TARGET_G0)
    if (action) {
        // Turn LED on (PA4 low)
#if defined(OW_PORT_TARGET_G0)
        GPIOA->BSRR = GPIO_BSRR_BR4;
#else
        GPIOA->BSRR = GPIO_BSRR_BR_4;
#endif
    } else {
        // Turn LED off (PA4 high)
#if defined(OW_PORT_TARGET_G0)
        GPIOA->BSRR = GPIO_BSRR_BS4;
#else
        GPIOA->BSRR = GPIO_BSRR_BS_4;
#endif
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
