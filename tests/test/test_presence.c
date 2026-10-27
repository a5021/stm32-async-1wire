/* ============================================================
 *  test_presence.c - Presence Detection Boundary Tests
 *
 *  Validates onewire_present() against the DS18B20 spec ranges:
 *  - pulse[0] (reset pulse) in [480, 540]µs
 *  - pulse[1] (presence pulse) in [555, 840]µs
 *  plus the arm_capture DMA width configuration.
 * ============================================================ */

#include "ds18b20.h"
#include "ds18b20_test_access.h"
#include "mock_target.h"
#include "ow_port.h"
#include "unity.h"

#define RESET_MIN 480u
#define RESET_MAX 540u
#define PRES_MIN 555u
#define PRES_MAX 840u

static unsigned check_presence_with_pulses(uint16_t pulse0, uint16_t pulse1,
                                           uint8_t captured) {
    uint16_t pulses[2] = {pulse0, pulse1};
    return onewire_present(pulses, captured);
}

static void assert_presence(uint16_t pulse0, uint16_t pulse1, unsigned expected) {
    if (expected) {
        TEST_ASSERT_TRUE(check_presence_with_pulses(pulse0, pulse1, 2));
    } else {
        TEST_ASSERT_FALSE(check_presence_with_pulses(pulse0, pulse1, 2));
    }
}

void test_presence_nominal_values_present(void) {
    assert_presence(510, 700, 1);
}

void test_presence_minimum_values_present(void) {
    assert_presence(RESET_MIN, PRES_MIN, 1);
}

void test_presence_maximum_values_present(void) {
    assert_presence(RESET_MAX, PRES_MAX, 1);
}

void test_presence_reset_pulse_too_short(void) {
    assert_presence(RESET_MIN - 1, 700, 0);
}

void test_presence_reset_pulse_too_long(void) {
    assert_presence(RESET_MAX + 1, 700, 0);
}

void test_presence_presence_pulse_too_short(void) {
    assert_presence(510, PRES_MIN - 1, 0);
}

void test_presence_presence_pulse_too_long(void) {
    assert_presence(510, PRES_MAX + 1, 0);
}

void test_presence_both_pulses_out_of_range(void) {
    assert_presence(100, 100, 0);
}

void test_presence_zero_pulses_absent(void) {
    assert_presence(0, 0, 0);
}

void test_presence_very_large_values_absent(void) {
    assert_presence(10000, 10000, 0);
}

void test_presence_reset_min_presence_max(void) {
    assert_presence(RESET_MIN, PRES_MAX, 1);
}

void test_presence_reset_max_presence_min(void) {
    assert_presence(RESET_MAX, PRES_MIN, 1);
}

/* A no-presence reset captures only pulse[0]; pulse[1] keeps whatever the
 * shared buffer held before and may sit inside the presence window. The
 * captured-edge count is what proves the presence edge arrived, so a stale
 * pulse[1] must not fabricate a device. */
void test_presence_underrun_ignores_stale_presence(void) {
    const uint16_t stale_reset = 510;
    const uint16_t stale_presence = 700; /* inside the presence window */

    TEST_ASSERT_FALSE(check_presence_with_pulses(stale_reset, stale_presence, 0));
    TEST_ASSERT_FALSE(check_presence_with_pulses(stale_reset, stale_presence, 1));
    TEST_ASSERT_TRUE(check_presence_with_pulses(stale_reset, stale_presence, 2));
}

void test_capture_16bit_config(void) {
    uint16_t dst[2];
    ds18b20_test_arm_capture((volatile void*)dst, 2, 16);
#if defined(STM32H5)
    TEST_ASSERT_BITS_HIGH(DMA_CCR_EN, mock_dma1_ch4.CCR);
    TEST_ASSERT_EQUAL_UINT32((uint32_t)OW_PORT_DMA_CR_RX16, mock_dma1_ch4.CTR1);
    TEST_ASSERT_EQUAL_UINT32((61u << DMA_CTR2_REQSEL_Pos), mock_dma1_ch4.CTR2);
#else
    uint32_t ccr = mock_dma1_ch4.CCR;
    TEST_ASSERT_TRUE((ccr & DMA_CCR_EN) != 0);
    TEST_ASSERT_TRUE((ccr & DMA_CCR_MINC) != 0);
    TEST_ASSERT_TRUE((ccr & DMA_CCR_PSIZE_0) != 0);
    TEST_ASSERT_TRUE((ccr & DMA_CCR_MSIZE_0) != 0);
    TEST_ASSERT_TRUE((ccr & DMA_CCR_MSIZE_1) == 0);
#endif
    TEST_ASSERT_EQUAL_UINT32(MOCK_CAP_CNDTR(2), mock_dma1_ch4.CNDTR);
}

void test_capture_8bit_config(void) {
    uint8_t dst[2];
    ds18b20_test_arm_capture((volatile void*)dst, 2, 8);
#if defined(STM32H5)
    TEST_ASSERT_BITS_HIGH(DMA_CCR_EN, mock_dma1_ch4.CCR);
    TEST_ASSERT_EQUAL_UINT32((uint32_t)OW_PORT_DMA_CR_RX8, mock_dma1_ch4.CTR1);
    TEST_ASSERT_EQUAL_UINT32((61u << DMA_CTR2_REQSEL_Pos), mock_dma1_ch4.CTR2);
#else
    uint32_t ccr = mock_dma1_ch4.CCR;
    TEST_ASSERT_TRUE((ccr & DMA_CCR_EN) != 0);
    TEST_ASSERT_TRUE((ccr & DMA_CCR_MINC) != 0);
#if defined(STM32F4)
    /* F4 direct mode forces the memory width to the 8-bit PSIZE */
    TEST_ASSERT_TRUE((ccr & DMA_CCR_PSIZE_0) == 0);
#else
    TEST_ASSERT_TRUE((ccr & DMA_CCR_PSIZE_0) != 0);
#endif
    TEST_ASSERT_TRUE((ccr & DMA_CCR_MSIZE_0) == 0);
    TEST_ASSERT_TRUE((ccr & DMA_CCR_MSIZE_1) == 0);
#endif
    TEST_ASSERT_EQUAL_UINT32(MOCK_CAP_CNDTR(2), mock_dma1_ch4.CNDTR);
}

void run_test_presence(void) {
    TEST_RUN(test_presence_nominal_values_present);
    TEST_RUN(test_presence_minimum_values_present);
    TEST_RUN(test_presence_maximum_values_present);
    TEST_RUN(test_presence_reset_pulse_too_short);
    TEST_RUN(test_presence_reset_pulse_too_long);
    TEST_RUN(test_presence_presence_pulse_too_short);
    TEST_RUN(test_presence_presence_pulse_too_long);
    TEST_RUN(test_presence_both_pulses_out_of_range);
    TEST_RUN(test_presence_zero_pulses_absent);
    TEST_RUN(test_presence_very_large_values_absent);
    TEST_RUN(test_presence_reset_min_presence_max);
    TEST_RUN(test_presence_reset_max_presence_min);
    TEST_RUN(test_presence_underrun_ignores_stale_presence);
    TEST_RUN(test_capture_16bit_config);
    TEST_RUN(test_capture_8bit_config);
}
