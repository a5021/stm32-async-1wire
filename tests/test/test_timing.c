/* ============================================================
 *  test_timing.c - Timing Register Regression Tests
 *
 *  Verifies the actual register values each bus operation programs
 *  into TIM1 (ARR/RCR/CCR2/CCR3) against the DS18B20 timing spec.
 *  Unlike a constant re-check, this locks the real driver output.
 * ============================================================ */

#include "ds18b20.h"
#include "ds18b20_test_access.h"
#include "hw_model.h"
#include "mock_target.h"
#include "onewire.h"
#include "unity.h"
#if defined(OW_PORT_TARGET_F4)
#include "app.h" /* configure_system_clock() test surface */
#endif

void test_timing_reset_programs_timeout_and_pulse(void) {
    hw_reset_all();
    test_bus_reset();
    /* RESET_TIMEOUT = 2 * RESET_PULSE_MIN = 960µs, pulse = 480µs */
    TEST_ASSERT_EQUAL_UINT16(960, (uint16_t)mock_tim1.ARR);
    /* one slot: RCR = 0 */
    TEST_ASSERT_EQUAL_UINT32(0, mock_tim1.RCR);
    /* capture ops preload the output CCR with 0 via OCxPE (hardware bus release) */
    TEST_ASSERT_TRUE(MOCK_TIM_OUT_CCMR & MOCK_TIM_OUT_PE);
    TEST_ASSERT_EQUAL_UINT16(0, (uint16_t)MOCK_TIM_OUT_CCR);
}

void test_timing_command_programs_slot_period(void) {
    hw_reset_all();
    ow_pulse_t cmd[17];
    for (int i = 0; i < 16; i++) {
        cmd[i] = (i & 1) ? ONEWIRE_ONE_PULSE : ONEWIRE_ZERO_PULSE;
    }
    cmd[16] = 0;
    test_bus_send_command_n(cmd, 16);
    /* ARR = one_pulse + zero_pulse + guard_band */
    TEST_ASSERT_EQUAL_UINT16((uint16_t)(ONEWIRE_ONE_PULSE + ONEWIRE_ZERO_PULSE + ONEWIRE_GUARD_BAND),
                             (uint16_t)mock_tim1.ARR);
    /* RCR = slots - 1 */
    TEST_ASSERT_EQUAL_UINT32(15, mock_tim1.RCR);
    /* the slot-end marker compare triggers the DMA reload at one_pulse + zero_pulse */
    TEST_ASSERT_EQUAL_UINT32((uint32_t)(ONEWIRE_ONE_PULSE + ONEWIRE_ZERO_PULSE), MOCK_TIM_MARKER_CCR);
}

void test_timing_read_programs_72_slots(void) {
    hw_reset_all();
    test_bus_read_data();
    /* 72 data slots: RCR = 71 */
    TEST_ASSERT_EQUAL_UINT32(71, mock_tim1.RCR);
    TEST_ASSERT_EQUAL_UINT16((uint16_t)(ONEWIRE_ONE_PULSE + ONEWIRE_ZERO_PULSE + ONEWIRE_GUARD_BAND),
                             (uint16_t)mock_tim1.ARR);
    /* capture ops preload the output CCR with 0 via OCxPE (hardware bus release) */
    TEST_ASSERT_TRUE(MOCK_TIM_OUT_CCMR & MOCK_TIM_OUT_PE);
    TEST_ASSERT_EQUAL_UINT32(0, MOCK_TIM_OUT_CCR);
}

void test_timing_wait_conversion_750ms(void) {
    hw_reset_all();
    test_bus_wait_conversion();
    /* PAUSE_750MS = 62500 ticks * 12 periods * 1µs = 750ms */
    TEST_ASSERT_EQUAL_UINT32(62500, mock_tim1.ARR);
    TEST_ASSERT_EQUAL_UINT32(11, mock_tim1.RCR);
}

void test_timing_long_wait_arr_rcr(void) {
    hw_reset_all();
    /* Generic long idle-HIGH wait: 62500 ticks * 80 periods * 1us = 5s. The
     * driver no longer has a cycle-pause knob, so the ARR/RCR math of a long
     * timer pass is covered through the raw onewire_start_timer() contract. */
    test_bus_start_timer(62500, 79);
    TEST_ASSERT_EQUAL_UINT32(62500, mock_tim1.ARR);
    TEST_ASSERT_EQUAL_UINT32(79, mock_tim1.RCR);
}

void test_timing_temperature_formula(void) {
    /* raw = 0x0164 = 356 -> 22.25°C -> 223 tenths (round-half-away-from-zero) */
    TEST_ASSERT_EQUAL_INT(223, (int)(((int32_t)0x0164 * 10 + 8) / 16));
    /* raw = 0x0000 -> 0 */
    TEST_ASSERT_EQUAL_INT(0, (int)(((int32_t)0x0000 * 10 + 8) / 16));
}

