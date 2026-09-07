/*
 * libFuzzer harness for the Search ROM / Alarm Search state machine.
 *
 * Compiled as single-TU via ds18b20_test_access.c (src/ds18b20.c +
 * src/onewire.c), so the fuzz input drives exactly the same merge
 * (write+read) DMA operations the functional tests exercise.
 *
 * The fuzzer bytes are a header followed by a response stream.  The
 * capture source decodes the running operation from mock_tim1.RCR:
 *   rcr == 0 -> reset op  (idx0 reset pulse, idx1 presence)
 *   rcr == 1 -> first read pair (idx0 id, idx1 cmp)
 *   rcr == 7 -> merged write+read (idx0 write edge ignored, idx1 id, idx2 cmp)
 * and pulls id/cmp/presence decisions from the stream (bit 0 of the byte).
 *
 * Properties:
 * 1. Termination: every input finishes the search within POLL_GUARD poll
 *    iterations (no livelock on hostile response streams).
 * 2. Sink discipline: exactly onewire_search_count() sink calls, never more
 *    than max_devices, never more than MAX_DEVICES.
 * 3. Every reported ROM has a valid CRC8; with the family filter enabled
 *    rom[0] always matches the requested family.
 * 4. Determinism: the same input reproduces the same found set.
 */

#include "ds18b20.h"
#include "onewire.h"
#include "hw_model.h"
#include "mock_target.h"
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/* Single-TU: compile the driver + shared layer through the accessor TU so
 * the fuzz input drives the exact production state machines. */
#include "../mock/ds18b20_test_access.c"
#include "../mock/ds18b20_test_access.h"

#define MAX_DEVICES 4u
#define POLL_GUARD 5000u

static const uint8_t* g_data;
static size_t g_size;
static size_t g_cursor;

static uint8_t g_command;
static uint8_t g_family;
static uint8_t g_max;

static uint8_t g_all[MAX_DEVICES][8];
static uint8_t g_sink_count;

static uint8_t next_bit(void) {
    if (g_size == 0u) {
        return 0u;
    }
    uint8_t b = g_data[g_cursor % g_size];
    g_cursor++;
    return (uint8_t)(b & 1u);
}

static uint16_t pulse_for_bit(uint8_t bit) {
    return bit ? ow_one_pulse_us : ow_zero_pulse_us;
}

static uint16_t fuzz_cap(uint32_t idx) {
    uint8_t rcr = (uint8_t)mock_tim1.RCR;
    if (rcr == 0) {
        if (idx == 0u) return 500u; /* valid reset pulse */
        return next_bit() ? 700u : 0u;
    }
    if (rcr == 1) {
        return (idx == 0u) ? pulse_for_bit(next_bit())
                           : pulse_for_bit(next_bit());
    }
    /* merged write+read: idx0 is the direction-write edge, never decoded */
    if (idx == 0u) return 0u;
    if (idx == 1u) return pulse_for_bit(next_bit());
    if (idx == 2u) return pulse_for_bit(next_bit());
    return 0u;
}

static uint8_t fuzz_sink(const uint8_t* rom) {
    if (g_sink_count >= MAX_DEVICES) {
        abort();
    }
    memcpy(g_all[g_sink_count++], rom, 8);
    return 0;
}

static void start_search(void) {
    g_sink_count = 0;
    g_cursor = 0;
    ds18b20_test_reset_search();
    onewire_search_start(fuzz_sink, g_max, g_command, g_family);
}

static uint8_t run_search(uint8_t(*out)[8]) {
    uint16_t guard = 0;
    for (;;) {
        if (onewire_search_poll()) {
            break;
        }
        if (mock_tim1.CR1 & TIM_CR1_CEN) {
            hw_run_until_uif(100);
        }
        if (++guard > POLL_GUARD) {
            abort(); /* property 1: livelock */
        }
    }

    uint8_t count = onewire_search_count();
    if (count > g_max || count > MAX_DEVICES) {
        abort(); /* property 2 */
    }
    if (count != g_sink_count) {
        abort(); /* property 2 */
    }
    for (uint8_t i = 0u; i < count; i++) {
        if (onewire_crc8(g_all[i], DS18B20_ROM_BYTES) != 0u) {
            abort(); /* property 3 */
        }
        if (g_family != 0u && g_all[i][0] != g_family) {
            abort(); /* property 3 */
        }
        memcpy(out[i], g_all[i], 8);
    }
    return count;
}

int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    if (size < 1) {
        return 0;
    }
    g_data = data;
    g_size = size;
    g_command = (data[0] & 0x01u) ? DS18B20_ALARM_SEARCH : DS18B20_SEARCH_ROM;
    g_family = (data[0] & 0x02u) ? DS18B20_FAMILY_CODE : 0u;
    g_max = (uint8_t)(1u + ((data[0] >> 2) & 0x03u));

    hw_reset_all();
    ds18b20_test_register_buffers();
    hw_set_capture_source(fuzz_cap);
    uint8_t found_a[MAX_DEVICES][8];

    start_search();
    uint8_t n_a = run_search(found_a);

    /* Determinism (property 4): same stream, same found set. */
    hw_reset_all();
    ds18b20_test_register_buffers();
    hw_set_capture_source(fuzz_cap);
    uint8_t found_b[MAX_DEVICES][8];

    start_search();
    uint8_t n_b = run_search(found_b);

    if (n_a != n_b) {
        abort(); /* property 4 */
    }
    for (uint8_t i = 0; i < n_a; i++) {
        for (uint8_t b = 0; b < DS18B20_ROM_BYTES; b++) {
            if (found_a[i][b] != found_b[i][b]) {
                abort(); /* property 4 */
            }
        }
    }
    return 0;
}

#ifndef FUZZING_BUILD_MODE_UNSAFE_FOR_PRODUCTION
int main(void) {
    /* Smoke: single device, no family filter, deterministic pass. */
    uint8_t input[32];
    memset(input, 0, sizeof(input));
    input[0] = 0x01; /* presence on; then a full all-zero bit walk */

    hw_reset_all();
    ds18b20_test_register_buffers();
    hw_set_capture_source(fuzz_cap);
    g_data = input;
    g_size = sizeof(input);
    g_command = DS18B20_SEARCH_ROM;
    g_family = 0u;
    g_max = 1u;

    uint8_t found[1][8];
    start_search();
    run_search(found);
    if (g_sink_count > 1u) {
        abort();
    }
    return 0;
}
#endif