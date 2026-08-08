/**
 * @file demo.c
 * @brief Single-sensor example: one DS18B20, Skip ROM addressing
 * 
 * Demonstrates the basic measurement flow of the non-blocking driver.
 * The shared platform layer (app.h/app.c) hides the UART and clock setup.
 */

#include "app.h"

/**
 * @brief Weak implementation for DS18B20 measurement completion callback - handles result display
 * @param[in] temp Temperature value in tenths of degrees Celsius, or error code
 */
void ds18b20_complete(int16_t temp) {
    if (temp == DS18B20_TEMP_ERROR_NO_SENSOR) { // No sensor detected error - enqueue error message
        uart_write_str("DS18B20 error: no sensor detected.\r\n");
    } else if (temp == DS18B20_TEMP_ERROR_CRC_FAIL) { // CRC check failed error - enqueue error message
        uart_write_str("DS18B20 error: CRC check failed.\r\n");
    } else if (temp == DS18B20_TEMP_ERROR_GENERIC) { // Generic error - enqueue error message
        uart_write_str("DS18B20 error: generic failure.\r\n");
    } else { // Valid temperature reading - format and display
        int whole = temp / 10; // Get whole degrees (temp is in tenths)
        int frac = temp % 10; // Get fractional part (tenths)
        if (frac < 0) frac = -frac; // Ensure fractional part is positive
        uart_write_str("Temperature: ");
        if (whole == 0 && temp < 0) {
            uart_write_str("-0"); // Handle -0.5°C case
        } else {
            uart_write_int(whole); // Display whole part
        }
        uart_write_str("."); // Decimal point
        uart_write_int(frac); // Display fractional part
        uart_write_str(" C"); // Units
        uart_write_str("\r\n"); // And newline
    }
}

/**
 * @brief Main application entry point
 * @note Implements non-blocking architecture with periodic polling
 */
int main(void) {

    app_init(); // System clock, UART and LED GPIO - single setup call

    uart_write_str("DS18B20 demo starting...\r\n"); // Enqueue startup message

    ds18b20_init(); // Initialize DS18B20 driver (non-blocking)

    for (;;) { // Main event loop (non-blocking, cooperative multitasking)

        ds18b20_poll(); // Poll DS18B20 state machine - advances 1-Wire communication state
        uart_poll_tx(); // Poll UART transmission - feeds hardware from buffer
        // Other non-blocking tasks can be added here
    }
}
