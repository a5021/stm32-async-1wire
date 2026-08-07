/**
 * @file demo2.c
 * @brief Multi-sensor example: startup bus scan + round-robin polling
 * 
 * Demonstrates ds18b20_search_devices() to enumerate every DS18B20 on the
 * bus, then ds18b20_select() to measure each sensor in turn. The shared
 * platform layer (app.h/app.c) hides the UART and clock setup.
 */

#include "app.h"

// ======== Config: maximum devices reported by the startup bus scan ========
#ifndef DS18B20_SEARCH_MAX_DEVICES
#define DS18B20_SEARCH_MAX_DEVICES 8u
#endif

// ======== Selected device state (round-robin measurement) ========
static uint8_t found_roms[DS18B20_SEARCH_MAX_DEVICES][8]; // ROMs found at startup
static uint8_t found_count = 0; // how many devices were found
static uint8_t select_index = 0; // index of the currently selected device

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
static void search_devices_and_report(void) {
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
 * @return Number of characters enqueued
 */
static int print_device_prefix(void) {
    for (uint8_t i = 0; i < 8; i++) {
        uart_write_hex(found_roms[select_index][i]);
        if (i != 7) uart_tx_enqueue_byte(' ');
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
 * @note Implements non-blocking architecture with periodic polling
 */
int main(void) {

    app_init(); // System clock, UART and LED GPIO - single setup call

    uart_write_str("DS18B20 demo starting...\r\n"); // Enqueue startup message
    ds18b20_init(); // Initialize DS18B20 driver (non-blocking)
    search_devices_and_report(); // Blocking one-time bus scan (all sensors)
    uart_tx_flush(); // Block until the startup banner is fully transmitted

    for (;;) { // Main event loop (non-blocking, cooperative multitasking)

        ds18b20_poll(); // Poll DS18B20 state machine - advances 1-Wire communication state
        uart_poll_tx(); // Poll UART transmission - feeds hardware from buffer
        // Other non-blocking tasks can be added here
    }
}