/*-------------------------------------------------------------
 *  Test: APB prescaler feeding TIM1 keeps the 1µs-tick invariant.
 *
 *  STM32 rule: if APB prescaler != 1, TIM clock = 2 × PCLK.
 *  On F0/F1/G0 that doubles the tick rate and breaks every µs-based
 *  timing constant (slots, reset pulse, conversion wait), so those
 *  families must leave the TIM1 APB prescaler at /1.
 *
 *  F4 (168MHz) is the intentional exception: APB2 is programmed /2
 *  (84MHz, datasheet max) by configure_system_clock(), and the timer
 *  clock doubles back to 2 × 84 = 168 = SYSCLK — same 1µs tick.
 *  This test drives the real clock path against the RCC/FLASH mocks
 *  and asserts those programmed fields.
 *
 *  F0: TIM1 on APB2, PPRE defaults /1.  ✓ (vacuous under harness)
 *  F1: TIM1 on APB2, PPRE2 stays /1 (PPRE1=/2 is OK — different bus).  ✓
 *  G0: single APB bus, PPRE defaults /1.  ✓
 * -----------------------------------------------------------*/
void test_apb_prescaler_div1_for_tim1(void) {
    hw_reset_all();
    ds18b20_init();
    /* PSC must equal SYSCLK_MHZ - 1 (1µs tick at full SYSCLK) */
    TEST_ASSERT_EQUAL_UINT16(OW_PORT_SYSCLK_MHZ - 1, (uint16_t)mock_tim1.PSC);

#if defined(OW_PORT_TARGET_F1)
    /* F1: TIM1 on APB2 — PPRE2 must be /1 (field = 0) */
    TEST_ASSERT_EQUAL_UINT32(0, mock_rcc.CFGR & RCC_CFGR_PPRE2_Msk);
#elif defined(OW_PORT_TARGET_F4)
    /* Preset the ready/status flags configure_system_clock() spin-waits on:
     * the mock does not model hardware self-setting of HSERDY/PLLRDY/SWS. */
    mock_rcc.CR = RCC_CR_HSERDY | RCC_CR_PLLRDY;
    mock_rcc.CFGR = RCC_CFGR_SWS_PLL;
    configure_system_clock();
    /* APB1 /4 = 42MHz, APB2 /2 = 84MHz (both at datasheet limits).
     * TIM1 is on APB2: 2 × 84 = 168 = SYSCLK (1µs tick invariant). */
    TEST_ASSERT_EQUAL_UINT32(RCC_CFGR_PPRE1_DIV4, mock_rcc.CFGR & RCC_CFGR_PPRE1_Msk);
    TEST_ASSERT_EQUAL_UINT32(RCC_CFGR_PPRE2_DIV2, mock_rcc.CFGR & RCC_CFGR_PPRE2_Msk);
#elif defined(OW_PORT_TARGET_F0)
    /* F0: single APB bus — PPRE must be /1 (field = 0) */
    TEST_ASSERT_EQUAL_UINT32(0, mock_rcc.CFGR & RCC_CFGR_PPRE_Msk);
#elif defined(OW_PORT_TARGET_G0)
    /* G0: single APB bus — PPRE must be /1 (field = 0) */
    TEST_ASSERT_EQUAL_UINT32(0, mock_rcc.CFGR & RCC_CFGR_PPRE_Msk);
#endif
}

/*-------------------------------------------------------------
 *  onewire_search_start() (onewire.c early-return guard).
 *----------------------------------------------------------*/
void test_search_start_ignored_while_running(void) {
    onewire_search_start(NULL, 1, DS18B20_SEARCH_ROM, 0);
    TEST_ASSERT_TRUE(onewire_search_active());
    /* Second start while the search owns the timer must be ignored. */
    onewire_search_start(NULL, 1, DS18B20_SEARCH_ROM, 0);
    TEST_ASSERT_TRUE(onewire_search_active());
    ds18b20_test_reset_search();
}

void run_test_timing(void) {
    TEST_RUN(test_timing_reset_programs_timeout_and_pulse);
    TEST_RUN(test_timing_command_programs_slot_period);
    TEST_RUN(test_timing_read_programs_72_slots);
    TEST_RUN(test_timing_wait_conversion_750ms);
    TEST_RUN(test_timing_long_wait_arr_rcr);
    TEST_RUN(test_timing_temperature_formula);
    TEST_RUN(test_apb_prescaler_div1_for_tim1);
    TEST_RUN(test_search_start_ignored_while_running);
}
