#include "app.h"
#include "ds18b20.h"

#ifndef DS18B20_SEARCH_MAX_DEVICES
#define DS18B20_SEARCH_MAX_DEVICES 8u
#endif

static uint8_t found_roms[DS18B20_SEARCH_MAX_DEVICES][8];
static uint8_t found_count = 0;
static uint8_t select_index = 0;
static uint8_t search_running = 1;

/** Time between two measurement cycles (ms) */
#define MEASURE_PERIOD_MS 5000u

/** Deadline (app_millis()) for the next measurement cycle */
static uint32_t next_measure_ms;

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
        uart_write_str(" device(s). Measuring each in turn.\r\n");
        select_index = 0;
        ds18b20_select(found_roms[select_index]);
        next_measure_ms = app_millis() + MEASURE_PERIOD_MS;
        ds18b20_start_measure(); // Request the first measurement cycle
    }
}

void ds18b20_complete(int16_t temp) {
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
    /* The driver is parked now: the next cycle starts when our own clock
     * reaches the deadline set below. */
    next_measure_ms = app_millis() + MEASURE_PERIOD_MS;
}

int main(void) {
    app_init();
    app_tick_init(1000u);
    uart_write_str("DS18B20 2_device_search starting...\r\n");
    uart_write_str("Searching 1-Wire bus...\r\n");
    ds18b20_init();
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
            ds18b20_poll();
            if ((int32_t)(app_millis() - next_measure_ms) >= 0) {
                next_measure_ms += MEASURE_PERIOD_MS;
                ds18b20_start_measure();
            }
        }
        uart_poll_tx();
    }
}
