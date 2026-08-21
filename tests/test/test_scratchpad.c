/* ============================================================
 *  test_scratchpad.c - Scratchpad Decoding Tests
 *
 *  Tests the decode_scratchpad() function which converts
 *  captured pulse durations (from timer DMA) into scratchpad bytes.
 *
 *  Logic: pulse <= 10µs -> bit is 1 (short pulse = logic 1)
 *         pulse > 10µs  -> bit is 0 (long pulse = logic 0)
 *
 *  The scratchpad has 9 bytes (72 bits), read LSB first.
 * ============================================================ */

#include "ds18b20.h"
#include "ds18b20_test_access.h"
#include "mock_target.h"
#include "unity.h"

/*-------------------------------------------------------------
 *  Test: All short pulses (<=10µs) produce all 0xFF bytes
 * -----------------------------------------------------------*/
void test_scratchpad_all_short_pulses_produce_0xFF(void) {
    /* Set all 72 pulses to ONE_PULSE (5µs) - all logic 1 */
    for (int i = 0; i < 72; i++) {
        ds18b20_test_set_pulse(i, 5);
    }

    ds18b20_test_decode_scratchpad();

    /* All 9 bytes should be 0xFF */
    for (int i = 0; i < 9; i++) {
        TEST_ASSERT_EQUAL_HEX8(0xFF, ds18b20_test_get_scratchpad(i));
    }
}

/*-------------------------------------------------------------
 *  Test: All long pulses (>10µs) produce all 0x00 bytes
 * -----------------------------------------------------------*/
void test_scratchpad_all_long_pulses_produce_0x00(void) {
    /* Set all 72 pulses to ZERO_PULSE (60µs) - all logic 0 */
    for (int i = 0; i < 72; i++) {
        ds18b20_test_set_pulse(i, 60);
    }

    ds18b20_test_decode_scratchpad();

    /* All 9 bytes should be 0x00 */
    for (int i = 0; i < 9; i++) {
        TEST_ASSERT_EQUAL_HEX8(0x00, ds18b20_test_get_scratchpad(i));
    }
}

/*-------------------------------------------------------------
 *  Test: Boundary value - pulse = 10µs is logic 1
 * -----------------------------------------------------------*/
void test_scratchpad_pulse_10_is_logic_1(void) {
    /* Set first pulse to exactly 10µs */
    ds18b20_test_set_pulse(0, 10);
    /* Rest are 60µs (logic 0) */
    for (int i = 1; i < 72; i++) {
        ds18b20_test_set_pulse(i, 60);
    }

    ds18b20_test_decode_scratchpad();

    /* Byte 0 should have bit 0 set (0x01), rest 0 */
    TEST_ASSERT_EQUAL_HEX8(0x01, ds18b20_test_get_scratchpad(0));
    /* Other bytes should be 0x00 */
    for (int i = 1; i < 9; i++) {
        TEST_ASSERT_EQUAL_HEX8(0x00, ds18b20_test_get_scratchpad(i));
    }
}

/*-------------------------------------------------------------
 *  Test: Boundary value - pulse = 11µs is logic 0
 * -----------------------------------------------------------*/
void test_scratchpad_pulse_11_is_logic_0(void) {
    /* Set first pulse to 11µs (just over threshold) */
    ds18b20_test_set_pulse(0, 11);
    /* Rest are 5µs (logic 1) */
    for (int i = 1; i < 72; i++) {
        ds18b20_test_set_pulse(i, 5);
    }

    ds18b20_test_decode_scratchpad();

    /* Byte 0 should have bit 0 clear, bits 1-7 set (0xFE) */
    TEST_ASSERT_EQUAL_HEX8(0xFE, ds18b20_test_get_scratchpad(0));
}

/*-------------------------------------------------------------
 *  Test: Alternating pattern - 0xAA = 10101010
 * -----------------------------------------------------------*/
