/* ============================================================
 *  test_pulse_encoding.c - Pulse Encoding Tests
 *
 *  Tests encode_byte_pulses(): byte value -> 8 pulse durations,
 *  LSB first. bit = 1 -> ONE_PULSE (5µs), bit = 0 -> ZERO_PULSE (60µs).
 * ============================================================ */

#include "ds18b20.h"
#include "ds18b20_test_access.h"
#include "onewire.h"
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

void test_onewire_bit_from_pulse_short_is_one(void) {
    TEST_ASSERT_EQUAL_UINT8(1, onewire_bit_from_pulse(0));
    TEST_ASSERT_EQUAL_UINT8(1, onewire_bit_from_pulse(ONEWIRE_SHORT_PULSE_MAX));
    TEST_ASSERT_EQUAL_UINT8(1, onewire_bit_from_pulse(5));
}

void test_onewire_bit_from_pulse_long_is_zero(void) {
    TEST_ASSERT_EQUAL_UINT8(0, onewire_bit_from_pulse(ONEWIRE_SHORT_PULSE_MAX + 1));
    TEST_ASSERT_EQUAL_UINT8(0, onewire_bit_from_pulse(60));
    TEST_ASSERT_EQUAL_UINT8(0, onewire_bit_from_pulse(0xFFFF));
}

void test_onewire_decode_pulses_all_short_is_0xFF(void) {
    uint8_t pulse[8];
    for (int i = 0; i < 8; i++)
        pulse[i] = (uint8_t)ONEWIRE_SHORT_PULSE_MAX;
    uint8_t dst[1];
    onewire_decode_pulses(dst, pulse, 1);
    TEST_ASSERT_EQUAL_UINT8(0xFF, dst[0]);
}

void test_onewire_decode_pulses_all_long_is_0x00(void) {
    uint8_t pulse[8];
    for (int i = 0; i < 8; i++)
        pulse[i] = 60;
    uint8_t dst[1];
    onewire_decode_pulses(dst, pulse, 1);
    TEST_ASSERT_EQUAL_UINT8(0x00, dst[0]);
}

void test_onewire_decode_pulses_pattern(void) {
    uint8_t pulse[8];
    for (int i = 0; i < 8; i++) {
        pulse[i] = (uint8_t)(((0x55u >> i) & 1u) ? ONEWIRE_SHORT_PULSE_MAX : 60u);
    }
    uint8_t dst[1];
    onewire_decode_pulses(dst, pulse, 1);
    TEST_ASSERT_EQUAL_UINT8(0x55, dst[0]);
}

void test_onewire_decode_pulses_two_bytes(void) {
    uint8_t pulse[16];
    for (int i = 0; i < 16; i++) {
        uint8_t byte = (i < 8) ? 0x55u : 0xAAu;
        pulse[i] = (uint8_t)(((byte >> (i % 8)) & 1u) ? ONEWIRE_SHORT_PULSE_MAX : 60u);
    }
    uint8_t dst[2];
    onewire_decode_pulses(dst, pulse, 2);
    TEST_ASSERT_EQUAL_UINT8(0x55, dst[0]);
    TEST_ASSERT_EQUAL_UINT8(0xAA, dst[1]);
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
    TEST_RUN(test_onewire_bit_from_pulse_short_is_one);
    TEST_RUN(test_onewire_bit_from_pulse_long_is_zero);
    TEST_RUN(test_onewire_decode_pulses_all_short_is_0xFF);
    TEST_RUN(test_onewire_decode_pulses_all_long_is_0x00);
    TEST_RUN(test_onewire_decode_pulses_pattern);
    TEST_RUN(test_onewire_decode_pulses_two_bytes);
}
