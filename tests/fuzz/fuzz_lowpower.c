/*
 * libFuzzer harness for the opt-in low-power long-stage predicate
 * ow_port_long_wait_pending() (reached through its onewire_long_wait_pending()
 * wrapper).
 *
 * The predicate is a pure function of the timer schedule the port programmed:
 *   running (CEN) && unfinished (!UIF) && (RCR+1) * ARR > OW_PORT_LONG_STAGE_US
 * so a low-power application can decide whether a stage is worth sleeping on.
 * It has no stored flag to drift from the schedule, which is exactly what this
 * harness checks against the formula.
 *
 * Input layout (>= 4 bytes):
 *   data[0] bits 0/1 -> CEN / UIF
 *   data[1]          -> RCR (8-bit, as on real TIM1)
 *   data[2..3]       -> ARR (little-endian, 16-bit)
 *
 * Compiled ONLY with -DOW_LOW_POWER=1. The decision is always covered;
 * the sleep itself (ow_port_sleep_until_done()) is additionally covered when
 * the build also defines -DMOCK_WFE_HOOK, which routes the host __WFE() through
 * mock_wfe_set_hook() so this harness can model the TIM1 update event arriving
 * after a fuzz-chosen number of wake-ups. Without that define the host __WFE()
 * is a no-op and a still-running stage would spin the while(!UIF) loop forever,
 * so the sleep path stays out of the build.
 *
 * Properties (each checked with abort()):
 * 1. Result is 0 or 1 and matches the reference formula.
 * 2. Deterministic: the same schedule reads back the same answer.
 * 3. Monotone: with CEN set and UIF clear, extending RCR or ARR never turns
 *    "sleep" off.
 * 4. Not running, or already complete, is never a long wait.
 * 5. (MOCK_WFE_HOOK) The sleep loop wakes on exactly the wake-up that sets UIF
 *    - one __WFE() per wake-up, never spinning past the event, event still set.
 * 6. (MOCK_WFE_HOOK) Deterministic: the same event schedule sleeps the same.
 */

#include "hw_model.h"
#include "mock_target.h"
#include "onewire.h"
/* The engine's own long-stage threshold. The harness asserts against the same
 * constant the port uses, so it needs the private header - the same access
 * the host test suite has (tests/test/test_lowpower.c). */
#include "ow_port.h"
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

static uint8_t lp_expected(uint8_t cen, uint8_t uif, uint8_t rcr, uint16_t arr) {
    if (!cen) {
        return 0u;
    }
    if (uif) {
        return 0u;
    }
    return (uint8_t)(((uint32_t)rcr + 1u) * (uint32_t)arr > OW_PORT_LONG_STAGE_US);
}

static void lp_arm(uint8_t cen, uint8_t uif, uint8_t rcr, uint16_t arr) {
    mock_tim1.CR1 = cen ? TIM_CR1_CEN : 0u;
    mock_tim1.SR = uif ? TIM_SR_UIF : 0u;
    mock_tim1.RCR = rcr;
    mock_tim1.ARR = arr;
}

#ifdef MOCK_WFE_HOOK
/* --- Sleep-loop coverage (host only) ---------------------------------------
 * The host __WFE() is routed through mock_wfe_set_hook(), so we can deliver
 * the TIM1 update event asynchronously: on wake-up `g_wfe_fire_at` the hook
 * sets UIF and the driver's while(!UIF) loop then exits. */
static uint32_t g_wfe_calls;
static uint32_t g_wfe_fire_at;

static void lp_wfe_hook(void) {
    g_wfe_calls++;
    if (g_wfe_calls >= g_wfe_fire_at) {
        hw_tim_update_event(); /* the update event wakes the sleep loop */
    }
}

/* Arm a running, unfinished long stage, sleep until the update event fires on
 * wake-up `fire_at` (>= 1), and return how many __WFE() calls the sleep made. */
static uint32_t lp_sleep_run(uint8_t rcr, uint16_t arr, uint32_t fire_at) {
    lp_arm(1u, 0u, rcr, arr);
    g_wfe_calls = 0;
    g_wfe_fire_at = fire_at;
    mock_wfe_set_hook(lp_wfe_hook);
    onewire_sleep_until_done();
    mock_wfe_set_hook(NULL);
    return g_wfe_calls;
}
#endif