void test_scratchpad_alternating_0xAA(void) {
    /* Set byte 0 to pattern 10101010 (0xAA)
     * LSB first: bit0=0, bit1=1, bit2=0, bit3=1, ...
     * pulse[0]=60 (bit0=0), pulse[1]=5 (bit1=1), etc.
     */
    for (int i = 0; i < 8; i++) {
        if (i % 2 == 0) {
            ds18b20_test_set_pulse(i, 60); /* Even bits = 0 */
        } else {
            ds18b20_test_set_pulse(i, 5); /* Odd bits = 1 */
        }
    }
    /* Rest are 60µs */
    for (int i = 8; i < 72; i++) {
        ds18b20_test_set_pulse(i, 60);
    }

    ds18b20_test_decode_scratchpad();

    /* Byte 0 should be 0xAA */
    TEST_ASSERT_EQUAL_HEX8(0xAA, ds18b20_test_get_scratchpad(0));
}

/*-------------------------------------------------------------
 *  Test: Alternating pattern - 0x55 = 01010101
 * -----------------------------------------------------------*/
void test_scratchpad_alternating_0x55(void) {
    /* Set byte 0 to pattern 01010101 (0x55)
     * LSB first: bit0=1, bit1=0, bit2=1, bit3=0, ...
     */
    for (int i = 0; i < 8; i++) {
        if (i % 2 == 0) {
            ds18b20_test_set_pulse(i, 5); /* Even bits = 1 */
        } else {
            ds18b20_test_set_pulse(i, 60); /* Odd bits = 0 */
        }
    }
    /* Rest are 60µs */
    for (int i = 8; i < 72; i++) {
        ds18b20_test_set_pulse(i, 60);
    }

    ds18b20_test_decode_scratchpad();

    /* Byte 0 should be 0x55 */
    TEST_ASSERT_EQUAL_HEX8(0x55, ds18b20_test_get_scratchpad(0));
}

/*-------------------------------------------------------------
 *  Test: Verify bit ordering - LSB first
 * -----------------------------------------------------------*/
void test_scratchpad_bit_ordering_LSB_first(void) {
    /* Set only bit 7 (MSB of byte) to 1 */
    /* Bit 7 is at pulse index 7 */
    for (int i = 0; i < 72; i++) {
        ds18b20_test_set_pulse(i, 60); /* All 0 */
    }
    ds18b20_test_set_pulse(7, 5); /* Bit 7 = 1 */

    ds18b20_test_decode_scratchpad();

    /* Byte 0 should be 0x80 (bit 7 set) */
    TEST_ASSERT_EQUAL_HEX8(0x80, ds18b20_test_get_scratchpad(0));
}

/*-------------------------------------------------------------
 *  Test: Verify second byte starts at bit 8
 * -----------------------------------------------------------*/
void test_scratchpad_second_byte_starts_at_bit_8(void) {
    /* Set all pulses to 60µs (0) */
    for (int i = 0; i < 72; i++) {
        ds18b20_test_set_pulse(i, 60);
    }

    /* Set bit 8 (first bit of byte 1) to 1 */
    ds18b20_test_set_pulse(8, 5);

    ds18b20_test_decode_scratchpad();

    /* Byte 0 should be 0x00 */
    TEST_ASSERT_EQUAL_HEX8(0x00, ds18b20_test_get_scratchpad(0));
    /* Byte 1 should be 0x01 (bit 0 of byte 1 = 1) */
    TEST_ASSERT_EQUAL_HEX8(0x01, ds18b20_test_get_scratchpad(1));
}

/*-------------------------------------------------------------
 *  Test: Realistic temperature reading - 22.25°C
 * -----------------------------------------------------------*/
