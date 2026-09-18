/* ============================================================
 *  test_tim_model.c - TIM1/DMA temporal-contract tests
 *
 *  hw_run_until_uif() fires the CC2 feed DMA at each slot START
 *  ("modeled at slot start for simplicity"), which is enough to
 *  prove the memory-side DMA contract. Real hardware is different:
 *  the slot-end marker compare (CC2, at ONE+ZERO us) triggers the
 *  DMA request, so CCR3 reload for slot N+1 happens only AFTER
 *  slot N's pulse has completed. The temporal stepper hw_tim_step()
 *  places each event at its physical counter position so these
 *  tests prove:
 *
 *    - CCR3(slot N) stays in effect for the whole of slot N;
 *    - the CCR3 reload happens only after the CC2 compare, never
 *      at the slot start;
 *    - the trailing bus-release zero is applied only after the
 *      last slot's pulse has finished.
 *    - CC4 capture fires at the pulse-edge counter position.
 *    - OC3PE preload keeps the active output stable while the
 *      preload register is already zero.
 * ============================================================ */

#include "hw_model.h"
#include "mock_target.h"
#include "onewire_internal.h"
#include "unity.h"

#define ONE ONEWIRE_ONE_PULSE
#define ZERO ONEWIRE_ZERO_PULSE
#define SLOT_ARR (ONEWIRE_ONE_PULSE + ONEWIRE_ZERO_PULSE + ONEWIRE_GUARD_BAND)
#define MARKER (ONEWIRE_ONE_PULSE + ONEWIRE_ZERO_PULSE)

/* ============================================================
 *  1. CCR3 reload happens only after CC2, never at slot start
 * ============================================================ */

void test_tim_reload_happens_only_after_cc2(void) {
    static uint8_t buf[4] = {(uint8_t)ONE, ZERO, ONE, 0u};
    hw_register_buf(&buf[1]); /* the driver feeds CCR3 from &cmd[1] */
    onewire_write_pulses(buf, 3);
    TEST_ASSERT_EQUAL_UINT32(3u, mock_feed_ch.CNDTR);
    hw_tim_init();

    /* --- slot 0: ONE (5 us) active from tick 0 --- */
    TEST_ASSERT_EQUAL_UINT32(0u, hw_tim_period());
    TEST_ASSERT_EQUAL_UINT32(0u, hw_tim_tick());
    TEST_ASSERT_EQUAL_UINT16(ONE, hw_tim_active());
    TEST_ASSERT_EQUAL_UINT16(ONE, hw_tim_ccr3());

    /* CC2 at tick 65: compare fires, but no reload yet */
    TEST_ASSERT_EQUAL_INT(HW_TIM_EV_CC2, hw_tim_step());
    TEST_ASSERT_EQUAL_UINT32(MARKER, hw_tim_tick());
    TEST_ASSERT_EQUAL_UINT16(ONE, hw_tim_active());
    TEST_ASSERT_EQUAL_UINT16(ONE, hw_tim_ccr3());

    /* FEED at the same tick: DMA transfers the next pulse (ZERO) */
    TEST_ASSERT_EQUAL_INT(HW_TIM_EV_FEED, hw_tim_step());
    TEST_ASSERT_EQUAL_UINT16(ZERO, hw_tim_ccr3());
    TEST_ASSERT_EQUAL_UINT16(ZERO, hw_tim_active());
    TEST_ASSERT_EQUAL_UINT32(2u, mock_feed_ch.CNDTR);

    /* update at ARR: next slot */
    TEST_ASSERT_EQUAL_INT(HW_TIM_EV_UPDATE, hw_tim_step());
    TEST_ASSERT_EQUAL_UINT32(1u, hw_tim_period());
    TEST_ASSERT_EQUAL_UINT32(0u, hw_tim_tick());

    /* --- slot 1: ZERO (60 us) --- */
    TEST_ASSERT_EQUAL_UINT16(ZERO, hw_tim_active());

    TEST_ASSERT_EQUAL_INT(HW_TIM_EV_CC2, hw_tim_step());
    TEST_ASSERT_EQUAL_UINT16(ZERO, hw_tim_active()); /* still slot-1 value */

    TEST_ASSERT_EQUAL_INT(HW_TIM_EV_FEED, hw_tim_step());
    TEST_ASSERT_EQUAL_UINT16(ONE, hw_tim_ccr3());
    TEST_ASSERT_EQUAL_UINT32(1u, mock_feed_ch.CNDTR);

    TEST_ASSERT_EQUAL_INT(HW_TIM_EV_UPDATE, hw_tim_step());
    TEST_ASSERT_EQUAL_UINT32(2u, hw_tim_period());

    /* --- slot 2 (last): ONE (5 us) --- */
    TEST_ASSERT_EQUAL_UINT16(ONE, hw_tim_active());

    TEST_ASSERT_EQUAL_INT(HW_TIM_EV_CC2, hw_tim_step());
    TEST_ASSERT_EQUAL_UINT16(ONE, hw_tim_active()); /* still slot-2 pulse */

    TEST_ASSERT_EQUAL_INT(HW_TIM_EV_FEED, hw_tim_step());
    TEST_ASSERT_EQUAL_UINT16(0u, hw_tim_ccr3()); /* trailing bus-release zero */
    TEST_ASSERT_EQUAL_UINT16(0u, hw_tim_active());
    TEST_ASSERT_EQUAL_UINT32(0u, mock_feed_ch.CNDTR);
    TEST_ASSERT_BITS_LOW(DMA_CCR_EN, mock_feed_ch.CCR);

    /* terminal update */
    TEST_ASSERT_EQUAL_INT(HW_TIM_EV_TERMINAL, hw_tim_step());
    TEST_ASSERT_TRUE(mock_tim1.SR & TIM_SR_UIF);
    TEST_ASSERT_BITS_LOW(TIM_CR1_CEN, mock_tim1.CR1);
    TEST_ASSERT_EQUAL_INT(HW_TIM_EV_IDLE, hw_tim_step());
}

