/**
 * @file demo5.c
 * @brief Multi-sensor example with signal statistics collection
 *
 * Builds on the demo2 scan-and-poll architecture but replaces the
 * resolution cycling with a statistics dump: after a configurable
 * number of full measurement rounds (default 100), the accumulated
 * pulse-width histogram, per-sensor min/max, and error counters are
 * printed via UART, then the counters are reset for the next batch.
 *
 * Requires OW_STATS_ENABLE to be defined at build time:
 *   make clean && make OW_TARGET=g0 APP=demo5 EXT="-DOW_STATS_ENABLE"
 */

#include "app.h"
#include "ds18b20.h"
#include "ow_stats.h"

/* ======== Configuration ======== */
#ifndef DS18B20_SEARCH_MAX_DEVICES
#define DS18B20_SEARCH_MAX_DEVICES 8u
#endif

#ifndef STATS_DUMP_INTERVAL
#define STATS_DUMP_INTERVAL 100u
#endif

/* ======== Device table ======== */
static uint8_t found_roms[DS18B20_SEARCH_MAX_DEVICES][8];
static uint8_t found_count  = 0;
static uint8_t select_index = 0;
static uint8_t search_running = 1;

/* ======== Non-blocking stats dump state ======== */
static uint8_t dump_busy = 0;  /**< 1 while ow_stats_dump_poll() is running */

/* ======== Search callback ======== */
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
        uart_write_str(" device(s).\r\n");
        select_index = 0;
        ds18b20_select(found_roms[select_index]);
    }
}

/* ======== Measurement complete callback ======== */
void ds18b20_complete(int16_t temp) {
    /* Print temperature or error */
    for (uint8_t i = 0; i < DS18B20_ROM_BYTES; i++) {
        uart_write_hex(found_roms[select_index][i]);
        if (i != DS18B20_ROM_BYTES - 1) uart_tx_enqueue_byte(' ');
    }
    uart_write_str(": ");

    if (temp == DS18B20_TEMP_ERROR_NO_SENSOR) {
        uart_write_str("no sensor");
    } else if (temp == DS18B20_TEMP_ERROR_CRC_FAIL) {
        uart_write_str("CRC fail");
    } else if (temp == DS18B20_TEMP_ERROR_GENERIC) {
        uart_write_str("error");
    } else {
        int whole = temp / 10;
        int frac  = temp % 10;
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

    /* Round-robin to next device */
    if (found_count > 1) {
        select_index = (uint8_t)((select_index + 1u) % found_count);
        ds18b20_select(found_roms[select_index]);
        if (select_index == 0) {
            uart_write_str("---\r\n");
        }
    }

    /* Stats: tick and dump after STATS_DUMP_INTERVAL full rounds */
    uint16_t cycles = ow_stats_tick();
    if (cycles >= STATS_DUMP_INTERVAL && !dump_busy) {
        ow_stats_dump_start();
        dump_busy = 1;
    }
}

/* ======== Main ======== */
int main(void) {
    app_init();

    uart_write_str("DS18B20 demo5 (stats) starting...\r\n");

    ow_stats_init();
    ds18b20_init();
#if defined(PARASITE_POWER)
    ds18b20_set_parasite(1);
#endif
    ds18b20_search_start(device_found_sink, DS18B20_SEARCH_MAX_DEVICES);

    for (;;) {
        if (dump_busy) {
            if (ow_stats_dump_poll()) {
                dump_busy = 0;
                ow_stats_reset();
            }
        } else if (search_running) {
            if (ds18b20_search_poll()) {
                search_running = 0;
                found_count = ds18b20_search_count();
                report_search_result();
            }
        } else {
            ds18b20_poll();
        }
        if (!dump_busy) uart_poll_tx();
    }
}