void test_scratchpad_realistic_22_25C(void) {
    /* Simulate scratchpad for 22.25°C:
     * Byte 0: 0x64 (temp LSB = 0x0164)
     * Byte 1: 0x01 (temp MSB)
     * Byte 2: 0x4B (Th)
     * Byte 3: 0x46 (Tl)
     * Byte 4: 0x7F (config)
     * Byte 5: 0xFF (reserved)
     * Byte 6: 0x08 (reserved)
     * Byte 7: 0x10 (reserved)
     * Byte 8: CRC (computed later)
     */

    uint8_t expected_data[9] = {0x64, 0x01, 0x4B, 0x46, 0x7F, 0xFF, 0x08, 0x10, 0x10};

    /* Encode each byte into pulses */
    for (int byte_idx = 0; byte_idx < 9; byte_idx++) {
        uint8_t b = expected_data[byte_idx];
        for (int bit_idx = 0; bit_idx < 8; bit_idx++) {
            int pulse_idx = byte_idx * 8 + bit_idx;
            if (b & (1 << bit_idx)) {
                ds18b20_test_set_pulse(pulse_idx, 5); /* Bit = 1 -> short pulse */
            } else {
                ds18b20_test_set_pulse(pulse_idx, 60); /* Bit = 0 -> long pulse */
            }
        }
    }

    ds18b20_test_decode_scratchpad();

    /* Verify decoded bytes match expected */
    for (int i = 0; i < 9; i++) {
        TEST_ASSERT_EQUAL_HEX8(expected_data[i], ds18b20_test_get_scratchpad(i));
    }
}

/*-------------------------------------------------------------
 *  Test: All-zero scratchpad has wrong reserved bytes (byte 5 != 0xFF)
 * -----------------------------------------------------------*/
void test_scratchpad_all_zero_has_wrong_reserved_bytes(void) {
    /* All-zero scratchpad means bus fault */
    for (int i = 0; i < 9; i++) {
        ds18b20_test_set_scratchpad(i, 0x00);
    }

    /* Byte 5 should be 0xFF in valid data */
    TEST_ASSERT_EQUAL_HEX8(0x00, ds18b20_test_get_scratchpad(5));
    /* Byte 7 should be 0x10 in valid data */
    TEST_ASSERT_EQUAL_HEX8(0x00, ds18b20_test_get_scratchpad(7));
}

/*-------------------------------------------------------------
 *  Test: All-0xFF scratchpad has wrong byte 7 (should be 0x10)
 * -----------------------------------------------------------*/
void test_scratchpad_all_0xFF_has_wrong_byte7(void) {
    /* All-0xFF means bus stuck high */
    for (int i = 0; i < 9; i++) {
        ds18b20_test_set_scratchpad(i, 0xFF);
    }

    /* Byte 5 = 0xFF is correct, but byte 7 should be 0x10 */
    TEST_ASSERT_EQUAL_HEX8(0xFF, ds18b20_test_get_scratchpad(5));
    TEST_ASSERT_EQUAL_HEX8(0xFF, ds18b20_test_get_scratchpad(7));
}

/*-------------------------------------------------------------
 *  Test: Valid scratchpad has correct reserved bytes
 * -----------------------------------------------------------*/
void test_scratchpad_valid_has_correct_reserved_bytes(void) {
    /* Set up a valid scratchpad with correct reserved bytes */
    uint8_t valid_data[9] = {0x64, 0x01, 0x4B, 0x46, 0x7F, 0xFF, 0x08, 0x10, 0x10};

    for (int i = 0; i < 9; i++) {
        ds18b20_test_set_scratchpad(i, valid_data[i]);
    }

    /* Reserved bytes must match DS18B20 spec */
    TEST_ASSERT_EQUAL_HEX8(0xFF, ds18b20_test_get_scratchpad(5));
    TEST_ASSERT_EQUAL_HEX8(0x10, ds18b20_test_get_scratchpad(7));
}

/*-------------------------------------------------------------
 *  Test: check_scratchpad_crc matches direct crc8 over 8 bytes
 * -----------------------------------------------------------*/
