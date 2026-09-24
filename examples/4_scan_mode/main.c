/**
 * @file main.c
 * @brief Multi-sensor example: simultaneous broadcast conversion (scan mode)
 *
 * Runs the non-blocking device search (ds18b20_search_*) to discover every
 * sensor, then programs the conversion resolution to every sensor with one
 * broadcast Write Scratchpad (ds18b20_set_resolution(), Skip ROM), then
 * ds18b20_scan_start() to convert all sensors at the same time with a single
 * broadcast Convert T (Skip ROM) and read each one back through Match ROM.
 * Programming the resolution first makes the fleet uniform, so scan mode's
 * single conversion wait is correct even if a previous run left a sensor at a
 * different resolution. One conversion wait covers every device; the
 * temperature of each sensor is reported through ds18b20_complete() in
 * device-table order.
 * All low-level bus operations live in the shared 1-Wire layer
 * (onewire.h/onewire.c), and the driver's search/scan state machines build on
 * it; this example only uses the public high-level interface. Everything is
 * non-blocking: the search and the scan advance by one hardware operation per
 * poll call from the main loop. The shared platform layer (app.h/app.c) hides
 * the UART, the clock and the application time base.
 *
 * The driver measures only when asked (ds18b20_start_measure()), so the demo
 * paces the simultaneous-conversion rounds with its own millisecond clock.
 */

#include "app.h"
#include "ds18b20.h"

// ======== Config: maximum devices reported by the startup bus scan ========
#ifndef DS18B20_SEARCH_MAX_DEVICES
#define DS18B20_SEARCH_MAX_DEVICES 8u
#endif

static uint8_t search_running = 1; // 1 until the non-blocking bus scan finishes
static uint8_t cfg_running = 0; // 1 while the broadcast resolution write runs

/** Time between two simultaneous-conversion rounds (ms) */
#define MEASURE_PERIOD_MS 5000u

/** Deadline (app_millis()) for the next round */
static uint32_t next_measure_ms;

// Conversion resolution programmed to every sensor (broadcast) before the
// scan: scan mode assumes a uniform resolution, so the single broadcast
// conversion wait matches the whole fleet. Without this, a sensor left at a
// different resolution by a previous run would be read before its conversion
// completes (a 12-bit sensor after a 9-bit one reads back 85.0).
#ifndef SCAN_RESOLUTION
#define SCAN_RESOLUTION DS18B20_RES_DEFAULT
#endif

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
    // the scan returned to IDLE before the next broadcast Convert T). The
    // driver is parked until the deadline below, so schedule the next round.
    if ((uint16_t)idx + 1u >= ds18b20_device_count()) {
        for (int i = 0; i < line_len; i++) {
            uart_tx_enqueue_byte('-');
        }
        uart_write_str("\r\n");
        next_measure_ms = app_millis() + MEASURE_PERIOD_MS;
    }
}

/**
 * @brief Main application entry point
 * @note Fully non-blocking: the search advances step by step, then scan mode
 *       converts every sensor in parallel and reports each one in turn. Each
 *       round is requested explicitly (ds18b20_start_measure()) once its
 *       application-side deadline is reached.
 */
int main(void) {
    app_init(); // System clock, UART and LED GPIO - single setup call
    app_tick_init(1000u); // 1 ms time base for the round cadence

    uart_write_str("DS18B20 4_scan_mode starting...\r\n"); // Enqueue startup message
    uart_write_str("Searching 1-Wire bus...\r\n"); // Enqueue search banner
    ds18b20_init(); // Initialize DS18B20 driver (non-blocking)
#if OW_PARASITE_POWER
    ds18b20_set_parasite(1); // Devices are powered over the data line
#endif
    ds18b20_search_start(device_found_sink, DS18B20_SEARCH_MAX_DEVICES); // Start scan

    for (;;) { // Main event loop (non-blocking, cooperative multitasking)
        if (search_running) {
            // Advance the non-blocking device search by one hardware operation
            if (ds18b20_search_poll()) {
                search_running = 0;
                uart_write_str("Found ");
                uart_write_int(ds18b20_device_count());
                uart_write_str(" device(s). Simultaneous conversion:\r\n");
                // Program the conversion resolution to every device (broadcast
                // Skip ROM config write) BEFORE converting, so scan mode's
                // single conversion wait matches a uniform resolution.
                ds18b20_scan_start();
                ds18b20_set_resolution(SCAN_RESOLUTION);
                cfg_running = 1;
            }
        } else if (cfg_running) {
            // Advance the non-blocking broadcast configuration write.
            if (ds18b20_set_resolution_poll()) {
                cfg_running = 0;
                // The config write leaves the driver parked: ask for the
                // first simultaneous-conversion round now.
                ds18b20_start_measure();
            }
        } else {
            ds18b20_poll(); // Advance the scan/measurement state machine
            if ((int32_t)(app_millis() - next_measure_ms) >= 0) {
                next_measure_ms += MEASURE_PERIOD_MS;
                ds18b20_start_measure();
            }
        }
        uart_poll_tx(); // Poll UART transmission - feeds hardware from buffer
        // Other non-blocking tasks can be added here
    }
}
