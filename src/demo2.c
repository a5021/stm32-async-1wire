/**
 * @file demo2.c
 * @brief Multi-sensor example: non-blocking bus scan + round-robin polling
 *
 * Demonstrates a Maxim 1-Wire Search ROM bus scan using the driver's
 * non-blocking device search (ds18b20_search_*), then ds18b20_select() to
 * measure each sensor in turn. All low-level bus operations and the search
 * state machine live inside the driver; the example only uses the public
 * high-level interface. Nothing in this example blocks: the search advances
 * by one hardware operation per ds18b20_search_poll() call from the main
 * loop. The shared platform layer (app.h/app.c) hides the UART and clock
 * setup.
 */

#include "app.h"
#include "ds18b20.h"

// ======== Config: maximum devices reported by the startup bus scan ========
#ifndef DS18B20_SEARCH_MAX_DEVICES
#define DS18B20_SEARCH_MAX_DEVICES 8u
#endif

// ======== Selected device state (round-robin measurement) ========
static uint8_t found_roms[DS18B20_SEARCH_MAX_DEVICES][8]; // ROMs found at startup
static uint8_t found_count = 0; // how many devices were found
static uint8_t select_index = 0; // index of the currently selected device
static uint8_t search_running = 1; // 1 until the non-blocking bus scan finishes

/**
 * @brief Device search callback - stores the ROM and prints it in hex
 * @param[in] rom Pointer to the 8-byte ROM address (LSB first)
 * @return 0 to continue the search
 * @note Only DS18B20 devices reach this callback; the search module filters
 *       by family code and enforces the device count limit.
 */
static uint8_t device_found_sink(const uint8_t* rom) {
    for (uint8_t i = 0; i < DS18B20_ROM_BYTES; i++) {
        found_roms[found_count][i] = rom[i];
    }
    found_count++;
    uart_write_str("  ROM: ");
    for (uint8_t i = 0; i < DS18B20_ROM_BYTES; i++) {
        uart_write_hex(rom[i]);
        if (i != DS18B20_ROM_BYTES - 1) uart_tx_enqueue_byte(' ');
    }
    uart_write_str("\r\n");
    return 0;
}

/**
 * @brief Report the bus scan result once the non-blocking search finished
 * @note Non-blocking: only enqueues into the UART TX ring buffer.
 */
static void report_search_result(void) {
    if (found_count == 0) {
        uart_write_str("No devices on the 1-Wire bus.\r\n");
    } else {
        uart_write_str("Found ");
        uart_write_int(found_count);
        uart_write_str(" device(s).\r\n");
        select_index = 0;
        ds18b20_select(found_roms[select_index]);
        if (found_count == 1) {
            uart_write_str("Measuring the single device.\r\n");
        } else {
            uart_write_str("Measuring devices in turn.\r\n");
        }
    }
}

/**
 * @brief Print the ROM address of the currently selected device followed by ": "
 * @return Number of characters enqueued
 */
static int print_device_prefix(void) {
    for (uint8_t i = 0; i < DS18B20_ROM_BYTES; i++) {
        uart_write_hex(found_roms[select_index][i]);
        if (i != DS18B20_ROM_BYTES - 1) uart_tx_enqueue_byte(' ');
    }
    uart_write_str(": ");
    return 25; // 8 bytes * 2 hex chars + 7 spaces + ": "
}

/**
 * @brief Weak implementation for DS18B20 measurement completion callback - handles result display
 * @param[in] temp Temperature value in tenths of degrees Celsius, or error code
 */
void ds18b20_complete(int16_t temp) {
    int line_len = print_device_prefix(); // ROM of the device that was just measured
    if (temp == DS18B20_TEMP_ERROR_NO_SENSOR) { // No sensor detected error - enqueue error message
        line_len += uart_write_str("no sensor detected.");
    } else if (temp == DS18B20_TEMP_ERROR_CRC_FAIL) { // CRC check failed error - enqueue error message
        line_len += uart_write_str("CRC check failed.");
    } else if (temp == DS18B20_TEMP_ERROR_GENERIC) { // Generic error - enqueue error message
        line_len += uart_write_str("generic failure.");
    } else { // Valid temperature reading - format and display
        int whole = temp / 10; // Get whole degrees (temp is in tenths)
        int frac = temp % 10; // Get fractional part (tenths)
        if (frac < 0) frac = -frac; // Ensure fractional part is positive
        if (whole == 0 && temp < 0) {
            line_len += uart_write_str("-0"); // Handle -0.5°C case
        } else {
            line_len += uart_write_int(whole); // Display whole part
        }
        line_len += uart_write_str("."); // Decimal point
        line_len += uart_write_int(frac); // Display fractional part
        line_len += uart_write_str(" C"); // Units
    }
    uart_write_str("\r\n"); // And newline (not counted in line length)

    // Round-robin: switch to the next device for the next measurement cycle
    if (found_count > 1) {
        select_index = (uint8_t)((select_index + 1u) % found_count);
        ds18b20_select(found_roms[select_index]);
        if (select_index == 0) {
            // Every sensor has been measured - close the cycle with a
            // separator as wide as the measurement line
            for (int i = 0; i < line_len; i++) {
                uart_tx_enqueue_byte('-');
            }
            uart_write_str("\r\n");
        }
    }
}

/**
 * @brief Main application entry point
 * @note Implements a fully non-blocking architecture with periodic polling:
 *       first the device search advances step by step, then the measurement
 *       state machine takes over and measures every found sensor in turn.
 */
int main(void) {

    app_init(); // System clock, UART and LED GPIO - single setup call

    uart_write_str("DS18B20 demo starting...\r\n"); // Enqueue startup message
    uart_write_str("Searching 1-Wire bus...\r\n"); // Enqueue search banner
    ds18b20_init(); // Initialize DS18B20 driver (non-blocking)
    ds18b20_search_start(device_found_sink, DS18B20_SEARCH_MAX_DEVICES); // Start scan

    for (;;) { // Main event loop (non-blocking, cooperative multitasking)

        if (search_running) {
            // Advance the non-blocking device search by one hardware operation
            if (ds18b20_search_poll()) {
                search_running = 0;
                found_count = ds18b20_search_count();
                report_search_result();
            }
        } else {
            ds18b20_poll(); // Poll DS18B20 state machine - measures devices in turn
        }
        uart_poll_tx(); // Poll UART transmission - feeds hardware from buffer
        // Other non-blocking tasks can be added here
    }
}
