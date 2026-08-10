/* ============================================================
 *  test_temperature.c - Temperature Decoding Tests
 *
 *  Tests decode_temperature(): raw scratchpad bytes 0/1 (LSB/MSB)
 *  -> tenths of °C via (raw * 10) / 16.
 * ============================================================ */

#include "ds18b20.h"
#include "ds18b20_test_access.h"
#include "unity.h"

static int16_t decode_temp_from_raw(uint8_t lsb, uint8_t msb) {
    ds18b20_test_set_scratchpad(0, lsb);
    ds18b20_test_set_scratchpad(1, msb);
    return ds18b20_test_decode_temperature();
}

void test_temperature_zero_raw_returns_zero(void) {
    TEST_ASSERT_EQUAL_INT(0, decode_temp_from_raw(0x00, 0x00));
}

void test_temperature_22_25C(void) {
    TEST_ASSERT_EQUAL_INT(222, decode_temp_from_raw(0x64, 0x01));
}

void test_temperature_25_0625C(void) {
    TEST_ASSERT_EQUAL_INT(250, decode_temp_from_raw(0x91, 0x01));
}

void test_temperature_125C_max(void) {
    TEST_ASSERT_EQUAL_INT(1250, decode_temp_from_raw(0xD0, 0x07));
}

void test_temperature_neg_55C_min(void) {
    TEST_ASSERT_EQUAL_INT(-550, decode_temp_from_raw(0x90, 0xFC));
}

void test_temperature_neg_0_5C(void) {
    TEST_ASSERT_EQUAL_INT(-5, decode_temp_from_raw(0xF8, 0xFF));
}

void test_temperature_neg_10C(void) {
    TEST_ASSERT_EQUAL_INT(-100, decode_temp_from_raw(0x60, 0xFF));
}

void test_temperature_0_5C(void) {
    TEST_ASSERT_EQUAL_INT(5, decode_temp_from_raw(0x08, 0x00));
}

void test_temperature_smallest_positive(void) {
    TEST_ASSERT_EQUAL_INT(0, decode_temp_from_raw(0x01, 0x00));
}

void test_temperature_smallest_negative(void) {
    TEST_ASSERT_EQUAL_INT(0, decode_temp_from_raw(0xFF, 0xFF));
}

void test_temperature_85C_power_on_reset(void) {
    TEST_ASSERT_EQUAL_INT(850, decode_temp_from_raw(0x50, 0x05));
}

void run_test_temperature(void) {
    TEST_RUN(test_temperature_zero_raw_returns_zero);
    TEST_RUN(test_temperature_22_25C);
    TEST_RUN(test_temperature_25_0625C);
    TEST_RUN(test_temperature_125C_max);
    TEST_RUN(test_temperature_neg_55C_min);
    TEST_RUN(test_temperature_neg_0_5C);
    TEST_RUN(test_temperature_neg_10C);
    TEST_RUN(test_temperature_0_5C);
    TEST_RUN(test_temperature_smallest_positive);
    TEST_RUN(test_temperature_smallest_negative);
    TEST_RUN(test_temperature_85C_power_on_reset);
}
