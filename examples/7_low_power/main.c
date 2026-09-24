/**
 * @file main.c
 * @brief Low-power example: WFE sleep between measurement cycles
 *
 * Same search + sequential poll architecture as `2_device_search`, but the
 * main loop blocks in __WFE() in two places:
 *  - while the driver is running a "long" stage (> 1 ms): the temperature
 *    conversion (up to 750 ms) and the scratchpad read (~5 ms);
 *  - while the demo waits for its own measurement interval, i.e. between
 *    measurement cycles. The driver itself has no interval at all: it parks
 *    after every result, so the app-side SysTick tick both paces the cycles
 *    and wakes the core when the next one is due.
 *
 * Build requirements:
 *   -DOW_PORT_LOW_POWER=1             (this demo's whole point - enables the
 *                                   TIM1 update interrupt source (UIE) +
 *                                   SEVONPEND + the sleep helpers; UIE is used
 *                                   only as a WFE wake-up event source, no
 *                                   NVIC interrupt or ISR is involved)
 *   When the macro is omitted the demo still builds and simply busy-polls
 *   (ow_port_sleep_until_done() and ow_port_long_wait_pending() are not
 *   defined without it, so the sleep path degrades to a plain poll).
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

/** Idle time between two measurement cycles (ms) */
#define MEASURE_PERIOD_MS 5000u

/**
 * @brief SysTick rate of this demo (Hz)
 * @note Deliberately low: every tick is a wake-up, so a fast tick would undo
 *       the point of sleeping between the cycles. 10 Hz gives a ~17 ms time
 *       base on a 168 MHz core (SysTick reload is 24-bit), which is plenty to
 *       time a 5 s interval.
 */
#define APP_TICK_HZ 10u

static uint8_t found_roms[DS18B20_SEARCH_MAX_DEVICES][8];
static uint8_t found_count = 0;
static uint8_t select_index = 0;
static uint8_t search_running = 1;

/** Deadline (app_millis()) for the next measurement cycle */
static uint32_t next_measure_ms;

/** 1 while a measurement cycle is in flight (set on start, cleared on result) */
static uint8_t measure_in_flight = 0;

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
        ds18b20_start_measure(); // Request the first measurement cycle
        measure_in_flight = 1;
    }
}

void ds18b20_complete(int16_t temp) {
    /* All reporting happens here, in a short window right after the scratchpad
     * read, never during a sleeping long stage - so the blocking WFE sleep does
     * not starve the UART and no TX bytes are lost. */
    measure_in_flight = 0;
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

    /* The driver is parked: sleep until our own deadline expires, then ask for
     * the next cycle. Sleeping here (instead of a driver-side pause timer) is
     * what keeps the bus - and the core - idle in between. */
    next_measure_ms = app_millis() + MEASURE_PERIOD_MS;
}

/**
 * @brief Block in WFE while a long 1-Wire stage is still running
 * @note Without OW_PORT_LOW_POWER this compiles to nothing, so the demo still
 *       works as a plain busy-poll loop.
 */
static void low_power_poll(void) {
#if OW_PORT_LOW_POWER
    if (ow_port_long_wait_pending() && !ow_port_bus_done()) {
        ow_port_sleep_until_done();
    }
#else
    (void)0;
#endif
}

int main(void) {
    app_init();
    app_tick_init(APP_TICK_HZ);
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
        } else if (measure_in_flight) {
            /* A cycle is running: poll it and sleep inside its long stages. */
            ds18b20_poll();
            low_power_poll();
        } else {
            /* Driver parked: sleep until the app-side deadline expires, then
             * request the next cycle. Only the SysTick tick wakes the core. */
            ds18b20_poll();
            if ((int32_t)(app_millis() - next_measure_ms) >= 0) {
                measure_in_flight = 1;
                ds18b20_start_measure();
            } else {
                app_sleep_until(next_measure_ms);
            }
        }
        uart_poll_tx();
    }
}
