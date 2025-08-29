#include "ds18b20.h"
#include "stm32f1xx.h"

// ======== Config: printing buffer size (power of two) ========
#ifndef UART_TX_BUF_SIZE
#define UART_TX_BUF_SIZE 128u     // set to 128 or 256 as desired
#endif

// Validate that buffer size is a power of two for efficient masking operations
#if (UART_TX_BUF_SIZE & (UART_TX_BUF_SIZE - 1u)) != 0
#error "UART_TX_BUF_SIZE must be a power of two (e.g., 32, 64, 128, 256)."
#endif

// Create mask for buffer index wrapping (power of two optimization)
#define UART_TX_IDX_MASK (UART_TX_BUF_SIZE - 1u)
// Calculate baud rate register value with rounding for accuracy
#define USART_BRR_CALC(PCLK, BAUD) (((PCLK) + ((BAUD)/2)) / (BAUD))

// ======== USART1 TX ring buffer ========
static uint8_t uart_tx_head = 0;                     // write index - points to next free slot
static uint8_t uart_tx_tail = 0;                     // read index - points to oldest data
static uint8_t uart_tx_buf[UART_TX_BUF_SIZE];        // circular buffer for UART transmission

/**
 * @brief Non-blocking function to enqueue a single byte into the UART transmit buffer
 * @param[in] b Byte to enqueue
 * @return 1 on success, 0 if buffer full
 * @note Returns immediately without blocking
 */
__STATIC_FORCEINLINE int uart_tx_enqueue_byte(uint8_t b) {
    uint8_t head = uart_tx_head;
    // Calculate next head position with wrap-around using power-of-two mask
    uint8_t next = (uint8_t)((head + 1u) & UART_TX_IDX_MASK);
    if (next == uart_tx_tail) { // Check if buffer is full 
        return 0; // Buffer full - non-blocking return
    }

    uart_tx_buf[head] = b; // Store byte at current head position
    uart_tx_head = next;   // Atomically update head pointer
    return 1; // Success
}

/**
 * @brief Non-blocking function to enqueue entire null-terminated string into UART transmit buffer
 * @param[in] s Null-terminated string to enqueue
 * @return Number of characters successfully enqueued
 */
__STATIC_FORCEINLINE int uart_write_str(const char *s) {
    const char *start = s;
    // Process each character until null terminator
    while (*s) {
        // Try to enqueue current character, break on buffer full (non-blocking)
        if (!uart_tx_enqueue_byte((uint8_t)*s)) break;
        s++;
    }
    // Return count of successfully enqueued characters
    return (int)(s - start);
}

/**
 * @brief Non-blocking function to advance UART transmission by at most one byte
 * @note Must be called periodically to feed UART hardware from buffer
 * @note Returns immediately without blocking
 */
