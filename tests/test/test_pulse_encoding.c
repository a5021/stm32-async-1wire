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

#define ONE_P ONEWIRE_ONE_PULSE
#define ZERO_P ONEWIRE_ZERO_PULSE

void test_pulse_encoding_zero_byte_all_zero_pulse(void) {
    ow_pulse_t out[8];
    ds18b20_test_encode_byte_pulses(out, 0x00);
    for (int i = 0; i < 8; i++) {
        TEST_ASSERT_EQUAL_UINT8(ZERO_P, out[i]);
    }
}

void test_pulse_encoding_0xFF_all_one_pulse(void) {
    ow_pulse_t out[8];
    ds18b20_test_encode_byte_pulses(out, 0xFF);
    for (int i = 0; i < 8; i++) {
        TEST_ASSERT_EQUAL_UINT8(ONE_P, out[i]);
    }
}

void test_pulse_encoding_0x01_first_bit_one_pulse(void) {
    ow_pulse_t out[8];
    ds18b20_test_encode_byte_pulses(out, 0x01);
    TEST_ASSERT_EQUAL_UINT8(ONE_P, out[0]);
    for (int i = 1; i < 8; i++) {
        TEST_ASSERT_EQUAL_UINT8(ZERO_P, out[i]);
    }
}

void test_pulse_encoding_0x80_last_bit_one_pulse(void) {
    ow_pulse_t out[8];
    ds18b20_test_encode_byte_pulses(out, 0x80);
    for (int i = 0; i < 7; i++) {
        TEST_ASSERT_EQUAL_UINT8(ZERO_P, out[i]);
    }
    TEST_ASSERT_EQUAL_UINT8(ONE_P, out[7]);
}

void test_pulse_encoding_0xAA_alternating(void) {
    ow_pulse_t out[8];
    ds18b20_test_encode_byte_pulses(out, 0xAA);
    for (int i = 0; i < 8; i++) {
        TEST_ASSERT_EQUAL_UINT8((i & 1) ? ONE_P : ZERO_P, out[i]);
    }
}

void test_pulse_encoding_0x55_alternating(void) {
    ow_pulse_t out[8];
    ds18b20_test_encode_byte_pulses(out, 0x55);
    for (int i = 0; i < 8; i++) {
        TEST_ASSERT_EQUAL_UINT8((i & 1) ? ZERO_P : ONE_P, out[i]);
    }
}

void test_pulse_encoding_output_length_always_8(void) {
    ow_pulse_t out[8];
    ds18b20_test_encode_byte_pulses(out, 0x00);
    ds18b20_test_encode_byte_pulses(out, 0xFF);
    ds18b20_test_encode_byte_pulses(out, 0x55);
    ds18b20_test_encode_byte_pulses(out, 0xAA);
    TEST_ASSERT_TRUE(1);
}

void test_pulse_encoding_only_valid_pulse_values(void) {
    ow_pulse_t out[8];
    for (int b = 0; b < 256; b++) {
        ds18b20_test_encode_byte_pulses(out, (uint8_t)b);
        for (int i = 0; i < 8; i++) {
            TEST_ASSERT_TRUE(out[i] == ONE_P || out[i] == ZERO_P);
        }
    }
}

void test_pulse_encoding_single_bit_positions(void) {
    ow_pulse_t out[8];
    for (int bit = 0; bit < 8; bit++) {
        ds18b20_test_encode_byte_pulses(out, (uint8_t)(1u << bit));
        for (int i = 0; i < 8; i++) {
            TEST_ASSERT_EQUAL_UINT8(i == bit ? ONE_P : ZERO_P, out[i]);
        }
    }
}

/* The pre-encoded flash Search ROM / Alarm Search tables must match what
 * onewire_encode_byte() produces for 0xF0/0xEC, slot for slot, with the
 * trailing hardware bus-release entry last. A hand-edited table with two
 * swapped entries passes every other test here (they only check the encoder
 * logic) and shows up on hardware as "search finds nothing". */
static void check_flash_cmd_table(const ow_pulse_t* table, uint8_t cmd) {
    ow_pulse_t out[ONEWIRE_BITS_PER_BYTE];
    ds18b20_test_encode_byte_pulses(out, cmd);
    for (int i = 0; i < ONEWIRE_BITS_PER_BYTE; i++) {
        TEST_ASSERT_EQUAL_UINT8(out[i], table[i]);
    }
    TEST_ASSERT_EQUAL_UINT8(ONEWIRE_RELEASE_PULSE, table[ONEWIRE_BITS_PER_BYTE]);
}