/* ============================================================
 *  2. Trailing zero only after the last slot (two-slot variant)
 * ============================================================ */

void test_tim_trailing_zero_not_during_last_slot(void) {
    static uint8_t buf[3] = {(uint8_t)ZERO, ZERO, 0u}; /* 2 slots */
    hw_register_buf(&buf[1]);
    onewire_write_pulses(buf, 2);
    hw_tim_init();

    /* skip slot 0 */
    TEST_ASSERT_EQUAL_INT(HW_TIM_EV_CC2, hw_tim_step());
    TEST_ASSERT_EQUAL_UINT16(ZERO, hw_tim_active());
    TEST_ASSERT_EQUAL_INT(HW_TIM_EV_FEED, hw_tim_step());
    TEST_ASSERT_EQUAL_UINT16(ZERO, hw_tim_active());
    TEST_ASSERT_EQUAL_INT(HW_TIM_EV_UPDATE, hw_tim_step());
    TEST_ASSERT_EQUAL_UINT32(1u, hw_tim_period());

    /* slot 1 (last): active is still ZERO, not trailing 0 yet */
    TEST_ASSERT_EQUAL_UINT16(ZERO, hw_tim_active());
    TEST_ASSERT_EQUAL_UINT16(ZERO, hw_tim_ccr3()); /* buf[1] == ZERO */

    TEST_ASSERT_EQUAL_INT(HW_TIM_EV_CC2, hw_tim_step());
    TEST_ASSERT_EQUAL_UINT16(ZERO, hw_tim_active()); /* stable through CC2 */

    TEST_ASSERT_EQUAL_INT(HW_TIM_EV_FEED, hw_tim_step());
    TEST_ASSERT_EQUAL_UINT16(0u, hw_tim_ccr3()); /* trailing 0 loaded */

    TEST_ASSERT_EQUAL_INT(HW_TIM_EV_TERMINAL, hw_tim_step());
    TEST_ASSERT_TRUE(mock_tim1.SR & TIM_SR_UIF);
    /* after the stop the bus is released (active == 0) */
    TEST_ASSERT_EQUAL_UINT16(0u, hw_tim_active());
}