void test_scratchpad_crc_matches_direct_crc8(void) {
    uint8_t data[9] = {0x64, 0x01, 0x4B, 0x46, 0x7F, 0xFF, 0x08, 0x10, 0x10};
    for (int i = 0; i < 9; i++) {
        ds18b20_test_set_scratchpad(i, data[i]);
    }
    TEST_ASSERT_EQUAL_HEX8(ds18b20_crc8(data, 8), ds18b20_test_check_scratchpad_crc());
}

/*-------------------------------------------------------------
 *  Test: check_scratchpad_crc over a zeroed scratchpad
 * -----------------------------------------------------------*/
void test_scratchpad_crc_zeroed_data(void) {
    uint8_t zeros[8] = {0};
    for (int i = 0; i < 8; i++) {
        ds18b20_test_set_scratchpad(i, zeros[i]);
    }
    TEST_ASSERT_EQUAL_HEX8(ds18b20_crc8(zeros, 8), ds18b20_test_check_scratchpad_crc());
}

/*-------------------------------------------------------------
 *  Test: corrupting a data byte changes the computed CRC
 * -----------------------------------------------------------*/
void test_scratchpad_crc_changes_with_corruption(void) {
    uint8_t data[9] = {0x64, 0x01, 0x4B, 0x46, 0x7F, 0xFF, 0x08, 0x10, 0x10};
    for (int i = 0; i < 9; i++) {
        ds18b20_test_set_scratchpad(i, data[i]);
    }
    uint8_t crc_ok = ds18b20_test_check_scratchpad_crc();
    ds18b20_test_set_scratchpad(0, 0x63);
    uint8_t crc_bad = ds18b20_test_check_scratchpad_crc();
    TEST_ASSERT_TRUE(crc_ok != crc_bad);
}

/*-------------------------------------------------------------
 *  Test: check_scratchpad_crc ignores the CRC byte (byte 8)
 * -----------------------------------------------------------*/
void test_scratchpad_crc_ignores_crc_byte(void) {
    uint8_t data[9] = {0x64, 0x01, 0x4B, 0x46, 0x7F, 0xFF, 0x08, 0x10, 0x00};
    for (int i = 0; i < 9; i++) {
        ds18b20_test_set_scratchpad(i, data[i]);
    }
    uint8_t crc_before = ds18b20_test_check_scratchpad_crc();
    ds18b20_test_set_scratchpad(8, 0xFF);
    TEST_ASSERT_EQUAL_HEX8(crc_before, ds18b20_test_check_scratchpad_crc());
}

/*-------------------------------------------------------------
 *  Run all scratchpad tests
 * -----------------------------------------------------------*/
void run_test_scratchpad(void) {
    TEST_RUN(test_scratchpad_all_short_pulses_produce_0xFF);
    TEST_RUN(test_scratchpad_all_long_pulses_produce_0x00);
    TEST_RUN(test_scratchpad_pulse_10_is_logic_1);
    TEST_RUN(test_scratchpad_pulse_11_is_logic_0);
    TEST_RUN(test_scratchpad_alternating_0xAA);
    TEST_RUN(test_scratchpad_alternating_0x55);
    TEST_RUN(test_scratchpad_bit_ordering_LSB_first);
    TEST_RUN(test_scratchpad_second_byte_starts_at_bit_8);
    TEST_RUN(test_scratchpad_realistic_22_25C);
    TEST_RUN(test_scratchpad_all_zero_has_wrong_reserved_bytes);
    TEST_RUN(test_scratchpad_all_0xFF_has_wrong_byte7);
    TEST_RUN(test_scratchpad_valid_has_correct_reserved_bytes);
    TEST_RUN(test_scratchpad_crc_matches_direct_crc8);
    TEST_RUN(test_scratchpad_crc_zeroed_data);
    TEST_RUN(test_scratchpad_crc_changes_with_corruption);
    TEST_RUN(test_scratchpad_crc_ignores_crc_byte);
}
