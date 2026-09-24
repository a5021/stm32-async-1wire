/**
 * @file main.c
 * @brief Single-sensor example: one DS18B20, Skip ROM addressing
 *
 * Demonstrates the basic measurement flow of the non-blocking driver.
 * The shared platform layer (app.h/app.c) hides the UART, the clock and the
 * application time base.
 *
 * The driver measures only when asked: it runs one conversion + read cycle per
 * ds18b20_start_measure() call and then parks. The pace between cycles is this
 * application's decision, so the demo measures every MEASURE_PERIOD_MS using
 * its own millisecond clock - no driver-side pause timer, no delay loop.
 */

#include "app.h"

/** Time between two measurement cycles (ms) */
#define MEASURE_PERIOD_MS 5000u

/** Deadline (app_millis()) for the next measurement cycle */
static uint32_t next_measure_ms;

/**
 * @brief Weak implementation for DS18B20 measurement completion callback - handles result display
 * @param[in] temp Temperature value in tenths of degrees Celsius, or error code
 * @note Runs at driver IDLE: the deadline set here is what paces the next cycle.
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
    next_measure_ms = app_millis() + MEASURE_PERIOD_MS;
}

/**
 * @brief Main application entry point
 * @note Fully non-blocking: the driver is polled every pass and the next
 *       measurement is requested only when its deadline is reached.
 */
int main(void) {

    app_init(); // System clock, UART and LED GPIO - single setup call
    app_tick_init(1000u); // 1 ms time base for the measurement cadence

    uart_write_str("DS18B20 1_basic starting...\r\n"); // Enqueue startup message

    ds18b20_init(); // Initialize DS18B20 driver (non-blocking)
#if OW_PARASITE_POWER
    ds18b20_set_parasite(1); // Devices are powered over the data line
#endif
    ds18b20_start_measure(); // Request the first measurement cycle

    for (;;) { // Main event loop (non-blocking, cooperative multitasking)

        ds18b20_poll(); // Poll DS18B20 state machine - advances 1-Wire communication state
        if ((int32_t)(app_millis() - next_measure_ms) >= 0) { // Deadline reached: ask for the next cycle
            next_measure_ms += MEASURE_PERIOD_MS;
            ds18b20_start_measure(); // No-op unless the driver is idle
        }
        uart_poll_tx(); // Poll UART transmission - feeds hardware from buffer
        // Other non-blocking tasks can be added here
    }
}
