/**
 * @file demo3.c
 * @brief Multi-sensor example: simultaneous broadcast conversion (scan mode)
 *
 * Runs the non-blocking device search (ds18b20_search_*) to discover every
 * sensor, then ds18b20_scan_start() to convert all sensors at the same time
 * with a single broadcast Convert T (Skip ROM) and read each one back through
 * Match ROM. One conversion wait covers every device; the temperature of each
 * sensor is reported through ds18b20_complete() in device-table order.
 * All low-level bus operations live in the shared 1-Wire layer
 * (onewire.h/onewire.c), and the driver's search/scan state machines build on
 * it; this example only uses the public high-level interface. Everything is
 * non-blocking: the search and the scan advance by one hardware operation per
 * poll call from the main loop. The shared platform layer (app.h/app.c) hides
 * the UART and clock setup.
 */

#include "app.h"
#include "ds18b20.h"

// ======== Config: maximum devices reported by the startup bus scan ========
#ifndef DS18B20_SEARCH_MAX_DEVICES
#define DS18B20_SEARCH_MAX_DEVICES 8u
#endif

static uint8_t search_running = 1; // 1 until the non-blocking bus scan finishes

/**
 * @brief Device search callback - prints the ROM in hex
 * @param[in] rom Pointer to the 8-byte ROM address (LSB first)
 * @return 0 to continue the search
 */
static uint8_t device_found_sink(const uint8_t* rom) {
    uart_write_str("  ROM: ");
    for (uint8_t i = 0; i < DS18B20_ROM_BYTES; i++) {
        uart_write_hex(rom[i]);
        if (i != DS18B20_ROM_BYTES - 1) uart_tx_enqueue_byte(' ');
    }
    uart_write_str("\r\n");
    return 0;
}

/**
 * @brief Weak implementation for DS18B20 measurement completion callback
 * @param[in] temp Temperature value in tenths of degrees Celsius, or error code
 * @note In scan mode this is invoked once per device, in device-table order;
 *       ds18b20_scan_index()/ds18b20_device_rom() identify the sensor.
 */
void ds18b20_complete(int16_t temp) {
    const uint8_t idx = ds18b20_scan_index();
    const uint8_t* rom = ds18b20_device_rom(idx);
    int line_len = 0;
    for (uint8_t i = 0; i < DS18B20_ROM_BYTES; i++) {
        line_len += uart_write_hex(rom ? rom[i] : 0u);
        if (i != DS18B20_ROM_BYTES - 1) line_len += uart_tx_enqueue_byte(' ');
    }
    line_len += uart_write_str(": ");
    if (temp == DS18B20_TEMP_ERROR_NO_SENSOR) { // No sensor detected error
        line_len += uart_write_str("no sensor detected.");
    } else if (temp == DS18B20_TEMP_ERROR_CRC_FAIL) { // CRC check failed error
        line_len += uart_write_str("CRC check failed.");
    } else if (temp == DS18B20_TEMP_ERROR_GENERIC) { // Generic error
        line_len += uart_write_str("generic failure.");
    } else { // Valid temperature reading - format and display
        int whole = temp / 10; // Get whole degrees (temp is in tenths)
        int frac = temp % 10; // Get fractional part (tenths)
        if (frac < 0) frac = -frac; // Ensure fractional part is positive
        if (whole == 0 && temp < 0) {
            line_len += uart_write_str("-0"); // Handle -0.5C case
        } else {
            line_len += uart_write_int(whole); // Display whole part
        }
        line_len += uart_write_str("."); // Decimal point
        line_len += uart_write_int(frac); // Display fractional part
        line_len += uart_write_str(" C"); // Units
    }
    uart_write_str("\r\n"); // And newline

    // Close the measurement batch: a separator as wide as the measurement
    // line marks the end of the round (the last device was just reported and
    // the scan returns to IDLE before the next broadcast Convert T).
    if ((uint16_t)idx + 1u >= ds18b20_device_count()) {
        for (int i = 0; i < line_len; i++) {
            uart_tx_enqueue_byte('-');
        }
        uart_write_str("\r\n");
    }
}

/**
 * @brief Main application entry point
 * @note Fully non-blocking: the search advances step by step, then scan mode
 *       converts every sensor in parallel and reports each one in turn.
 */
int main(void) {
    app_init(); // System clock, UART and LED GPIO - single setup call

    uart_write_str("DS18B20 demo3 starting...\r\n"); // Enqueue startup message
    uart_write_str("Searching 1-Wire bus...\r\n"); // Enqueue search banner
    ds18b20_init(); // Initialize DS18B20 driver (non-blocking)
    ds18b20_search_start(device_found_sink, DS18B20_SEARCH_MAX_DEVICES); // Start scan

    for (;;) { // Main event loop (non-blocking, cooperative multitasking)
        if (search_running) {
            // Advance the non-blocking device search by one hardware operation
            if (ds18b20_search_poll()) {
                search_running = 0;
                uart_write_str("Found ");
                uart_write_int(ds18b20_device_count());
                uart_write_str(" device(s). Simultaneous conversion:\r\n");
                ds18b20_scan_start();
            }
        } else {
            ds18b20_poll(); // Advance the scan/measurement state machine
        }
        uart_poll_tx(); // Poll UART transmission - feeds hardware from buffer
        // Other non-blocking tasks can be added here
    }
}
