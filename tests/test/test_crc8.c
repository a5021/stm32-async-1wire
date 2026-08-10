/* ============================================================
 *  test_crc8.c - CRC-8 Dallas/Maxim Algorithm Tests
 * ============================================================ */

#include "ds18b20.h"
#include "unity.h"

void test_crc8_empty_buffer_returns_zero(void) {
    uint8_t data[1] = {0};
    TEST_ASSERT_EQUAL_UINT8(0, ds18b20_crc8(data, 0));
}

void test_crc8_single_zero_byte_returns_zero(void) {
    uint8_t data[1] = {0x00};
    TEST_ASSERT_EQUAL_UINT8(0, ds18b20_crc8(data, 1));
}

void test_crc8_valid_scratchpad_returns_zero(void) {
    uint8_t scratchpad[9] = {0x64, 0x01, 0x4B, 0x46, 0x7F, 0xFF, 0x08, 0x10, 0x00};
    uint8_t expected_crc = ds18b20_crc8(scratchpad, 8);
    scratchpad[8] = expected_crc;
    TEST_ASSERT_EQUAL_UINT8(0, ds18b20_crc8(scratchpad, 9));
}

void test_crc8_corrupted_data_returns_nonzero(void) {
    uint8_t scratchpad[9] = {0x64, 0x01, 0x4B, 0x46, 0x7F, 0xFF, 0x08, 0x10, 0x10};
    scratchpad[0] ^= 0x01;
    TEST_ASSERT_NOT_EQUAL(0, ds18b20_crc8(scratchpad, 9));
}

void test_crc8_maxim_vector(void) {
    uint8_t data[8] = {0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08};
    TEST_ASSERT_EQUAL_HEX8(0x83, ds18b20_crc8(data, 8));
}

void test_crc8_single_bit_changes_produce_different_crc(void) {
    uint8_t data1[4] = {0x00, 0x00, 0x00, 0x00};
    uint8_t data2[4] = {0x01, 0x00, 0x00, 0x00};
    TEST_ASSERT_NOT_EQUAL(ds18b20_crc8(data1, 4), ds18b20_crc8(data2, 4));
}

void test_crc8_is_deterministic(void) {
    uint8_t data[5] = {0xAA, 0x55, 0xFF, 0x00, 0x0F};
    TEST_ASSERT_EQUAL_UINT8(ds18b20_crc8(data, 5), ds18b20_crc8(data, 5));
}

void test_crc8_different_lengths_produce_different_results(void) {
    uint8_t data[4] = {0x01, 0x02, 0x03, 0x04};
    uint8_t crc_1 = ds18b20_crc8(data, 1);
    uint8_t crc_2 = ds18b20_crc8(data, 2);
    uint8_t crc_3 = ds18b20_crc8(data, 3);
    uint8_t crc_4 = ds18b20_crc8(data, 4);
    TEST_ASSERT_NOT_EQUAL(crc_1, crc_2);
    TEST_ASSERT_NOT_EQUAL(crc_2, crc_3);
    TEST_ASSERT_NOT_EQUAL(crc_3, crc_4);
}

void test_crc8_all_0xFF_bytes(void) {
    uint8_t data[8] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
    TEST_ASSERT_NOT_EQUAL(0, ds18b20_crc8(data, 8));
}

void run_test_crc8(void) {
    TEST_RUN(test_crc8_empty_buffer_returns_zero);
    TEST_RUN(test_crc8_single_zero_byte_returns_zero);
    TEST_RUN(test_crc8_valid_scratchpad_returns_zero);
    TEST_RUN(test_crc8_corrupted_data_returns_nonzero);
    TEST_RUN(test_crc8_maxim_vector);
    TEST_RUN(test_crc8_single_bit_changes_produce_different_crc);
    TEST_RUN(test_crc8_is_deterministic);
    TEST_RUN(test_crc8_different_lengths_produce_different_results);
    TEST_RUN(test_crc8_all_0xFF_bytes);
}
