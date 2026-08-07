#include "ds18b20.h"
#include "stm32f1xx.h"

// ======== Config: printing buffer size (power of two) ========
#ifndef UART_TX_BUF_SIZE
#define UART_TX_BUF_SIZE 256u // set to 128 or 256 as desired
#endif
// ======== Config: maximum devices reported by the startup bus scan ========
#ifndef DS18B20_SEARCH_MAX_DEVICES
#define DS18B20_SEARCH_MAX_DEVICES 8u
#endif

// ======== Selected device state (round-robin measurement) ========
static uint8_t found_roms[DS18B20_SEARCH_MAX_DEVICES][8]; // ROMs found at startup
static uint8_t found_count = 0; // how many devices were found
static uint8_t select_index = 0; // index of the currently selected device

// Validate that buffer size is a power of two for efficient masking operations
#if (UART_TX_BUF_SIZE & (UART_TX_BUF_SIZE - 1u)) != 0
#error "UART_TX_BUF_SIZE must be a power of two (e.g., 32, 64, 128, 256)."
#endif

// Create mask for buffer index wrapping (power of two optimization)
#define UART_TX_IDX_MASK (UART_TX_BUF_SIZE - 1u)
// Calculate baud rate register value with rounding for accuracy
#define USART_BRR_CALC(PCLK, BAUD) (((PCLK) + ((BAUD) / 2)) / (BAUD))

// ======== USART1 TX ring buffer ========
static uint8_t uart_tx_head = 0; // write index - points to next free slot
static uint8_t uart_tx_tail = 0; // read index - points to oldest data
static uint8_t uart_tx_buf[UART_TX_BUF_SIZE]; // circular buffer for UART transmission

/**
 * @brief Advance UART transmission by at most one byte (non-blocking)
 * @note Must be called periodically to feed UART hardware from the buffer
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
 * @brief Enqueue a single byte into the UART transmit buffer (lossless)
 * @param[in] b Byte to enqueue
 * @note Blocks only while the buffer is full, polling uart_poll_tx() to make
 *       room - a byte is never dropped. With a 256-byte buffer the worst-case
 *       stall at 115200 baud is ~23 ms.
 */
__STATIC_FORCEINLINE void uart_tx_enqueue_byte(uint8_t b) {
    for (;;) {
        uint8_t head = uart_tx_head;
        // Calculate next head position with wrap-around using power-of-two mask
        uint8_t next = (uint8_t)((head + 1u) & UART_TX_IDX_MASK);
        if (next != uart_tx_tail) { // Room is available
            uart_tx_buf[head] = b; // Store byte at current head position
            uart_tx_head = next; // Update head pointer
            return;
        }
        uart_poll_tx(); // Make room by feeding one byte to the UART
    }
}

/**
 * @brief Enqueue an entire null-terminated string (lossless)
 * @param[in] s Null-terminated string to enqueue
 */
__STATIC_FORCEINLINE void uart_write_str(const char* s) {
    while (*s) {
        uart_tx_enqueue_byte((uint8_t)*s);
        s++;
    }
}

/**
 * @brief Blocking drain of the UART TX ring buffer
 * @note Waits until every enqueued byte has been shifted out (TC set).
 *       Call once at startup after the bus-scan report so the banner is
 *       never truncated, regardless of buffer size vs. message length.
 *       At 115200 baud a 256-byte buffer drains in ~23 ms.
 */
__STATIC_FORCEINLINE void uart_tx_flush(void) {
    while (uart_tx_tail != uart_tx_head) {
        uart_poll_tx();
    }
    while (!(USART1->SR & USART_SR_TC)) {
        ;
    }
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
 * @brief Busy indicator - toggles LED during measurement
 * @param[in] action 0 = idle, non-zero = busy
 * @note Non-blocking LED control using atomic BSRR register operations
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

/**
 * @brief Convert integer to string and enqueue for UART transmission
 * @param[in] value Integer value to convert and transmit
 */
__STATIC_FORCEINLINE void uart_write_int(int value) {
    char buf[7]; // enough for -32768 and '\0'
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
            uvalue = value;
        }

        do { // Convert digits from least significant to most significant
            *(--p) = '0' + (uvalue % 10);
            uvalue /= 10;
        } while (uvalue);

        if (is_negative) *(--p) = '-'; // Add negative sign if needed
    }
    uart_write_str(p);
}

/**
 * @brief Enqueue one byte as two uppercase hexadecimal digits (best-effort)
 * @param[in] b Byte to convert and transmit
 */
