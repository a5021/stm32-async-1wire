/**
 * @file demo6.c
 * @brief Low-power example: WFE sleep during long 1-Wire stages
 *
 * Same search + sequential poll architecture as demo1, but the main loop
 * blocks in __WFE() while the driver is running a "long" stage (> 1 ms):
 * the temperature conversion (up to 750 ms), the scratchpad read (~5 ms),
 * an EEPROM hold-off (10 ms) and the inter-measurement pause. Short stages
 * (reset, command writes, search reads) stay non-blocking and are polled.
 *
 * Build requirements:
 *   -DOW_PORT_LOW_POWER             (this demo's whole point - enables UIE +
 *                                   SEVONPEND + the sleep helpers)
 *   When the macro is omitted the demo still builds and simply busy-polls
 *   (onewire_sleep_until_done() and onewire_long_wait_pending() are not
 *   defined without it, so the sleep path degrades to a plain poll).
 *
 * Build:
 *   make OW_TARGET=g0 APP=demo6 EXT="-DOW_PORT_LOW_POWER"
 *   On a parasite-powered bus add -DPARASITE_POWER=1.
 */

#include "app.h"
#include "ds18b20.h"
#include "onewire.h"

#ifndef DS18B20_SEARCH_MAX_DEVICES
#define DS18B20_SEARCH_MAX_DEVICES 8u
#endif

static uint8_t found_roms[DS18B20_SEARCH_MAX_DEVICES][8];
static uint8_t found_count = 0;
static uint8_t select_index = 0;
static uint8_t search_running = 1;

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
    }
}

void ds18b20_complete(int16_t temp) {
    /* All reporting happens here, in a short window right after the scratchpad
     * read, never during a sleeping long stage - so the blocking WFE sleep does
     * not starve the UART and no TX bytes are lost. */
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
}

/**
 * @brief Block in WFE while a long 1-Wire stage is still running
 * @note Without OW_PORT_LOW_POWER this compiles to nothing, so the demo still
 *       works as a plain busy-poll loop.
 */
static void low_power_poll(void) {
#ifdef OW_PORT_LOW_POWER
    if (onewire_long_wait_pending() && !onewire_bus_done()) {
        onewire_sleep_until_done();
    }
#else
    (void)0;
#endif
}

int main(void) {
    app_init();
    uart_write_str("DS18B20 demo6 (low power) starting...\r\n");
    uart_write_str("Searching 1-Wire bus...\r\n");
    ds18b20_init();
#ifdef OW_PORT_LOW_POWER
    uart_write_str("OW_PORT_LOW_POWER enabled - WFE sleep on stages > 1ms\r\n");
#else
    uart_write_str("OW_PORT_LOW_POWER NOT defined - busy-poll only\r\n");
#endif
#if defined(PARASITE_POWER)
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
            ds18b20_poll();
            low_power_poll();
        }
        uart_poll_tx();
    }
}
