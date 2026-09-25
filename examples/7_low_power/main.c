/**
 * @file main.c
 * @brief Low-power example: the driver sleeps through its long stages
 *
 * Same search + sequential poll architecture as `2_device_search`. The only
 * difference is the build define below: with `-DOW_PORT_LOW_POWER=1` the driver
 * itself blocks in `__WFE()` while it waits for a long stage of a measurement
 * cycle (temperature conversion up to 750 ms, scratchpad read ~5 ms) instead of
 * being polled through it. The wake-up is the timer's own update event via
 * `SEVONPEND`, so still no NVIC interrupt and no ISR: the application loop is
 * exactly the same as in every other example, and this build contains no
 * interrupt handler at all.
 *
 * Build requirements:
 *   -DOW_PORT_LOW_POWER=1             (this demo's whole point - enables the
 *                                   TIM1 update interrupt source (UIE) +
 *                                   SEVONPEND and the sleep helpers inside
 *                                   ds18b20_poll(); UIE is used only as a WFE
 *                                   wake-up event source)
 *   When the macro is omitted the demo still builds and simply busy-polls
 *   (the sleep path is not compiled without it).
 *
 * Build:
 *   make OW_TARGET=g0 APP=7_low_power EXT="-DOW_PORT_LOW_POWER=1"
 *   On a parasite-powered bus add -DOW_PARASITE_POWER=1.
 */

#include "app.h"
#include "ds18b20.h"
#include "ow_port.h"

#ifndef DS18B20_SEARCH_MAX_DEVICES
#define DS18B20_SEARCH_MAX_DEVICES 8u
#endif

/** Pause between two measurement cycles, counted from the result (ms) */
#define MEASURE_PERIOD_MS 5000u

static uint8_t found_roms[DS18B20_SEARCH_MAX_DEVICES][8];
static uint8_t found_count = 0;
static uint8_t select_index = 0;
static uint8_t search_running = 1;

/** app_millis() value at which the pause after the last result expires */
static uint32_t pause_end_ms;

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

static void report_search_result(void) {
    if (found_count == 0) {
        uart_write_str("No devices on the 1-Wire bus.\r\n");
    } else {
        uart_write_str("Found ");
        uart_write_int(found_count);
        uart_write_str(" device(s). Measuring each in turn (WFE sleep on long stages).\r\n");
        select_index = 0;
        ds18b20_select(found_roms[select_index]);
        pause_end_ms = app_millis() + MEASURE_PERIOD_MS;
        ds18b20_start_measure(); // Request the first measurement cycle
    }
}

void ds18b20_complete(int16_t temp) {
    /* All reporting happens here, in a short window right after the scratchpad
     * read - so the WFE sleep inside the driver's long stages does not starve
     * the UART and no TX bytes are lost. */
    for (uint8_t i = 0; i < DS18B20_ROM_BYTES; i++) {
        uart_write_hex(found_roms[select_index][i]);
        if (i != DS18B20_ROM_BYTES - 1) uart_tx_enqueue_byte(' ');
    }
    uart_write_str(": ");
    if (temp == DS18B20_TEMP_ERROR_NO_SENSOR) {
        uart_write_str("no sensor detected.");
    } else if (temp == DS18B20_TEMP_ERROR_CRC_FAIL) {
        uart_write_str("CRC check failed.");
    } else if (temp == DS18B20_TEMP_ERROR_GENERIC) {
        uart_write_str("generic failure.");
    } else {
        int whole = temp / 10;
        int frac = temp % 10;
        if (frac < 0) frac = -frac;
        if (whole == 0 && temp < 0) {
            uart_write_str("-0");
        } else {
            uart_write_int(whole);
        }
        uart_write_str(".");
        uart_write_int(frac);
        uart_write_str(" C");
    }
    uart_write_str("\r\n");

    if (found_count > 1) {
        select_index = (uint8_t)((select_index + 1u) % found_count);
        ds18b20_select(found_roms[select_index]);
        if (select_index == 0) {
            uart_write_str("--------------------------------\r\n");
        }
    }

    /* The conversion has finished: start counting the pause to the next cycle. */
    pause_end_ms = app_millis() + MEASURE_PERIOD_MS;
}

int main(void) {
    app_init();
    uart_write_str("DS18B20 7_low_power (low power) starting...\r\n");
    uart_write_str("Searching 1-Wire bus...\r\n");
    ds18b20_init();
#if OW_PORT_LOW_POWER
    uart_write_str("OW_PORT_LOW_POWER enabled - WFE sleep on stages > 1ms\r\n");
#else
    uart_write_str("OW_PORT_LOW_POWER disabled - busy-poll only\r\n");
#endif
#if OW_PARASITE_POWER
    ds18b20_set_parasite(1);
#endif
    ds18b20_search_start(device_found_sink, DS18B20_SEARCH_MAX_DEVICES);

    for (;;) {
        if (search_running) {
            if (ds18b20_search_poll()) {
                search_running = 0;
                report_search_result();
            }
        } else {
            /* Same loop as every other example: poll the driver, do other
             * work, and request the next cycle once the pause has passed. With
             * OW_PORT_LOW_POWER=1 the sleep inside a long stage is the
             * driver's own business - nothing to do here. */
            ds18b20_poll();
            if ((int32_t)(app_millis() - pause_end_ms) >= 0) {
                pause_end_ms = app_millis() + MEASURE_PERIOD_MS;
                ds18b20_start_measure();
            }
        }
        uart_poll_tx();
    }
}