__STATIC_FORCEINLINE void uart_write_hex(uint8_t b) {
    static const char hex[] = "0123456789ABCDEF";
    uart_tx_enqueue_byte((uint8_t)hex[(b >> 4) & 0x0F]);
    uart_tx_enqueue_byte((uint8_t)hex[b & 0x0F]);
}

/**
 * @brief Device search callback - stores the ROM and prints it in hex
 * @param[in] rom Pointer to the 8-byte ROM address (LSB first)
 * @return 0 to continue the search
 */
static uint8_t device_found_sink(const uint8_t* rom) {
    const uint8_t is_ds18b20 = (rom[0] == DS18B20_FAMILY_CODE);
    if (is_ds18b20 && found_count < DS18B20_SEARCH_MAX_DEVICES) {
        for (uint8_t i = 0; i < 8; i++) {
            found_roms[found_count][i] = rom[i];
        }
        found_count++;
    }
    uart_write_str("  ROM: ");
    for (uint8_t i = 0; i < 8; i++) {
        uart_write_hex(rom[i]);
        if (i != 7) uart_tx_enqueue_byte(' ');
    }
    uart_write_str(is_ds18b20 ? "\r\n" : " (not DS18B20, skipped)\r\n");
    return 0;
}

/**
 * @brief Run the blocking bus scan once at startup and report the result
 * @note The output is enqueued into the UART ring buffer; call uart_tx_flush()
 *       afterwards so the full banner is transmitted before the main loop.
 */
__STATIC_FORCEINLINE void search_devices_and_report(void) {
    uart_write_str("Searching 1-Wire bus...\r\n");
    uint8_t count = ds18b20_search_devices(device_found_sink, DS18B20_SEARCH_MAX_DEVICES);
    if (count == 0) {
        uart_write_str("No devices on the 1-Wire bus.\r\n");
    } else {
        uart_write_str("Found ");
        uart_write_int(count);
        uart_write_str(" device(s).\r\n");
        select_index = 0;
        ds18b20_select(found_roms[select_index]);
        if (count == 1) {
            uart_write_str("Measuring the single device.\r\n");
        } else {
            uart_write_str("Measuring devices in turn.\r\n");
        }
    }
}

/**
 * @brief Print the ROM address of the currently selected device followed by ": "
 */
__STATIC_FORCEINLINE void print_device_prefix(void) {
    for (uint8_t i = 0; i < 8; i++) {
        uart_write_hex(found_roms[select_index][i]);
        if (i != 7) uart_tx_enqueue_byte(' ');
    }
    uart_write_str(": ");
}

/**
 * @brief Weak implementation for DS18B20 measurement completion callback - handles result display
 * @param[in] temp Temperature value in tenths of degrees Celsius, or error code
 */
void ds18b20_complete(int16_t temp) {
    print_device_prefix(); // ROM of the device that was just measured
    if (temp == DS18B20_TEMP_ERROR_NO_SENSOR) { // No sensor detected error - enqueue error message
        uart_write_str("no sensor detected.\r\n");
    } else if (temp == DS18B20_TEMP_ERROR_CRC_FAIL) { // CRC check failed error - enqueue error message
        uart_write_str("CRC check failed.\r\n");
    } else if (temp == DS18B20_TEMP_ERROR_GENERIC) { // Generic error - enqueue error message
        uart_write_str("generic failure.\r\n");
    } else { // Valid temperature reading - format and display
        int whole = temp / 10; // Get whole degrees (temp is in tenths)
        int frac = temp % 10; // Get fractional part (tenths)
        if (frac < 0) frac = -frac; // Ensure fractional part is positive
        if (whole == 0 && temp < 0) {
            uart_write_str("-0"); // Handle -0.5°C case
        } else {
            uart_write_int(whole); // Display whole part
        }
        uart_write_str("."); // Decimal point
        uart_write_int(frac); // Display fractional part
        uart_write_str(" C\r\n"); // Units and newline
    }

    // Round-robin: switch to the next device for the next measurement cycle
    if (found_count > 1) {
        select_index = (uint8_t)((select_index + 1u) % found_count);
        ds18b20_select(found_roms[select_index]);
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
    ds18b20_init(); // Initialize DS18B20 driver (non-blocking)
    search_devices_and_report(); // Blocking one-time bus scan (all sensors)
    uart_tx_flush(); // Block until the startup banner is fully transmitted

    for (;;) { // Main event loop (non-blocking, cooperative multitasking)

        ds18b20_poll(); // Poll DS18B20 state machine - advances 1-Wire communication state
        uart_poll_tx(); // Poll UART transmission - feeds hardware from buffer
        // Other non-blocking tasks can be added here
    }
}
