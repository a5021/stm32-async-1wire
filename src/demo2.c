/**
 * @file demo2.c
 * @brief Multi-sensor example: startup bus scan + round-robin polling
 *
 * Demonstrates a Maxim 1-Wire Search ROM bus scan using the driver's
 * low-level blocking primitives (ds18b20_reset, ds18b20_write_bit,
 * ds18b20_read_bit, ds18b20_crc8), then ds18b20_select() to measure each
 * sensor in turn. The shared platform layer (app.h/app.c) hides the UART and
 * clock setup.
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
 * @brief Enumerate all DS18B20 devices on the 1-Wire bus (blocking)
 * @param[in] sink Callback invoked once per found DS18B20 device with its
 *                 64-bit ROM address (LSB first). May be NULL to only count
 *                 devices. Return non-zero from the callback to stop the
 *                 search early.
 * @param[in] max_devices Maximum number of devices to report (0 aborts)
 * @return Number of DS18B20 devices found on the bus
 * @note BLOCKING: busy-waits on the timer update flag (~15 ms per device).
 *       Intended for one-time use at startup before the main loop starts
 *       polling. Implements the standard Maxim 1-Wire Search ROM (0xF0)
 *       algorithm with the last-discrepancy method and CRC-8 validation.
 *       Only devices whose ROM family code is DS18B20_FAMILY_CODE (0x28)
 *       are reported; other 1-Wire devices are silently skipped.
 */
static uint8_t search_all_devices(uint8_t (*sink)(const uint8_t* rom), uint8_t max_devices) {
    uint8_t rom[DS18B20_ROM_BYTES] = {0};
    uint8_t found = 0;
    uint8_t last_device = 0;
    uint16_t last_discrepancy = 0;

    if (max_devices == 0) {
        return 0;
    }

    while (!last_device && found < max_devices) {
        // A reset + presence pulse is required before EVERY Search ROM pass:
        // after a completed search transaction the found device is left
        // selected and the other devices go into a "not participating"
        // state, so only a reset brings them all back to search mode.
        if (!ds18b20_reset()) {
            break;
        }

        ds18b20_write_byte(DS18B20_SEARCH_ROM);

        uint16_t id_bit_number = 1;
        uint16_t last_zero = 0;
        for (uint8_t byte_idx = 0; byte_idx < DS18B20_ROM_BYTES; byte_idx++) {
            uint8_t mask = 1;
            for (uint8_t bit_idx = 0; bit_idx < DS18B20_BITS_PER_BYTE; bit_idx++) {
                uint8_t id_bit = ds18b20_read_bit();
                uint8_t cmp_bit = ds18b20_read_bit();
                uint8_t direction;
                if (id_bit && cmp_bit) {
                    // No device follows this path - search tree exhausted
                    ds18b20_restore();
                    return found;
                }
                if (id_bit != cmp_bit) {
                    // Single device on this path - its bit fixes the direction
                    direction = id_bit;
                } else if (id_bit_number < last_discrepancy) {
                    // Follow the previously taken path
                    direction = (rom[byte_idx] & mask) ? 1u : 0u;
                    if (direction == 0) {
                        // Remember the last 0-branch taken at a discrepancy
                        last_zero = id_bit_number;
                    }
                } else {
                    // At the discrepancy point take the '1' branch first
                    direction = (id_bit_number == last_discrepancy) ? 1u : 0u;
                    if (direction == 0) {
                        // Remember the last 0-branch taken at a discrepancy
                        last_zero = id_bit_number;
                    }
                }
                if (direction) {
                    rom[byte_idx] |= mask;
                } else {
                    rom[byte_idx] &= (uint8_t)~mask;
                }
                ds18b20_write_bit(direction);
                id_bit_number++;
                mask <<= 1;
            }
        }
        last_discrepancy = last_zero;

        // Validate the assembled ROM address
        if (ds18b20_crc8(rom, DS18B20_ROM_BYTES) != 0) {
            break;
        }

        // Only report DS18B20 devices; other 1-Wire families share the bus
        // but are not temperature sensors and must not be measured as one.
        if (rom[0] != DS18B20_FAMILY_CODE) {
            if (last_discrepancy == 0) {
                last_device = 1; // This was the last device on the bus
            }
            continue;
        }

        found++;
        if (sink && sink(rom)) {
            break; // Callback requested an early stop
        }
        if (last_discrepancy == 0) {
            last_device = 1; // This was the last device on the bus
        }
    }

    // Restore the non-blocking state machine so the first poll() begins
    ds18b20_restore();
    return found;
}

/**
 * @brief Run the blocking bus scan once at startup and report the result
 * @note The output is enqueued into the UART ring buffer; call uart_tx_flush()
 *       afterwards so the full banner is transmitted before the main loop.
 */
static void search_devices_and_report(void) {
    uart_write_str("Searching 1-Wire bus...\r\n");
    uint8_t count = search_all_devices(device_found_sink, DS18B20_SEARCH_MAX_DEVICES);
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
