/**
 * @file app.c
 * @brief Shared application layer implementation (UART TX, system clock, LED)
 */

#include "app.h"
#include "stm32f1xx.h"

// ======== USART1 TX ring buffer ========
static uint8_t uart_tx_head = 0; // write index - points to next free slot
static uint8_t uart_tx_tail = 0; // read index - points to oldest data
static uint8_t uart_tx_buf[UART_TX_BUF_SIZE]; // circular buffer for UART transmission

/**
 * @brief Advance USART1 transmission by at most one byte (non-blocking)
 * @note Must be called periodically to feed the UART from the ring buffer
 */
void uart_poll_tx(void) {
    // Check if UART is ready to transmit (TXE flag set) and buffer not empty
    if ((USART1->SR & USART_SR_TXE) && (uart_tx_tail != uart_tx_head)) {
        // Get byte from buffer at tail position
        uint8_t b = uart_tx_buf[uart_tx_tail];
        // Advance tail pointer with wrap-around
        uart_tx_tail = (uint8_t)((uart_tx_tail + 1u) & UART_TX_IDX_MASK);
        // Write byte to UART data register for transmission
        USART1->DR = b;
    }
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
 * @brief Configure system clock (72MHz via HSE+PLL, or skip for HSI 8MHz)
 */
__STATIC_FORCEINLINE void configure_system_clock(void) {
#ifndef HSI_8MHZ
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
#endif
    // HSI_8MHZ: MCU already runs on HSI 8MHz after reset — nothing to configure
}

/**
 * @brief Initialize microcontroller peripherals for UART communication and LED control
 */
__STATIC_FORCEINLINE void hardware_init(void) {

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
#ifdef HSI_8MHZ
    USART1->BRR = USART_BRR_CALC(8000000, 115200); // PCLK2=8MHz
#else
    USART1->BRR = USART_BRR_CALC(72000000, 115200); // PCLK2=72MHz
#endif
    USART1->CR1 = USART_CR1_TE | USART_CR1_UE; // Enable USART1; TX enable only
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
 */
void ds18b20_busy(unsigned action) {
    if (action) {
        // Turn LED on (PC13 low due to pull-up LED configuration)
        // BSRR BR register: atomic bit reset operation
        GPIOC->BSRR = GPIO_BSRR_BR13;
    } else {
        // Turn LED off (PC13 high)
        // BSRR BS register: atomic bit set operation
        GPIOC->BSRR = GPIO_BSRR_BS13;
    }
}
