/*
 * libFuzzer harness for the resolution-change state machine.
 *
 * Compiled as single-TU via ds18b20_test_access.c (src/ds18b20.c +
 * src/onewire.c), so the fuzz input drives exactly the same reset +
 * config-write DMA operations the functional tests exercise.
 *
 * The fuzzer bytes select the requested resolution (9..12), whether a
 * device answers the presence reset, and the address mode (Skip ROM vs
 * Match ROM = 40 vs 104 config slots).  The capture source answers every
 * reset operation the state machine schedules:
 *   rcr == 0 -> reset op (idx0 reset pulse, idx1 presence)
 *  the config write never captures, so nothing else is needed.
 *
 * Properties:
 * 1. Termination: every input finishes the change within POLL_GUARD poll
 *    iterations (no livelock).
 * 2. Outcome agrees with the device presence:
 *      present  -> ctx.resolution becomes the requested bits
 *      absent   -> ctx.resolution is left unchanged (default 12)
 * 3. Determinism: the same input reproduces the same outcome.
 */

#include "ds18b20.h"
#include "onewire.h"
#include "hw_model.h"
#include "mock_target.h"
#include <stdint.h>
#include <stdlib.h>

/* Single-TU: compile the driver + shared layer through the accessor TU so
 * the fuzz input drives the exact production state machines. */
#include "../mock/ds18b20_test_access.c"
#include "../mock/ds18b20_test_access.h"

#define POLL_GUARD 5000u

static uint8_t g_bits;
static uint8_t g_presence;
static uint8_t g_address_mode;

static uint16_t res_cap(uint32_t idx) {
    uint8_t rcr = (uint8_t)mock_tim1.RCR;
    if (rcr == 0) {
        if (idx == 0u) return 510u; /* valid reset pulse */
        return g_presence ? 700u : 100u;
    }
    return 0u;
}

static void start_change(void) {
    hw_set_capture_source(res_cap);
    ds18b20_test_set_address_mode(g_address_mode);
    ds18b20_set_resolution(g_bits);
}

static uint8_t run_change(void) {
    uint16_t guard = 0;
    for (;;) {
        if (ds18b20_set_resolution_poll()) {
            break;
        }
        if (mock_tim1.CR1 & TIM_CR1_CEN) {
            hw_run_until_uif(120); /* longest config write = 104 slots */
        }
        if (++guard > POLL_GUARD) {
            abort(); /* property 1: livelock */
        }
    }
    return ds18b20_get_resolution();
}

static void run_case(uint8_t* out_res, uint8_t* out_unchanged) {
    uint8_t res_before = ds18b20_get_resolution();
    uint8_t res_after = run_change();
    *out_res = res_after;
    *out_unchanged = (uint8_t)(res_after == res_before);
}

int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    if (size < 2) {
        return 0;
    }
    g_bits = (uint8_t)(DS18B20_RES_MIN + (data[0] & 0x03u));
    g_presence = (uint8_t)((data[0] >> 2) & 0x01u);
    g_address_mode = (uint8_t)(data[1] & 0x01u);

    /* First run. */
    hw_reset_all();
    ds18b20_test_register_buffers();
    ds18b20_test_reset_ctx();
    ds18b20_test_reset_search();
    ds18b20_test_reset_txn();
    ds18b20_test_reset_resolution();
    start_change();
    uint8_t res_a;
    uint8_t unchanged_a;
    run_case(&res_a, &unchanged_a);

    /* Property 2: outcome agrees with presence. */
    if (g_presence) {
        if (res_a != g_bits) {
            abort();
        }
        if (g_bits != DS18B20_RES_DEFAULT && unchanged_a != 0u) {
            abort(); /* adoption must actually change the resolution */
        }
    } else {
        if (res_a != DS18B20_RES_DEFAULT) {
            abort();
        }
        if (unchanged_a != 1u) {
            abort();
        }
    }

    /* Determinism (property 3): same stream, same outcome. */
    hw_reset_all();
    ds18b20_test_register_buffers();
    ds18b20_test_reset_ctx();
    ds18b20_test_reset_search();
    ds18b20_test_reset_txn();
    ds18b20_test_reset_resolution();
    start_change();
    uint8_t res_b;
    uint8_t unchanged_b;
    run_case(&res_b, &unchanged_b);

    if (res_a != res_b || unchanged_a != unchanged_b) {
        abort(); /* property 3 */
    }
    return 0;
}

#ifndef FUZZING_BUILD_MODE_UNSAFE_FOR_PRODUCTION
int main(void) {
    uint8_t input[2];

    /* Present device: the resolution must adopt the requested bits. */
    input[0] = 0x00; /* bits 9, presence on */
    g_bits = 9u;
    g_presence = 1u;
    g_address_mode = 0u;
    hw_reset_all();
    ds18b20_test_register_buffers();
    ds18b20_test_reset_ctx();
    ds18b20_test_reset_search();
    ds18b20_test_reset_txn();
    ds18b20_test_reset_resolution();
    start_change();
    if (run_change() != 9u) {
        abort();
    }

    /* Absent device: resolution must stay at the default. */
    input[0] = 0x04; /* bits 9, presence off */
    g_presence = 0u;
    hw_reset_all();
    ds18b20_test_register_buffers();
    ds18b20_test_reset_ctx();
    ds18b20_test_reset_search();
    ds18b20_test_reset_txn();
    ds18b20_test_reset_resolution();
    start_change();
    if (run_change() != DS18B20_RES_DEFAULT) {
        abort();
    }
    return 0;
}
#endif