__STATIC_FORCEINLINE void uart_poll_tx(void) {
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
 * @brief Configure system clock to 72MHz using PLL
 */
__STATIC_FORCEINLINE void configure_system_clock(void) {
    // Enable HSI and HSE oscillators
    RCC->CR = RCC_CR_HSION | RCC_CR_HSEON;
    // Configure PLL: HSE source, multiply by 9, APB1 prescaler /2
    RCC->CFGR = RCC_CFGR_PLLSRC | RCC_CFGR_PLLMULL9 | RCC_CFGR_PPRE1_DIV2;
    // Enable PLL
    RCC->CR = RCC_CR_HSION | RCC_CR_HSEON | RCC_CR_PLLON;
    // Wait for PLL and HSE ready flags
    while ((RCC_CR_PLLRDY | RCC_CR_HSERDY) != (RCC->CR & (RCC_CR_PLLRDY | RCC_CR_HSERDY)));
    // Configure flash latency for 72MHz operation
    FLASH->ACR = FLASH_ACR_PRFTBE | FLASH_ACR_LATENCY_2;
    // Switch system clock to PLL
    RCC->CFGR = RCC_CFGR_PLLSRC | RCC_CFGR_PLLMULL9 | RCC_CFGR_PPRE1_DIV2 | RCC_CFGR_SW_PLL;
    // Wait for system clock switch to PLL
    while ((RCC->CFGR & RCC_CFGR_SWS_PLL) != RCC_CFGR_SWS_PLL);
    // Disable HSI oscillator
    RCC->CR &= ~RCC_CR_HSION;
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
    GPIOA->CRH |=  (GPIO_CRH_MODE9_1 | GPIO_CRH_CNF9_1);

    // Configure PC13 as general purpose output, 2MHz speed for LED control
    GPIOC->CRH &= ~(GPIO_CRH_MODE13 | GPIO_CRH_CNF13);
    GPIOC->CRH |=  GPIO_CRH_MODE13_1;

    // Configure USART1: 115200 baud, 8 data bits, no parity, 1 stop bit, TX only
    USART1->BRR = USART_BRR_CALC(72000000, 115200); // PCLK2=72MHz
    USART1->CR1 = USART_CR1_TE | USART_CR1_UE;      // Enable USART1; TX enable only
}

/**
 * @brief Weak implementation for DS18B20 LED control - provides visual feedback
 * @param[in] action 0 to turn LED off, non-zero to turn LED on
 * @note Non-blocking LED control using atomic BSRR register operations
 */
void ds18b20_led_control(unsigned action) {
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

/**
 * @brief Convert integer to string and enqueue for UART transmission
 * @param[in] value Integer value to convert and transmit
 */
__STATIC_FORCEINLINE void uart_write_int(int value) {
    char buf[7];       // enough for -32768 and '\0'
    char *p = buf + sizeof(buf) - 1;
    *p = '\0';
    
    if (value == 0) {  // Special case for zero
        *(--p) = '0';
    } else {
        int is_negative = 0;
        unsigned int uvalue = value;
        
        if (value < 0) {  // Handle negative numbers
            is_negative = 1;
            uvalue = -value;
        }
       
        do {  // Convert digits from least significant to most significant
            *(--p) = '0' + (uvalue % 10);
            uvalue /= 10;
        } while (uvalue);
        
        if (is_negative) *(--p) = '-'; // Add negative sign if needed
    }
    (void)uart_write_str(p); // best-effort enqueue to UART buffer
}

/**
 * @brief Weak implementation for DS18B20 temperature ready callback - handles temperature display
 * @param[in] temp Temperature value in tenths of degrees Celsius, or error code
 */
#if defined ELAPSED_TIME
void ds18b20_temp_ready(int16_t temp, uint32_t t) {
#else
void ds18b20_temp_ready(int16_t temp) {
#endif
    if (temp == DS18B20_TEMP_ERROR_NO_SENSOR) {  // No sensor detected error - enqueue error message
        uart_write_str("DS18B20 error: no sensor detected.\r\n");
    } else if (temp == DS18B20_TEMP_ERROR_CRC_FAIL) {  // CRC check failed error - enqueue error message
        uart_write_str("DS18B20 error: CRC check failed.\r\n");
    } else if (temp == DS18B20_TEMP_ERROR_GENERIC) { // Generic error - enqueue error message
        uart_write_str("DS18B20 error: generic failure.\r\n");
    } else {  // Valid temperature reading - format and display
        int whole = temp / 10;      // Get whole degrees (temp is in tenths)
        int frac = temp % 10;       // Get fractional part (tenths)
        if (frac < 0) frac = -frac; // Ensure fractional part is positive
        uart_write_str("Temperature: ");
        uart_write_int(whole);      // Display whole part
        uart_write_str(".");        // Decimal point
        uart_write_int(frac);       // Display fractional part
        uart_write_str(" C");       // Units
        #if defined ELAPSED_TIME
            uart_write_str(" (");   // Parenthesis
            uart_write_int(t / 72); // Display time elapsed
            uart_write_str(" us)"); // Parenthesis
        #endif
        uart_write_str("\r\n");     // And newline
    }
}

/**
 * @brief Main application entry point
 * @note Implements non-blocking architecture with periodic polling
 */
int main(void) {

    configure_system_clock(); // Configure system clock for MCU

    hardware_init(); // Initialize hardware peripherals (non-blocking)
    uart_write_str("DS18B20 demo starting...\r\n"); // Enqueue startup message to UART buffer
    ds18b20_init();  // Initialize DS18B20 driver (non-blocking)
    
    // Optional: Initialize DWT cycle counter for performance measurements
    #if defined ELAPSED_TIME
        CoreDebug->DEMCR |= CoreDebug_DEMCR_TRCENA_Msk;   // Enable trace and debug blocks
        DWT->CYCCNT = 0;                                  // Reset cycle counter
        DWT->CTRL |= DWT_CTRL_CYCCNTENA_Msk;              // Enable cycle counting
    #endif

    for (;;) {          // Main event loop (non-blocking, cooperative multitasking)
        
        ds18b20_poll(); // Poll DS18B20 state machine - advances 1-Wire communication state
        uart_poll_tx(); // Poll UART transmission - feeds hardware from buffer
        // Other non-blocking tasks can be added here
    }
}