/* ============================================================
 *  3. OC3PE preload: active output stays stable through the
 *     whole single-slot period even though the preload register
 *     is zeroed before CEN.
 * ============================================================ */

void test_tim_preload_shadow_stable_through_slot(void) {
    uint8_t pulse = ONE;
    onewire_write_pulses(&pulse, 1); /* single-slot path: OC3PE + preload 0 */

    /* the driver programmed OC3PE (preload enabled) */
    TEST_ASSERT_TRUE(MOCK_TIM_OUT_CCMR & MOCK_TIM_OUT_PE);

    /* CCR3 register == 0 (preload cleared by the driver), but the shadow
     * holds the ONE pulse that was loaded at the forced update event (UG)
     * before the preload write. hw_tim_init_shadow() feeds the shadow. */
    hw_tim_init_shadow(ONE);
    TEST_ASSERT_EQUAL_UINT16(ONE, hw_tim_active());
    TEST_ASSERT_EQUAL_UINT16(0u, hw_tim_ccr3());

    /* The only scheduled event is the terminal update: no CC2/FEED at all.
     * This means the ONE output is stable from tick 0 right through to ARR. */
    TEST_ASSERT_EQUAL_INT(HW_TIM_EV_TERMINAL, hw_tim_step());
    TEST_ASSERT_EQUAL_UINT32(0u, hw_tim_period());
    TEST_ASSERT_EQUAL_UINT32(SLOT_ARR, hw_tim_tick());
    TEST_ASSERT_TRUE(mock_tim1.SR & TIM_SR_UIF);
    TEST_ASSERT_BITS_LOW(TIM_CR1_CEN, mock_tim1.CR1);

    /* Only at the terminal stop does the preload zero enter the output. */
    TEST_ASSERT_EQUAL_UINT16(0u, hw_tim_active());
    TEST_ASSERT_EQUAL_INT(HW_TIM_EV_IDLE, hw_tim_step());
}

/* ============================================================
 *  4. CC4 capture fires at the physical pulse-edge position
 *     (the counter value equals the captured duration), proving
 *     the capture event is temporally placed, not lumped at the
 *     slot start.
 * ============================================================ */

static uint16_t const_cap_src(uint32_t idx) {
    (void)idx;
    return 20u; /* fixed pulse-edge at tick 20 */
}

void test_tim_capture_at_pulse_edge_position(void) {
    static uint8_t rx[8];
    uint8_t i;
    for (i = 0; i < 8; i++) {
        rx[i] = 0xEE;
    }
    hw_register_buf(rx);
    hw_set_capture_source(const_cap_src);

    onewire_read_data(rx, 1); /* 8 slots, 8 captures, 8-bit MSIZE */
    TEST_ASSERT_EQUAL_UINT32(7u, mock_tim1.RCR);
    hw_tim_init();

    for (uint32_t p = 0; p < 8; p++) {
        /* capture fires at tick 20 inside the slot (physically: bus edge time) */
        TEST_ASSERT_EQUAL_INT(HW_TIM_EV_CAPTURE, hw_tim_step());
        TEST_ASSERT_EQUAL_UINT32(p, hw_tim_period());
        TEST_ASSERT_EQUAL_UINT32(20u, hw_tim_tick());
        TEST_ASSERT_EQUAL_UINT16(20u, hw_tim_ccr4());
        if (p < 7u) {
            /* the next event is the slot overflow itself, proving nothing
             * fires between tick 20 and ARR (no span-of-transfers trickery) */
            TEST_ASSERT_EQUAL_INT(HW_TIM_EV_UPDATE, hw_tim_step());
            TEST_ASSERT_EQUAL_UINT32(p + 1u, hw_tim_period());
            TEST_ASSERT_EQUAL_UINT32(0u, hw_tim_tick());
        }
    }
    /* terminal */
    TEST_ASSERT_EQUAL_INT(HW_TIM_EV_TERMINAL, hw_tim_step());
    TEST_ASSERT_TRUE(mock_tim1.SR & TIM_SR_UIF);

    TEST_ASSERT_EQUAL_UINT32(0u, mock_dma1_ch4.CNDTR);
    TEST_ASSERT_BITS_LOW(DMA_CCR_EN, mock_dma1_ch4.CCR);
    for (i = 0; i < 8; i++) {
        TEST_ASSERT_EQUAL_UINT8(20u, rx[i]);
    }
    TEST_ASSERT_EQUAL_INT(HW_TIM_EV_IDLE, hw_tim_step());
}

