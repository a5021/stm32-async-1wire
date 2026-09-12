#include "app.h"
#include "ds18b20.h"

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
        uart_write_str(" device(s). Measuring each in turn.\r\n");
        select_index = 0;
        ds18b20_select(found_roms[select_index]);
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
}

int main(void) {
    app_init();
    uart_write_str("DS18B20 demo1 starting...\r\n");
    uart_write_str("Searching 1-Wire bus...\r\n");
    ds18b20_init();
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
        }
        uart_poll_tx();
    }
}
