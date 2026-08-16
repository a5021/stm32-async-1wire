/**
 * @file demo2.c
 * @brief Multi-sensor example: non-blocking bus scan + round-robin polling
 *
 * Demonstrates a Maxim 1-Wire Search ROM bus scan using the driver's
 * non-blocking device search (ds18b20_search_*), then ds18b20_select() to
 * measure each sensor in turn. It also cycles the conversion resolution
 * (9 -> 10 -> 11 -> 12 bit) between measurements through the non-blocking
 * ds18b20_set_resolution()/ds18b20_set_resolution_poll() API, so the
 * resolution-aware conversion wait is exercised live: at 9-bit a cycle
 * completes ~8x faster than at 12-bit. All low-level bus operations live in
 * the shared 1-Wire layer (onewire.h/onewire.c), and the driver's
 * search/resolution state machines build on it; the example only uses the
 * public high-level interface. Everything is non-blocking: the search and the
 * resolution change advance by one hardware operation per poll call from the
 * main loop. The shared platform layer (app.h/app.c) hides the UART and clock
 * setup.
 */

#include "app.h"
#include "ds18b20.h"

#ifdef DS18B20_TEST_HARNESS
extern void ds18b20_test_set_gap_us(uint16_t us); // [TEST] temporary hook
#endif

// ======== Config: maximum devices reported by the startup bus scan ========
#ifndef DS18B20_SEARCH_MAX_DEVICES
#define DS18B20_SEARCH_MAX_DEVICES 8u
#endif

// ======== Selected device state (round-robin measurement) ========
static uint8_t found_roms[DS18B20_SEARCH_MAX_DEVICES][8]; // ROMs found at startup
static uint8_t found_count = 0; // how many devices were found
static uint8_t select_index = 0; // index of the currently selected device
static uint8_t search_running = 1; // 1 until the non-blocking bus scan finishes

// ======== Non-blocking resolution change (resolution demo) ========
// After every measurement the demo queues the next resolution for the device
// about to be measured. The change runs from the main loop through
// ds18b20_set_resolution()/ds18b20_set_resolution_poll() while the driver's
// own measurement poll is suspended by the ownership guard.
static const uint8_t res_cycle[] = {DS18B20_RES_MIN, 10u, 11u, DS18B20_RES_MAX};
#define RES_CYCLE_LEN (sizeof(res_cycle) / sizeof(res_cycle[0]))
static uint8_t res_cycle_idx = 0; // index into res_cycle[]
static uint8_t next_resolution = 0; // 0 = none pending, else the resolution to apply
static uint8_t res_change_busy = 0; // 1 while ds18b20_set_resolution_poll() is running

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
#ifndef DS18B20_TEST_HARNESS
    uart_write_str("  ROM: ");
    for (uint8_t i = 0; i < DS18B20_ROM_BYTES; i++) {
        uart_write_hex(rom[i]);
        if (i != DS18B20_ROM_BYTES - 1) uart_tx_enqueue_byte(' ');
    }
    uart_write_str("\r\n");
#endif
    return 0;
}

/**
 * @brief Report the bus scan result once the non-blocking search finished
 * @note Non-blocking: only enqueues into the UART TX ring buffer.
 */
#ifndef DS18B20_TEST_HARNESS
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
#endif // !DS18B20_TEST_HARNESS

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
    // 8 bytes * 2 hex chars + 7 separator spaces + ": " (kept in sync with the loop)
    return (int)(DS18B20_ROM_BYTES * 2 + (DS18B20_ROM_BYTES - 1) + 2);
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

    // Resolution demo: queue the next resolution (9 -> 10 -> 11 -> 12 -> 9)
    // for the device that will be measured next. The change itself is applied
    // non-blocking from the main loop before the next measurement cycle.
    res_cycle_idx = (uint8_t)((res_cycle_idx + 1u) % RES_CYCLE_LEN);
    next_resolution = res_cycle[res_cycle_idx];
    uart_write_str("  next resolution: ");
    uart_write_int(next_resolution);
    uart_write_str(" bit\r\n");
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

#ifdef DS18B20_TEST_HARNESS
    // [TEST] Gap sweep: run the non-blocking search repeatedly, inserting a
    // timed idle-HIGH gap after every search slot. For each gap value, K runs
    // are executed and the found-device count is logged, so the DS18B20's
    // tolerance to a delayed next slot (RTOS scenario) can be measured.
    static const uint16_t gap_table[] = {0u, 5u, 10u, 15u, 20u, 25u, 30u, 40u, 50u};
    const uint8_t num_gaps = (uint8_t)(sizeof(gap_table) / sizeof(gap_table[0]));
    const uint8_t runs_per_gap = 100u;
    uint8_t gap_idx = 0;
    uint8_t run = 0;
    ds18b20_init();
    uart_write_str("Search gap sweep:\r\n");
    ds18b20_test_set_gap_us(gap_table[gap_idx]);
    ds18b20_search_start(device_found_sink, DS18B20_SEARCH_MAX_DEVICES);
    for (;;) {
        if (search_running) {
            if (ds18b20_search_poll()) {
                search_running = 0;
                found_count = ds18b20_search_count();
                uart_write_str("GAP=");
                uart_write_int(gap_table[gap_idx]);
                uart_write_str(" run=");
                uart_write_int(run);
                uart_write_str(" found=");
                uart_write_int(found_count);
                uart_write_str("\r\n");
                run++;
                if (run >= runs_per_gap) {
                    run = 0;
                    gap_idx++;
                    if (gap_idx >= num_gaps) {
                        gap_idx = 0; // loop the sweep forever
                    }
                    ds18b20_test_set_gap_us(gap_table[gap_idx]);
                }
                found_count = 0;
                ds18b20_search_start(device_found_sink, DS18B20_SEARCH_MAX_DEVICES);
                search_running = 1;
            }
        }
        uart_poll_tx(); // Poll UART transmission - feeds hardware from buffer
    }
#else
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
            if (res_change_busy) {
                // Advance the non-blocking resolution change by one hardware op
                if (ds18b20_set_resolution_poll()) {
                    res_change_busy = 0;
                    uart_write_str("  resolution applied: ");
                    uart_write_int(ds18b20_get_resolution());
                    uart_write_str(" bit\r\n");
                    // The resolution change forces a timer update event, so the
                    // next ds18b20_poll() begins a measurement immediately,
                    // now with the faster (or slower) conversion wait.
                }
            } else if (next_resolution != 0) {
                // Ownership guard: a no-op unless the driver is IDLE and no
                // search/resolution transaction is running.
                ds18b20_set_resolution(next_resolution);
                next_resolution = 0;
                res_change_busy = 1;
            } else {
                ds18b20_poll(); // Poll DS18B20 state machine - measures devices in turn
            }
        }
        uart_poll_tx(); // Poll UART transmission - feeds hardware from buffer
        // Other non-blocking tasks can be added here
    }
#endif
}
