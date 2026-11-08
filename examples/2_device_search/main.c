#include "app.h"
#include "app_profile.h"
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

/** app_millis() value at which the pause after the last result expires */
static uint32_t pause_end_ms;

#if APP_PROFILE
static app_profile_t prof_search;
static app_profile_t prof_search_poll;
static app_profile_t prof_search_empty;
static app_profile_t prof_measure;
static app_profile_t prof_measure_poll;
static app_profile_t prof_measure_empty;
static uint32_t prof_search_start_ms;
static uint32_t prof_cycle_start_ms;
static uint32_t prof_measure_active_ms;
static uint8_t prof_measure_done;
#endif

static void start_measure_profiled(void) {
#if APP_PROFILE
    app_profile_reset(&prof_measure);
    app_profile_reset(&prof_measure_poll);
    app_profile_reset(&prof_measure_empty);
    prof_cycle_start_ms = app_millis();
    prof_measure_done = 0;
#endif
    ds18b20_start_measure();
}

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
        pause_end_ms = app_millis() + MEASURE_PERIOD_MS;
        start_measure_profiled(); // Request the first measurement cycle
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
    /* The conversion has finished: start counting the pause to the next cycle. */
    pause_end_ms = app_millis() + MEASURE_PERIOD_MS;
#if APP_PROFILE
    prof_measure_done = 1;
#endif
}

int main(void) {
    app_init();
    uart_write_str("DS18B20 2_device_search starting...\r\n");
    uart_write_str("Searching 1-Wire bus...\r\n");
    ds18b20_init();
#if OW_PARASITE_POWER
    ds18b20_set_parasite(1);
#endif
#if APP_PROFILE
    (void)app_profile_init();
    uint32_t overhead = app_profile_calibrate();
    prof_search.overhead = overhead;
    prof_search_poll.overhead = overhead;
    prof_search_empty.overhead = overhead;
    prof_measure.overhead = overhead;
    prof_measure_poll.overhead = overhead;
    prof_measure_empty.overhead = overhead;
    app_profile_reset(&prof_search);
    app_profile_reset(&prof_search_poll);
    app_profile_reset(&prof_search_empty);
    app_profile_print_cal(overhead);
    prof_search_start_ms = app_millis();
#endif
    ds18b20_search_start(device_found_sink, DS18B20_SEARCH_MAX_DEVICES);

    for (;;) {
        if (search_running) {
            /* Keep the polled millisecond clock advancing during the search;
             * app_millis() only counts when it is called. */
            (void)app_millis();
#if APP_PROFILE
            app_profile_select(&prof_search);
            uint32_t work_before = prof_search.events;
            uint32_t poll_start = app_profile_now();
#endif
            uint8_t search_done = ds18b20_search_poll();
#if APP_PROFILE
            uint32_t poll_delta = app_profile_take(poll_start);
            app_profile_add(&prof_search_poll, poll_delta);
            if (prof_search.events == work_before) {
                app_profile_add(&prof_search_empty, poll_delta);
            }
#endif
            if (search_done) {
                search_running = 0;
#if APP_PROFILE
                uint32_t search_ms = app_millis() - prof_search_start_ms;
                if (search_ms == 0u) search_ms = 1u;
                app_profile_print_timed("s", &prof_search,
                                        search_ms, search_ms);
                app_profile_print_timed("sp", &prof_search_poll,
                                        search_ms, search_ms);
                app_profile_print_timed("si", &prof_search_empty,
                                        search_ms, search_ms);
#endif
                report_search_result();
            }
        } else {
#if APP_PROFILE
            app_profile_select(&prof_measure);
            uint32_t work_before = prof_measure.events;
            uint32_t poll_start = app_profile_now();
#endif
            ds18b20_poll();
#if APP_PROFILE
            uint32_t poll_delta = app_profile_take(poll_start);
            uint8_t measure_complete = prof_measure_done;
            if (measure_complete) {
                /* End the active phase after the poll that delivered the
                 * completion. The active-time measurement includes the poll
                 * return path. */
                prof_measure_done = 0;
                prof_measure_active_ms = app_millis() - prof_cycle_start_ms;
            }
            app_profile_add(&prof_measure_poll, poll_delta);
            if (prof_measure.events == work_before) {
                app_profile_add(&prof_measure_empty, poll_delta);
            }
            if (measure_complete) {
                app_profile_print_timed(
                    "m", &prof_measure, prof_measure_active_ms,
                    prof_measure_active_ms + MEASURE_PERIOD_MS);
                app_profile_print_timed(
                    "p", &prof_measure_poll, prof_measure_active_ms,
                    prof_measure_active_ms + MEASURE_PERIOD_MS);
                app_profile_print_timed(
                    "i", &prof_measure_empty, prof_measure_active_ms,
                    prof_measure_active_ms + MEASURE_PERIOD_MS);
            }
#endif
            if ((int32_t)(app_millis() - pause_end_ms) >= 0) {
                pause_end_ms = app_millis() + MEASURE_PERIOD_MS;
                start_measure_profiled();
            }
        }
        uart_poll_tx();
    }
}
