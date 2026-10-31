/*
 * libFuzzer harness for the DS18B20 measurement state machine
 * (ds18b20_start_measure / ds18b20_scan_start + ds18b20_poll).
 *
 * Compiled as single-TU via ds18b20_test_access.c (src/ds18b20.c +
 * src/onewire.c), so the fuzz input drives exactly the same reset +
 * Convert-T command + conversion wait + reset + Read-Scratchpad DMA
 * operations the functional tests exercise.  The strong callback spies
 * (tests/mock/ds18b20_test_spy.c) record every ds18b20_complete()/busy()
 * call for the oracle comparison below.
 *
 * A single fuzzer byte toggles the mode; the rest of the stream is a
 * response stream consumed exactly as the driver consumes captures:
 *   data[0] & 0x01              -> scan mode (ds18b20_scan_start)
 *   data[0] & 0x02              -> parasite power (ds18b20_set_parasite(1))
 *   data[0] & 0x04              -> single-device Match ROM (ds18b20_select)
 *   ((data[0] >> 3) & 0x03) + 1 -> scan device count 1..4
 * then, for every reset the state machine schedules: one presence bit
 * (bit 0 of the next byte), and for every scratchpad read it schedules:
 * nine scratchpad bytes (LSB-first pulse encoding).
 *
 * The capture source decodes the running operation from mock_tim1.RCR:
 *   rcr == 0  -> reset op  (idx0 reset pulse, idx1 presence)
 *   rcr == 71 -> 9-byte scratchpad read (72 slots)
 *   (command writes and the conversion wait never capture)
 *
 * Properties (each checked with abort() so libFuzzer reports the input):
 * 1. Termination: every cycle finishes within POLL_GUARD poll iterations,
 *    always reporting at least one ds18b20_complete().
 * 2. Reference model: the recorded complete values, indices and the final
 *    ctx.resolution equal a from-scratch model of the driver's DECODE
 *    rules (presence -> NO_SENSOR, Match-ROM all-0xFF -> NO_SENSOR,
 *    reserved bytes -> CRC_FAIL, CRC8 -> temperature and adapted
 *    resolution), reproduced device per device in scan order.
 * 3. Busy discipline: busy(1) issued at START and the final busy(0) at
 *    the end of the cycle (no dangling "measuring" indicator).
 * 4. Determinism: the same input reproduces the same outcome.
 */

#include "ds18b20.h"
#include "hw_model.h"
#include "mock_target.h"
#include "onewire.h"
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/* Single-TU: compile the driver + shared layer through the accessor TU so
 * the fuzz input drives the exact production state machines. */
#include "../mock/ds18b20_test_access.c"
#include "../mock/ds18b20_test_access.h"
#include "../mock/ds18b20_test_spy.h"

#define MAX_SCAN_DEVICES 4u
#define MAX_RESETS (1u + MAX_SCAN_DEVICES) /* START + one CONTINUE per device */
#define MAX_READS MAX_SCAN_DEVICES
#define POLL_GUARD 5000u

static const uint8_t* g_data;
static size_t g_size;
static size_t g_cursor;

static uint8_t g_scan;
static uint8_t g_parasite;
static uint8_t g_addr;
static uint8_t g_dev_count;

/* Capture record, consumed by the reference model in the same order the
 * driver consumed it: presence bit of every reset op, then the raw
 * scratchpad bytes of every read op. */
static uint8_t g_presence[MAX_RESETS];
static uint8_t g_presence_count;
static uint8_t g_scratch[MAX_READS][DS18B20_SCRATCHPAD_LEN];
static uint8_t g_read_count;

static uint8_t next_bit(void) {
    if (g_size == 0u) {
        return 0u;
    }
    uint8_t b = g_data[g_cursor % g_size];
    g_cursor++;
    return (uint8_t)(b & 1u);
}

static uint8_t next_byte(void) {
    if (g_size == 0u) {
        return 0u;
    }
    uint8_t b = g_data[g_cursor % g_size];
    g_cursor++;
    return b;
}