int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    if (size < 4u) {
        return 0;
    }

    uint8_t cen = (uint8_t)(data[0] & 0x01u);
    uint8_t uif = (uint8_t)((data[0] >> 1) & 0x01u);
    uint8_t rcr = data[1];
    uint16_t arr = (uint16_t)(data[2] | ((uint16_t)data[3] << 8));

    lp_arm(cen, uif, rcr, arr);
    uint8_t got = onewire_long_wait_pending();
    uint8_t want = lp_expected(cen, uif, rcr, arr);

    /* Property 1: bounded and equal to the formula. */
    if (got > 1u || got != want) {
        abort();
    }

    /* Property 2: deterministic. */
    lp_arm(cen, uif, rcr, arr);
    if (onewire_long_wait_pending() != got) {
        abort();
    }

    /* Property 4: idle or complete is never a long wait. */
    if ((!cen || uif) && got != 0u) {
        abort();
    }

    /* Property 3: monotone across the schedule when actually waiting. */
    if (cen && !uif) {
        if (rcr < 0xFFu) {
            lp_arm(cen, uif, (uint8_t)(rcr + 1u), arr);
            if (onewire_long_wait_pending() < got) {
                abort();
            }
        }
        if (arr < 0xFFFFu) {
            lp_arm(cen, uif, rcr, (uint16_t)(arr + 1u));
            if (onewire_long_wait_pending() < got) {
                abort();
            }
        }
    }

#ifdef MOCK_WFE_HOOK
    /* Properties 5/6: the sleep loop wakes on exactly the wake-up that sets
     * UIF, and is deterministic. Reuse the schedule bytes; derive the wake-up
     * index from data[0] (the CEN/UIF bits are irrelevant - the sleep arms its
     * own running, unfinished stage). The probe saves/restores the timer it
     * momentarily re-arms, so it leaves no side effect the caller could see. */
    {
        uint32_t save_cr1 = mock_tim1.CR1;
        uint32_t save_sr = mock_tim1.SR;
        uint32_t save_rcr = mock_tim1.RCR;
        uint32_t save_arr = mock_tim1.ARR;
        uint32_t fire_at = 1u + (uint32_t)(data[0] % 64u);
        uint32_t calls = lp_sleep_run(rcr, arr, fire_at);

        if (calls != fire_at) {
            abort(); /* one __WFE() per wake-up, never spinning past the event */
        }
        if ((mock_tim1.SR & TIM_SR_UIF) == 0u) {
            abort(); /* the update event is what woke it, and is still set */
        }
        if (lp_sleep_run(rcr, arr, fire_at) != calls) {
            abort(); /* deterministic */
        }

        mock_tim1.CR1 = save_cr1;
        mock_tim1.SR = save_sr;
        mock_tim1.RCR = (uint8_t)save_rcr;
        mock_tim1.ARR = save_arr;
    }
#endif

    return 0;
}

#ifndef FUZZING_BUILD_MODE_UNSAFE_FOR_PRODUCTION
int main(void) {
    uint8_t in[4];

    /* Idle timer: never a long wait. */
    memset(in, 0, sizeof(in));
    in[0] = 0x00; /* CEN=0 UIF=0 */
    if (LLVMFuzzerTestOneInput(in, sizeof(in)) != 0) abort();
    if (onewire_long_wait_pending() != 0u) abort();

    /* Running, unfinished, short schedule (14 x 70 = 980 <= 1000). */
    memset(in, 0, sizeof(in));
    in[0] = 0x01; /* CEN=1 */
    in[1] = 13; /* RCR = 13 -> 14 slots */
    in[2] = 70; /* ARR = 70 us */
    if (LLVMFuzzerTestOneInput(in, sizeof(in)) != 0) abort();
    if (onewire_long_wait_pending() != 0u) abort();

    /* Running, unfinished, long schedule (15 x 70 = 1050 > 1000). */
    memset(in, 0, sizeof(in));
    in[0] = 0x01;
    in[1] = 14; /* RCR = 14 -> 15 slots */
    in[2] = 70;
    if (LLVMFuzzerTestOneInput(in, sizeof(in)) != 0) abort();
    if (onewire_long_wait_pending() != 1u) abort();

    /* Complete: UIF set beats a long schedule. */
    memset(in, 0, sizeof(in));
    in[0] = 0x03; /* CEN=1 UIF=1 */
    in[1] = 14;
    in[2] = 70;
    if (LLVMFuzzerTestOneInput(in, sizeof(in)) != 0) abort();
    if (onewire_long_wait_pending() != 0u) abort();

    /* Differential walk over the whole schedule space. */
    {
        uint32_t seed = 0x1234ABCDu;
        for (unsigned iter = 0; iter < 20000u; iter++) {
            seed ^= seed << 13;
            seed ^= seed >> 17;
            seed ^= seed << 5;
            in[0] = (uint8_t)(seed >> 24);
            in[1] = (uint8_t)(seed >> 16);
            in[2] = (uint8_t)(seed >> 8);
            in[3] = (uint8_t)seed;
            if (LLVMFuzzerTestOneInput(in, sizeof(in)) != 0) abort();
        }
    }

#ifdef MOCK_WFE_HOOK
    /* Sleep loop: the update event arrives on a chosen wake-up. */
    if (lp_sleep_run(14, 70, 3u) != 3u) abort();
    if ((mock_tim1.SR & TIM_SR_UIF) == 0u) abort();
    if (lp_sleep_run(14, 70, 1u) != 1u) abort(); /* event on the first wake-up */
    if (lp_sleep_run(0, 100, 32u) != 32u) abort(); /* event on a later wake-up */
#endif

    return 0;
}
#endif