/* ============================================================
 *  5. Read pair: 2 captures in 2 slots; capture value determines
 *     the physical tick within each slot (showing one-short and
 *     one-long pulse edge within the same operation).
 * ============================================================ */

static uint16_t two_tick_src(uint32_t idx) {
    return idx == 0 ? 8u : 55u; /* short then long */
}

void test_tim_pair_capture_ticks_per_slot(void) {
    static uint16_t pair[2];
    uint8_t i;
    for (i = 0; i < 2; i++) {
        pair[i] = 0xEEEE;
    }
    hw_register_buf(pair);
    hw_set_capture_source(two_tick_src);

    onewire_read_pair(pair); /* 2 slots, RCR=1, 2 captures */
    TEST_ASSERT_EQUAL_UINT32(1u, mock_tim1.RCR);
    hw_tim_init();

    /* slot 0: capture at tick 8 (short pulse) */
    TEST_ASSERT_EQUAL_INT(HW_TIM_EV_CAPTURE, hw_tim_step());
    TEST_ASSERT_EQUAL_UINT32(0u, hw_tim_period());
    TEST_ASSERT_EQUAL_UINT32(8u, hw_tim_tick());
    TEST_ASSERT_EQUAL_UINT16(8u, hw_tim_ccr4());

    TEST_ASSERT_EQUAL_INT(HW_TIM_EV_UPDATE, hw_tim_step());
    TEST_ASSERT_EQUAL_UINT32(1u, hw_tim_period());
    TEST_ASSERT_EQUAL_UINT32(0u, hw_tim_tick());

    /* slot 1: capture at tick 55 (long pulse) */
    TEST_ASSERT_EQUAL_INT(HW_TIM_EV_CAPTURE, hw_tim_step());
    TEST_ASSERT_EQUAL_UINT32(1u, hw_tim_period());
    TEST_ASSERT_EQUAL_UINT32(55u, hw_tim_tick());
    TEST_ASSERT_EQUAL_UINT16(55u, hw_tim_ccr4());

    TEST_ASSERT_EQUAL_INT(HW_TIM_EV_TERMINAL, hw_tim_step());
    TEST_ASSERT_TRUE(mock_tim1.SR & TIM_SR_UIF);

    TEST_ASSERT_EQUAL_UINT16(8u, pair[0]);
    TEST_ASSERT_EQUAL_UINT16(55u, pair[1]);
    TEST_ASSERT_EQUAL_INT(HW_TIM_EV_IDLE, hw_tim_step());
}

/* ============================================================
 *  Runner
 * ============================================================ */

void run_test_tim_model(void) {
    TEST_RUN(test_tim_reload_happens_only_after_cc2);
    TEST_RUN(test_tim_trailing_zero_not_during_last_slot);
    TEST_RUN(test_tim_preload_shadow_stable_through_slot);
    TEST_RUN(test_tim_capture_at_pulse_edge_position);
    TEST_RUN(test_tim_pair_capture_ticks_per_slot);
}