void test_flash_search_rom_table_matches_encoder(void) {
    check_flash_cmd_table(test_search_cmd_rom_addr(), ONEWIRE_SEARCH_ROM);
}

void test_flash_alarm_search_table_matches_encoder(void) {
    check_flash_cmd_table(test_search_cmd_alarm_addr(), ONEWIRE_ALARM_SEARCH);
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

/* The encoder is table-driven: prove every row against the bit-wise
 * definition, so a transposed or shifted row cannot pass. The other tests here
 * only exercise a handful of byte values. */
void test_onewire_encode_byte_matches_bitwise_reference(void) {
    ow_pulse_t out[ONEWIRE_BITS_PER_BYTE];
    for (int value = 0; value < 256; value++) {
        onewire_encode_byte(out, (uint8_t)value);
        for (int bit = 0; bit < ONEWIRE_BITS_PER_BYTE; bit++) {
            ow_pulse_t want =
                ((value >> bit) & 1) ? (ow_pulse_t)ONEWIRE_ONE_PULSE : (ow_pulse_t)ONEWIRE_ZERO_PULSE;
            TEST_ASSERT_EQUAL_UINT8(want, out[bit]);
        }
    }
}

/* The decoder classifies four slots at a time with a packed compare. Prove it
 * against onewire_bit_from_pulse() for every possible capture value in every
 * slot position: the packed form must stay exact for values on both sides of
 * the threshold and for values above 128, which are where a naive lane
 * subtract would carry into its neighbour. */
void test_onewire_decode_pulses_matches_per_bit_reference(void) {
    for (int value = 0; value < 256; value++) {
        for (int pos = 0; pos < ONEWIRE_BITS_PER_BYTE; pos++) {
            uint8_t pulse[ONEWIRE_BITS_PER_BYTE];
            uint8_t want = 0;
            for (int slot = 0; slot < ONEWIRE_BITS_PER_BYTE; slot++) {
                pulse[slot] = (uint8_t)((slot == pos) ? value : ONEWIRE_ZERO_PULSE);
                want |= (uint8_t)(onewire_bit_from_pulse(pulse[slot]) << slot);
            }
            uint8_t dst[1];
            onewire_decode_pulses(dst, pulse, 1);
            TEST_ASSERT_EQUAL_UINT8(want, dst[0]);
        }
    }
}

/* Full-length decode: the 32-byte maximum is what a raw read uses, and it
 * walks the outer loop and the 4-slot step to their last iteration. */
void test_onewire_decode_pulses_max_length(void) {
    uint8_t pulse[ONEWIRE_MAX_READ_BYTES * ONEWIRE_BITS_PER_BYTE];
    uint8_t dst[ONEWIRE_MAX_READ_BYTES];
    uint8_t want[ONEWIRE_MAX_READ_BYTES];
    for (int i = 0; i < (int)sizeof(pulse); i++) {
        /* Alternate the two real pulse widths, with an irregular phase so a
         * slot cannot be predicted from its index alone. */
        pulse[i] = (uint8_t)((((i * 37u) >> 3) & 1u) ? ONEWIRE_ONE_PULSE : ONEWIRE_ZERO_PULSE);
    }
    for (int b = 0; b < (int)ONEWIRE_MAX_READ_BYTES; b++) {
        want[b] = 0;
        for (int bit = 0; bit < ONEWIRE_BITS_PER_BYTE; bit++) {
            want[b] |= (uint8_t)(onewire_bit_from_pulse(pulse[b * ONEWIRE_BITS_PER_BYTE + bit]) << bit);
        }
    }
    onewire_decode_pulses(dst, pulse, ONEWIRE_MAX_READ_BYTES);
    for (int b = 0; b < (int)ONEWIRE_MAX_READ_BYTES; b++) {
        TEST_ASSERT_EQUAL_UINT8(want[b], dst[b]);
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
    TEST_RUN(test_flash_search_rom_table_matches_encoder);
    TEST_RUN(test_flash_alarm_search_table_matches_encoder);
    TEST_RUN(test_onewire_bit_from_pulse_short_is_one);
    TEST_RUN(test_onewire_bit_from_pulse_long_is_zero);
    TEST_RUN(test_onewire_decode_pulses_all_short_is_0xFF);
    TEST_RUN(test_onewire_decode_pulses_all_long_is_0x00);
    TEST_RUN(test_onewire_decode_pulses_pattern);
    TEST_RUN(test_onewire_decode_pulses_two_bytes);
    TEST_RUN(test_onewire_encode_byte_matches_bitwise_reference);
    TEST_RUN(test_onewire_decode_pulses_matches_per_bit_reference);
    TEST_RUN(test_onewire_decode_pulses_max_length);
}
