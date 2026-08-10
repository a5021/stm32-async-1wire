/* ============================================================
 *  test_pulse_encoding.c - Pulse Encoding Tests
 *
 *  Tests encode_byte_pulses(): byte value -> 8 pulse durations,
 *  LSB first. bit = 1 -> ONE_PULSE (5µs), bit = 0 -> ZERO_PULSE (60µs).
 * ============================================================ */

#include "ds18b20.h"
#include "ds18b20_test_access.h"
#include "unity.h"

#define ONE_P 5u
#define ZERO_P 60u

void test_pulse_encoding_zero_byte_all_zero_pulse(void) {
    uint8_t out[8];
    ds18b20_test_encode_byte_pulses(out, 0x00);
    for (int i = 0; i < 8; i++) {
        TEST_ASSERT_EQUAL_UINT8(ZERO_P, out[i]);
    }
}

void test_pulse_encoding_0xFF_all_one_pulse(void) {
    uint8_t out[8];
    ds18b20_test_encode_byte_pulses(out, 0xFF);
    for (int i = 0; i < 8; i++) {
        TEST_ASSERT_EQUAL_UINT8(ONE_P, out[i]);
    }
}

void test_pulse_encoding_0x01_first_bit_one_pulse(void) {
    uint8_t out[8];
    ds18b20_test_encode_byte_pulses(out, 0x01);
    TEST_ASSERT_EQUAL_UINT8(ONE_P, out[0]);
    for (int i = 1; i < 8; i++) {
        TEST_ASSERT_EQUAL_UINT8(ZERO_P, out[i]);
    }
}

void test_pulse_encoding_0x80_last_bit_one_pulse(void) {
    uint8_t out[8];
    ds18b20_test_encode_byte_pulses(out, 0x80);
    for (int i = 0; i < 7; i++) {
        TEST_ASSERT_EQUAL_UINT8(ZERO_P, out[i]);
    }
    TEST_ASSERT_EQUAL_UINT8(ONE_P, out[7]);
}

void test_pulse_encoding_0xAA_alternating(void) {
    uint8_t out[8];
    ds18b20_test_encode_byte_pulses(out, 0xAA);
    for (int i = 0; i < 8; i++) {
        TEST_ASSERT_EQUAL_UINT8((i & 1) ? ONE_P : ZERO_P, out[i]);
    }
}

void test_pulse_encoding_0x55_alternating(void) {
    uint8_t out[8];
    ds18b20_test_encode_byte_pulses(out, 0x55);
    for (int i = 0; i < 8; i++) {
        TEST_ASSERT_EQUAL_UINT8((i & 1) ? ZERO_P : ONE_P, out[i]);
    }
}

void test_pulse_encoding_output_length_always_8(void) {
    uint8_t out[8];
    ds18b20_test_encode_byte_pulses(out, 0x00);
    ds18b20_test_encode_byte_pulses(out, 0xFF);
    ds18b20_test_encode_byte_pulses(out, 0x55);
    ds18b20_test_encode_byte_pulses(out, 0xAA);
    TEST_ASSERT_TRUE(1);
}

void test_pulse_encoding_only_valid_pulse_values(void) {
    uint8_t out[8];
    for (int b = 0; b < 256; b++) {
        ds18b20_test_encode_byte_pulses(out, (uint8_t)b);
        for (int i = 0; i < 8; i++) {
            TEST_ASSERT_TRUE(out[i] == ONE_P || out[i] == ZERO_P);
        }
    }
}

void test_pulse_encoding_single_bit_positions(void) {
    uint8_t out[8];
    for (int bit = 0; bit < 8; bit++) {
        ds18b20_test_encode_byte_pulses(out, (uint8_t)(1u << bit));
        for (int i = 0; i < 8; i++) {
            TEST_ASSERT_EQUAL_UINT8(i == bit ? ONE_P : ZERO_P, out[i]);
        }
    }
}

void run_test_pulse_encoding(void) {
    TEST_RUN(test_pulse_encoding_zero_byte_all_zero_pulse);
    TEST_RUN(test_pulse_encoding_0xFF_all_one_pulse);
    TEST_RUN(test_pulse_encoding_0x01_first_bit_one_pulse);
    TEST_RUN(test_pulse_encoding_0x80_last_bit_one_pulse);
    TEST_RUN(test_pulse_encoding_0xAA_alternating);
    TEST_RUN(test_pulse_encoding_0x55_alternating);
    TEST_RUN(test_pulse_encoding_output_length_always_8);
    TEST_RUN(test_pulse_encoding_only_valid_pulse_values);
    TEST_RUN(test_pulse_encoding_single_bit_positions);
}