static uint16_t fuzz_cap(uint32_t idx) {
    uint8_t rcr = (uint8_t)mock_tim1.RCR;
    if (rcr == 0) {
        /* Reset op: 2 captures in the single slot. */
        if (idx == 0u) {
            return 500u; /* valid reset pulse */
        }
        uint8_t p = next_bit();
        if (g_presence_count < MAX_RESETS) {
            g_presence[g_presence_count++] = p;
        }
        return p ? 700u : 100u;
    }
    if (rcr == (uint8_t)(DS18B20_SCRATCHPAD_BITS - 1u)) {
        /* 9-byte scratchpad read: 72 slots, one bit each (LSB first). */
        if (idx == 0u) {
            if (g_read_count < MAX_READS) {
                for (uint8_t b = 0; b < DS18B20_SCRATCHPAD_LEN; b++) {
                    g_scratch[g_read_count][b] = next_byte();
                }
                g_read_count++;
            }
        }
        if (idx >= DS18B20_SCRATCHPAD_BITS) {
            return 0u;
        }
        uint8_t sp = (g_read_count > 0u) ? g_scratch[g_read_count - 1u][idx >> 3] : 0u;
        uint8_t bit = (uint8_t)((sp >> (idx & 7u)) & 1u);
        return bit ? (uint16_t)ONEWIRE_ONE_PULSE : (uint16_t)ONEWIRE_ZERO_PULSE;
    }
    return 0u;
}

/* ---- Reference model of the measurement state machine ---- */

typedef struct {
    int16_t values[TEST_SPY_MAX_COMPLETES];
    uint8_t indices[TEST_SPY_MAX_COMPLETES];
    uint8_t count;
    uint8_t resolution;
} measure_model_t;

static void model_setup(measure_model_t* m) {
    memset(m, 0, sizeof(*m));
    m->resolution = DS18B20_RES_DEFAULT;
}

static int16_t model_decode_temperature(const uint8_t* sp) {
    int16_t raw = (int16_t)((sp[1] << 8) | sp[0]);
    return (int16_t)(((int32_t)raw * 10 + ((raw < 0) ? -8 : 8)) / 16);
}

static void model_add(measure_model_t* m, int16_t value, uint8_t index) {
    if (m->count >= TEST_SPY_MAX_COMPLETES) {
        abort();
    }
    m->values[m->count] = value;
    m->indices[m->count] = index;
    m->count++;
}

static void model_run(measure_model_t* m) {
    uint8_t ri = 0; /* reset index consumed */
    uint8_t ri_used = (g_presence_count > 0u) ? g_presence[ri++] : 0u;

    /* START reset presence decides the Convert T broadcast. */
    if (ri_used == 0u) {
        model_add(m, DS18B20_TEMP_ERROR_NO_SENSOR, 0u);
        return;
    }

    uint8_t n = g_scan ? g_dev_count : 1u;
    uint8_t addr_mode = g_scan ? 1u : g_addr;
    uint8_t read_index = 0;

    for (uint8_t d = 0; d < n; d++) {
        uint8_t idx = g_scan ? d : 0u;
        uint8_t p = (ri < g_presence_count) ? g_presence[ri++] : 0u;
        if (p == 0u) {
            /* REQUEST presence fail: report NO_SENSOR and stop (scan aborts
             * the round here). */
            model_add(m, DS18B20_TEMP_ERROR_NO_SENSOR, idx);
            return;
        }
        if (read_index >= g_read_count) {
            abort(); /* the driver read a device the record cannot explain */
        }
        const uint8_t* sp = g_scratch[read_index++];

        /* Match ROM mode: an absent addressed device reads all 0xFF. */
        if (addr_mode != 0u) {
            uint8_t all_ones = 1;
            for (uint8_t i = 0; i < DS18B20_SCRATCHPAD_LEN; i++) {
                if (sp[i] != 0xFF) {
                    all_ones = 0;
                    break;
                }
            }
            if (all_ones) {
                model_add(m, DS18B20_TEMP_ERROR_NO_SENSOR, idx);
                continue;
            }
        }
        if (sp[5] != 0xFF || sp[7] != 0x10) {
            model_add(m, DS18B20_TEMP_ERROR_CRC_FAIL, idx);
            continue;
        }
        if (sp[DS18B20_SCRATCHPAD_LEN - 1] != onewire_crc8(sp, DS18B20_CRC8_BYTES)) {
            model_add(m, DS18B20_TEMP_ERROR_CRC_FAIL, idx);
            continue;
        }
        m->resolution = (uint8_t)(DS18B20_RES_MIN + ((sp[4] >> 5) & 0x3u));
        model_add(m, model_decode_temperature(sp), idx);
    }
}

/* ---- Drive one measurement cycle and record the spy outcome ---- */

typedef struct {
    uint8_t count;
    int16_t values[TEST_SPY_MAX_COMPLETES];
    uint8_t indices[TEST_SPY_MAX_COMPLETES];
    uint8_t resolution;
} measure_result_t;

