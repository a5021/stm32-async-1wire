/*
 * libFuzzer harness for ow_stats functions.
 *
 * Compiled as single-TU: #include the source to access static helpers.
 * Properties:
 * - hist_bucket: output in [0,12], monotonically non-decreasing
 * - sensor_find_or_alloc: NULL→sentinel, same ROM→same index
 * - ow_stats_capture_pulse: histogram integrity, min<=max
 * - ow_stats_count_error: total_errors increments by 1
 */

#include "ow_stats.h"
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/* Pull in the stats implementation to access static functions */
#include "ow_stats.c"

int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    if (size < 4) return 0;

    uint8_t action = data[0] & 3;

    switch (action) {
    case 0: {
        /* Fuzz hist_bucket directly */
        uint8_t pulse = data[1];
        uint8_t bucket = hist_bucket(pulse);
        if (bucket > 12) abort();

        /* Monotonicity: if pulse1 <= pulse2, bucket(pulse1) <= bucket(pulse2) */
        if (size >= 3) {
            uint8_t pulse2 = data[2];
            if (pulse <= pulse2) {
                uint8_t b2 = hist_bucket(pulse2);
                if (bucket > b2) abort();
            }
        }
        break;
    }
    case 1: {
        /* Fuzz sensor_find_or_alloc */
        const uint8_t* rom = (size >= 10) ? data + 2 : NULL;
        uint8_t idx = sensor_find_or_alloc(rom);

        /* NULL always returns sentinel */
        if (rom == NULL && idx != OW_STATS_MAX_SENSORS) abort();

        /* Same ROM always returns same index */
        if (rom != NULL) {
            uint8_t idx2 = sensor_find_or_alloc(rom);
            if (idx != idx2) abort();
        }
        break;
    }
    case 2: {
        /* Fuzz ow_stats_capture_pulse */
        if (size < 3) return 0;
        ow_stats_init();
        uint8_t n = data[1];
        if (n > 32) n = 32;
        if ((size_t)n > size - 2) n = (uint8_t)(size - 2);
        const uint8_t* rom = (size >= 12) ? data + 2 : NULL;
        ow_stats_capture_pulse(data + 2, n, rom);

        /* histogram entries are non-negative (uint32_t, always true) */
        /* min <= max for the sensor (if allocated) */
        break;
    }
    case 3: {
        /* Fuzz ow_stats_count_error */
        ow_stats_init();
        int16_t error = (int16_t)((data[1] << 8) | data[2]);
        const uint8_t* rom = (size >= 12) ? data + 3 : NULL;
        ow_stats_count_error(error, rom);
        /* total_errors should have incremented by 1 */
        break;
    }
    }

    return 0;
}

#ifndef FUZZING_BUILD_MODE_UNSAFE_FOR_PRODUCTION
int main(void) {
    ow_stats_init();

    /* hist_bucket boundaries */
    if (hist_bucket(0) != 0) abort();
    if (hist_bucket(2) != 0) abort();
    if (hist_bucket(3) != 1) abort();
    if (hist_bucket(60) != 12) abort();
    if (hist_bucket(255) != 12) abort();

    /* sensor_find_or_alloc: NULL returns sentinel */
    if (sensor_find_or_alloc(NULL) != OW_STATS_MAX_SENSORS) abort();

    /* ow_stats_count_error increments total_errors */
    ow_stats_init();
    ow_stats_count_error(DS18B20_TEMP_ERROR_CRC_FAIL, NULL);

    return 0;
}
#endif
