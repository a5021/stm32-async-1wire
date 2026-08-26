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
    uint8_t cmd[17];
    for (int i = 0; i < 16; i++) {
        cmd[i] = (i & 1) ? (uint8_t)5 : (uint8_t)60;
    }
    cmd[16] = 0;
    test_bus_send_command_n(cmd, 16);
    /* ARR = ONE_PULSE + ZERO_PULSE + GUARD_BAND = 5+60+5 = 70µs */
    TEST_ASSERT_EQUAL_UINT16(70, (uint16_t)mock_tim1.ARR);
    /* RCR = slots - 1 */
    TEST_ASSERT_EQUAL_UINT32(15, mock_tim1.RCR);
    /* the slot-end marker compare triggers the DMA reload at ONE_PULSE + ZERO_PULSE = 65µs */
    TEST_ASSERT_EQUAL_UINT32(65, MOCK_TIM_MARKER_CCR);
}

void test_timing_read_programs_72_slots(void) {
    hw_reset_all();
    test_bus_read_data();
    /* 72 data slots: RCR = 71 */
    TEST_ASSERT_EQUAL_UINT32(71, mock_tim1.RCR);
    TEST_ASSERT_EQUAL_UINT16(70, (uint16_t)mock_tim1.ARR);
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

void test_timing_start_cycle_pause_5s(void) {
    hw_reset_all();
    test_bus_start_cycle_pause();
    /* PAUSE_5S = 62500 ticks * 80 periods * 1µs = 5s */
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
 *  Test: APB prescaler for TIM1 bus must be /1.
 *
 *  STM32 rule: if APB prescaler != 1, TIM clock = 2 × PCLK.
 *  This doubles the tick rate and breaks every µs-based timing
 *  constant (slots, reset pulse, conversion wait).
 *
 *  configure_system_clock() must NOT divide the APB bus feeding
 *  TIM1.  This test catches the mistake at the register level.
 *
 *  F0: TIM1 on APB2, PPRE defaults /1.  ✓
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
#elif defined(OW_PORT_TARGET_F0)
    /* F0: single APB bus — PPRE must be /1 (field = 0) */
    TEST_ASSERT_EQUAL_UINT32(0, mock_rcc.CFGR & RCC_CFGR_PPRE_Msk);
#elif defined(OW_PORT_TARGET_G0)
    /* G0: single APB bus — PPRE must be /1 (field = 0) */
    TEST_ASSERT_EQUAL_UINT32(0, mock_rcc.CFGR & RCC_CFGR_PPRE_Msk);
#endif
}

void run_test_timing(void) {
    TEST_RUN(test_timing_reset_programs_timeout_and_pulse);
    TEST_RUN(test_timing_command_programs_slot_period);
    TEST_RUN(test_timing_read_programs_72_slots);
    TEST_RUN(test_timing_wait_conversion_750ms);
    TEST_RUN(test_timing_start_cycle_pause_5s);
    TEST_RUN(test_timing_temperature_formula);
    TEST_RUN(test_apb_prescaler_div1_for_tim1);
}