static void measure_setup(void) {
    hw_reset_all();
    ds18b20_test_register_buffers();
    hw_set_capture_source(fuzz_cap);
    test_spy_reset();
    ds18b20_test_reset_ctx();
    ds18b20_test_reset_search();
    ds18b20_test_reset_txn();
    ds18b20_test_reset_resolution();
    ds18b20_set_parasite(0);
    g_presence_count = 0;
    g_read_count = 0;
    g_cursor = 0;

    if (g_parasite) {
        ds18b20_set_parasite(1);
    }
    if (g_scan) {
        for (uint8_t i = 0; i < g_dev_count; i++) {
            uint8_t rom[DS18B20_ROM_BYTES];
            memset(rom, 0, sizeof(rom));
            rom[0] = DS18B20_FAMILY_CODE;
            ds18b20_test_set_device(i, rom);
        }
        ds18b20_test_set_device_count(g_dev_count);
        ds18b20_scan_start();
    } else {
        if (g_addr != 0u) {
            uint8_t rom[DS18B20_ROM_BYTES];
            memset(rom, 0, sizeof(rom));
            rom[0] = DS18B20_FAMILY_CODE;
            ds18b20_select(rom);
        }
        ds18b20_start_measure();
    }
    /* The mock does not model the EGR=UG write of ow_timer_kick(); the
     * driver arms the START reset on the resulting update event, so supply
     * it as the functional tests do. */
    hw_tim_update_event();
}

static void measure_run(measure_result_t* out) {
    uint16_t guard = 0;
    for (;;) {
        ds18b20_poll();
        if (mock_tim1.CR1 & TIM_CR1_CEN) {
            hw_run_until_uif(120); /* longest op = Match ROM write (80 slots) */
        }
        if (ds18b20_test_get_state() == DS18B20_ST_IDLE &&
            (mock_tim1.CR1 & TIM_CR1_CEN) == 0u && test_spy_complete_count >= 1u) {
            break;
        }
        if (++guard > POLL_GUARD) {
            abort(); /* property 1: livelock */
        }
    }
    out->count = test_spy_complete_count;
    for (uint8_t i = 0; i < out->count; i++) {
        out->values[i] = test_spy_complete_values[i];
        out->indices[i] = test_spy_complete_indices[i];
    }
    out->resolution = ds18b20_get_resolution();
}

static void run_case(measure_result_t* out) {
    measure_setup();
    measure_run(out);

    /* Property 2: match the reference model. */
    measure_model_t model;
    model_setup(&model);
    model_run(&model);
    if (out->count != model.count) {
        abort();
    }
    for (uint8_t i = 0; i < model.count; i++) {
        if (out->values[i] != model.values[i] || out->indices[i] != model.indices[i]) {
            abort();
        }
    }
    if (out->resolution != model.resolution) {
        abort();
    }

    /* Property 3: busy discipline. */
    if (test_spy_busy_calls < 2u || test_spy_busy_last_action != 0u) {
        abort();
    }
}

int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    if (size < 1) {
        return 0;
    }
    g_data = data;
    g_size = size;
    g_cursor = 0;
    g_scan = (uint8_t)((data[0] & 0x01u) != 0u);
    g_parasite = (uint8_t)((data[0] & 0x02u) != 0u);
    g_addr = (uint8_t)((data[0] & 0x04u) != 0u);
    g_dev_count = (uint8_t)(1u + ((data[0] >> 3) & 0x03u));

    measure_result_t a;
    run_case(&a);

    /* Property 4: determinism — same stream, same outcome. */
    measure_result_t b;
    run_case(&b);

    if (a.count != b.count || a.resolution != b.resolution) {
        abort();
    }
    for (uint8_t i = 0; i < a.count; i++) {
        if (a.values[i] != b.values[i] || a.indices[i] != b.indices[i]) {
            abort();
        }
    }
    return 0;
}

