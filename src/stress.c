/**
 * @file stress.c
 * @brief Stability stress test: bus scan + round-robin polling, repeating
 *
 * Runs the same non-blocking flow as demo2 (device search at startup, then
 * round-robin measurement of every found sensor) but re-runs the device search
 * after every full measurement cycle. This exercises the search state machine
 * and the measurement path continuously so the results can be logged over a
 * long window and checked for:
 *   - search cycles that find fewer devices than the physically connected bus
 *     (EXPECTED_DEVICES, 5 on this rig),
 *   - CRC / no-sensor measurement errors,
 *   - temperature values drifting outside a plausible band.
 *
 * Cadence is production-like: one measurement every ~5s (the driver's standard
 * inter-measurement pause), so a full cycle over all devices takes ~25s.
 */

#include "app.h"
#include "ds18b20.h"

// ======== Config: how many devices are physically on the bus ========
#ifndef STRESS_EXPECTED_DEVICES
#define STRESS_EXPECTED_DEVICES 5u
#endif
#ifndef DS18B20_SEARCH_MAX_DEVICES
#define DS18B20_SEARCH_MAX_DEVICES 8u
#endif

// ======== Selected device state (round-robin measurement) ========
static uint8_t found_roms[DS18B20_SEARCH_MAX_DEVICES][8]; // ROMs found by the last search
static uint8_t found_count = 0; // how many devices the last search found
static uint8_t select_index = 0; // index of the currently selected device
static uint8_t search_running = 1; // 1 while a search pass is in progress
static uint8_t restart_search = 0; // 1 once a full measurement cycle finished
static uint8_t measured_in_cycle = 0; // measurements done in the current cycle

// ======== Stability counters (printed in the header of every search) ========
static uint32_t search_number = 0; // total search passes completed
static uint32_t incomplete_searches = 0; // passes that found < EXPECTED
static uint8_t min_found = 255u; // smallest device count seen
static uint8_t max_found = 0u; // largest device count seen
static uint32_t measurement_errors = 0; // CRC / no-sensor errors seen

/**
 * @brief Device search callback - stores the ROM, counts devices
 * @param[in] rom Pointer to the 8-byte ROM address (LSB first)
 * @return 0 to continue the search
 */
static uint8_t device_found_sink(const uint8_t* rom) {
    for (uint8_t i = 0; i < DS18B20_ROM_BYTES; i++) {
        found_roms[found_count][i] = rom[i];
    }
    found_count++;
    return 0;
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
    return 25;
}

/**
 * @brief Report the result of the search pass that just finished
 */
static void report_search(void) {
    search_number++;
    if (found_count < min_found) min_found = found_count;
    if (found_count > max_found) max_found = found_count;

    uart_write_str("\r\n=== SEARCH #");
    uart_write_int(search_number);
    uart_write_str(": ");
    uart_write_int(found_count);
    uart_write_str("/");
    uart_write_int(STRESS_EXPECTED_DEVICES);
    uart_write_str(" devices");
    if (found_count != STRESS_EXPECTED_DEVICES) {
        incomplete_searches++;
        uart_write_str("  <-- INCOMPLETE");
    }
    uart_write_str(" [min=");
    uart_write_int(min_found);
    uart_write_str(" max=");
    uart_write_int(max_found);
    uart_write_str(" incompl=");
    uart_write_int(incomplete_searches);
    uart_write_str(" errs=");
    uart_write_int(measurement_errors);
    uart_write_str("]\r\n");
    for (uint8_t i = 0; i < found_count; i++) {
        uart_write_str("  ROM: ");
        for (uint8_t j = 0; j < DS18B20_ROM_BYTES; j++) {
            uart_write_hex(found_roms[i][j]);
            if (j != DS18B20_ROM_BYTES - 1) uart_tx_enqueue_byte(' ');
        }
        uart_write_str("\r\n");
    }

    measured_in_cycle = 0;
    if (found_count == 0) {
        uart_write_str("No devices on the 1-Wire bus.\r\n");
        restart_search = 1; // nothing to measure - re-search on the next loop
        return;
    }
    select_index = 0;
    ds18b20_select(found_roms[select_index]);
    uart_write_str("Measuring devices in turn.\r\n");
}

/**
 * @brief Weak implementation for DS18B20 measurement completion callback
 * @param[in] temp Temperature value in tenths of degrees Celsius, or error code
 */
void ds18b20_complete(int16_t temp) {
    int line_len = print_device_prefix();
    if (temp == DS18B20_TEMP_ERROR_NO_SENSOR) {
        measurement_errors++;
        line_len += uart_write_str("no sensor detected.");
    } else if (temp == DS18B20_TEMP_ERROR_CRC_FAIL) {
        measurement_errors++;
        line_len += uart_write_str("CRC check failed.");
    } else if (temp == DS18B20_TEMP_ERROR_GENERIC) {
        measurement_errors++;
        line_len += uart_write_str("generic failure.");
    } else {
        int whole = temp / 10;
        int frac = temp % 10;
        if (frac < 0) frac = -frac;
        if (whole == 0 && temp < 0) {
            line_len += uart_write_str("-0");
        } else {
            line_len += uart_write_int(whole);
        }
        line_len += uart_write_str(".");
        line_len += uart_write_int(frac);
        line_len += uart_write_str(" C");
    }
    uart_write_str("\r\n");

    measured_in_cycle++;
    if (found_count > 1) {
        select_index = (uint8_t)((select_index + 1u) % found_count);
        ds18b20_select(found_roms[select_index]);
    }
    if (found_count > 0 && measured_in_cycle >= found_count) {
        // Full cycle over every found device finished - re-run the search.
        for (int i = 0; i < line_len; i++) {
            uart_tx_enqueue_byte('-');
        }
        uart_write_str("\r\n");
        restart_search = 1;
    }
}

/**
 * @brief Main application entry point
 * @note Non-blocking: search polls and measurement polls alternate from the
 *       main loop; the search is re-run after every full measurement cycle.
 */
int main(void) {
    app_init();
    uart_write_str("DS18B20 stress demo starting...\r\n");
    uart_write_str("Searching 1-Wire bus...\r\n");
    ds18b20_init();
    ds18b20_search_start(device_found_sink, DS18B20_SEARCH_MAX_DEVICES);

    for (;;) {
        if (search_running) {
            // Advance the non-blocking device search by one hardware operation
            if (ds18b20_search_poll()) {
                search_running = 0;
                found_count = ds18b20_search_count();
                report_search();
            }
        } else if (restart_search) {
            // A full measurement cycle finished: re-run the search.
            restart_search = 0;
            found_count = 0;
            uart_write_str("Searching 1-Wire bus...\r\n");
            ds18b20_search_start(device_found_sink, DS18B20_SEARCH_MAX_DEVICES);
            search_running = 1;
        } else {
            ds18b20_poll(); // Poll DS18B20 state machine - measures devices in turn
        }
        uart_poll_tx(); // Poll UART transmission - feeds hardware from buffer
    }
}
