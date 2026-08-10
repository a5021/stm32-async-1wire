/* ============================================================
 *  test_presence.c - Presence Detection Boundary Tests
 *
 *  Validates check_presence() against the DS18B20 spec ranges:
 *  - edge[0] (reset pulse) in [480, 540]µs
 *  - edge[1] (presence pulse) in [555, 840]µs
 *  plus the arm_capture DMA width configuration.
 * ============================================================ */

#include "ds18b20.h"
#include "ds18b20_test_access.h"
#include "stm32f1xx.h"
#include "unity.h"

#define RESET_MIN 480u
#define RESET_MAX 540u
#define PRES_MIN 555u
#define PRES_MAX 840u

static unsigned check_presence_with_edges(uint16_t edge0, uint16_t edge1) {
    ds18b20_test_set_edge(0, edge0);
    ds18b20_test_set_edge(1, edge1);
    return ds18b20_test_check_presence();
}

static void assert_presence(uint16_t edge0, uint16_t edge1, unsigned expected) {
    if (expected) {
        TEST_ASSERT_TRUE(check_presence_with_edges(edge0, edge1));
    } else {
        TEST_ASSERT_FALSE(check_presence_with_edges(edge0, edge1));
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

void test_presence_both_edges_out_of_range(void) {
    assert_presence(100, 100, 0);
}

void test_presence_zero_edges_absent(void) {
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

void test_capture_16bit_config(void) {
    uint16_t dst[2];
    ds18b20_test_arm_capture((volatile void*)dst, 2, 16);
    uint32_t ccr = mock_dma1_ch3.CCR;
    TEST_ASSERT_TRUE((ccr & DMA_CCR_EN) != 0);
    TEST_ASSERT_TRUE((ccr & DMA_CCR_MINC) != 0);
    TEST_ASSERT_TRUE((ccr & DMA_CCR_PSIZE_0) != 0);
    TEST_ASSERT_TRUE((ccr & DMA_CCR_MSIZE_0) != 0);
    TEST_ASSERT_TRUE((ccr & DMA_CCR_MSIZE_1) == 0);
    TEST_ASSERT_EQUAL_UINT32(2, mock_dma1_ch3.CNDTR);
}

void test_capture_8bit_config(void) {
    uint8_t dst[2];
    ds18b20_test_arm_capture((volatile void*)dst, 2, 8);
    uint32_t ccr = mock_dma1_ch3.CCR;
    TEST_ASSERT_TRUE((ccr & DMA_CCR_EN) != 0);
    TEST_ASSERT_TRUE((ccr & DMA_CCR_MINC) != 0);
    TEST_ASSERT_TRUE((ccr & DMA_CCR_PSIZE_0) != 0);
    TEST_ASSERT_TRUE((ccr & DMA_CCR_MSIZE_0) == 0);
    TEST_ASSERT_TRUE((ccr & DMA_CCR_MSIZE_1) == 0);
    TEST_ASSERT_EQUAL_UINT32(2, mock_dma1_ch3.CNDTR);
}

void run_test_presence(void) {
    TEST_RUN(test_presence_nominal_values_present);
    TEST_RUN(test_presence_minimum_values_present);
    TEST_RUN(test_presence_maximum_values_present);
    TEST_RUN(test_presence_reset_pulse_too_short);
    TEST_RUN(test_presence_reset_pulse_too_long);
    TEST_RUN(test_presence_presence_pulse_too_short);
    TEST_RUN(test_presence_presence_pulse_too_long);
    TEST_RUN(test_presence_both_edges_out_of_range);
    TEST_RUN(test_presence_zero_edges_absent);
    TEST_RUN(test_presence_very_large_values_absent);
    TEST_RUN(test_presence_reset_min_presence_max);
    TEST_RUN(test_presence_reset_max_presence_min);
    TEST_RUN(test_capture_16bit_config);
    TEST_RUN(test_capture_8bit_config);
}