#ifndef FUZZING_BUILD_MODE_UNSAFE_FOR_PRODUCTION
int main(void) {
    uint8_t in[64];

    /* Scenario 1: single, Skip ROM, both resets present, valid scratchpad
     * with a 10-bit config -> resolution must adapt away from the 12-bit
     * default (the oracle inside LLVMFuzzerTestOneInput enforces it). */
    memset(in, 0, sizeof(in));
    in[0] = 0x00; /* single, external power, Skip ROM */
    in[1] = 0x01; /* START reset presence on */
    in[2] = 0x01; /* CONTINUE reset presence on */
    {
        uint8_t sp[DS18B20_SCRATCHPAD_LEN];
        memset(sp, 0, sizeof(sp));
        sp[0] = 0x00; /* temp LSB */
        sp[1] = 0x01; /* temp MSB: raw 0x0100 = 16.0 C */
        sp[4] = 0x20; /* config R1:R0 = 01 -> 10 bit */
        sp[5] = 0xFF;
        sp[7] = 0x10;
        sp[8] = onewire_crc8(sp, DS18B20_CRC8_BYTES);
        memcpy(in + 3u, sp, sizeof(sp));
    }
    LLVMFuzzerTestOneInput(in, 3u + DS18B20_SCRATCHPAD_LEN);

    /* Scenario 2: single, Skip ROM, valid reserved bytes but a bad CRC. */
    memset(in, 0, sizeof(in));
    in[0] = 0x00;
    in[1] = 0x01;
    in[2] = 0x01;
    in[3] = 0x00;
    in[4] = 0x01;
    in[5] = 0x00;
    in[6] = 0x00;
    in[7] = 0x7F;
    in[8] = 0xFF;
    in[9] = 0x00;
    in[10] = 0x10;
    in[11] = 0x5A; /* not the CRC */
    LLVMFuzzerTestOneInput(in, 12u);

    /* Scenario 3: single, Match ROM, both resets present, all-0xFF scratchpad
     * -> NO_SENSOR (absent addressed device). */
    memset(in, 0, sizeof(in));
    in[0] = 0x04; /* single, Match ROM */
    in[1] = 0x01;
    in[2] = 0x01;
    memset(in + 3u, 0xFF, DS18B20_SCRATCHPAD_LEN);
    LLVMFuzzerTestOneInput(in, 3u + DS18B20_SCRATCHPAD_LEN);

    /* Scenario 4: single, START reset absent -> immediate NO_SENSOR. */
    memset(in, 0, sizeof(in));
    in[0] = 0x00;
    in[1] = 0x00; /* START reset absent */
    LLVMFuzzerTestOneInput(in, 2u);

    /* Scenario 5: scan, two devices, both present, valid scratchpads with
     * different configs; two completions at indices 0 and 1. */
    memset(in, 0, sizeof(in));
    in[0] = 0x01 | (1u << 3); /* scan, two devices */
    in[1] = 0x01; /* START reset present */
    in[2] = 0x01; /* device 0 present */
    in[3] = 0x01; /* device 1 present */
    for (uint8_t d = 0; d < 2u; d++) {
        uint8_t sp[DS18B20_SCRATCHPAD_LEN];
        memset(sp, 0, sizeof(sp));
        sp[0] = (uint8_t)(d ? 0x00 : 0x80);
        sp[1] = (uint8_t)(d ? 0x02 : 0x00);
        sp[4] = (uint8_t)(d ? 0x40 : 0x00); /* device 1: 11 bit, device 0: 9 bit */
        sp[5] = 0xFF;
        sp[7] = 0x10;
        sp[8] = onewire_crc8(sp, DS18B20_CRC8_BYTES);
        memcpy(in + 4u + (size_t)d * DS18B20_SCRATCHPAD_LEN, sp, sizeof(sp));
    }
    LLVMFuzzerTestOneInput(in, 4u + 2u * DS18B20_SCRATCHPAD_LEN);

    /* Scenario 6: scan, one device whose read reset reports absent. */
    memset(in, 0, sizeof(in));
    in[0] = 0x01; /* scan, one device */
    in[1] = 0x01; /* START reset present */
    in[2] = 0x00; /* device 0 present reset absent */
    LLVMFuzzerTestOneInput(in, 3u);

    /* Scenario 7: single, parasite power, valid scratchpad. */
    memset(in, 0, sizeof(in));
    in[0] = 0x02; /* single, parasite */
    in[1] = 0x01;
    in[2] = 0x01;
    {
        uint8_t sp[DS18B20_SCRATCHPAD_LEN];
        memset(sp, 0, sizeof(sp));
        sp[0] = 0x50;
        sp[1] = 0x00;
        sp[4] = 0x00; /* 9 bit */
        sp[5] = 0xFF;
        sp[7] = 0x10;
        sp[8] = onewire_crc8(sp, DS18B20_CRC8_BYTES);
        memcpy(in + 3u, sp, sizeof(sp));
    }
    LLVMFuzzerTestOneInput(in, 3u + DS18B20_SCRATCHPAD_LEN);

    /* Differential walk: deterministic pseudo-random inputs through the
     * same oracle libFuzzer will use (local smoke only; the fuzz build
     * skips main() entirely). */
    {
        uint32_t seed = 0x12345678u;
        uint8_t buf[48];
        for (unsigned iter = 0; iter < 20000u; iter++) {
            seed ^= seed << 13;
            seed ^= seed >> 17;
            seed ^= seed << 5;
            size_t len = 1u + (size_t)(seed % (uint32_t)sizeof(buf));
            for (size_t i = 0; i < len; i++) {
                seed ^= seed << 13;
                seed ^= seed >> 17;
                seed ^= seed << 5;
                buf[i] = (uint8_t)((seed >> 24) ^ (seed >> 16));
            }
            LLVMFuzzerTestOneInput(buf, len);
        }
    }

    return 0;
}
#endif