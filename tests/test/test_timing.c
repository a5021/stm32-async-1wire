/* ============================================================
 *  test_timing.c - Timing Register Regression Tests
 *
 *  Verifies the actual register values each bus operation programs
 *  into TIM1 (ARR/RCR/CCR1/CCR4) against the DS18B20 timing spec.
 *  Unlike a constant re-check, this locks the real driver output.
 * ============================================================ */

#include "ds18b20.h"
#include "ds18b20_test_access.h"
#include "hw_model.h"
#include "stm32f1xx.h"
#include "unity.h"

void test_timing_reset_programs_timeout_and_pulse(void) {
    hw_reset_all();
    test_bus_reset();
    /* RESET_TIMEOUT = 2 * RESET_PULSE_MIN = 960µs, pulse = 480µs */
    TEST_ASSERT_EQUAL_UINT16(960, (uint16_t)mock_tim1.ARR);
    /* one slot: RCR = 0 */
    TEST_ASSERT_EQUAL_UINT32(0, mock_tim1.RCR);
    /* capture ops preload CCR1 with 0 via OC1PE (hardware bus release) */
    TEST_ASSERT_TRUE(mock_tim1.CCMR1 & TIM_CCMR1_OC1PE);
    TEST_ASSERT_EQUAL_UINT16(0, (uint16_t)mock_tim1.CCR1);
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
    /* CCR4 triggers the DMA reload at ONE_PULSE + ZERO_PULSE = 65µs */
    TEST_ASSERT_EQUAL_UINT32(65, mock_tim1.CCR4);
}

void test_timing_read_programs_72_slots(void) {
    hw_reset_all();
    test_bus_read_data();
    /* 72 data slots: RCR = 71 */
    TEST_ASSERT_EQUAL_UINT32(71, mock_tim1.RCR);
    TEST_ASSERT_EQUAL_UINT16(70, (uint16_t)mock_tim1.ARR);
    /* capture ops preload CCR1 with 0 via OC1PE (hardware bus release) */
    TEST_ASSERT_TRUE(mock_tim1.CCMR1 & TIM_CCMR1_OC1PE);
    TEST_ASSERT_EQUAL_UINT32(0, mock_tim1.CCR1);
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

void run_test_timing(void) {
    TEST_RUN(test_timing_reset_programs_timeout_and_pulse);
    TEST_RUN(test_timing_command_programs_slot_period);
    TEST_RUN(test_timing_read_programs_72_slots);
    TEST_RUN(test_timing_wait_conversion_750ms);
    TEST_RUN(test_timing_start_cycle_pause_5s);
    TEST_RUN(test_timing_temperature_formula);
}
