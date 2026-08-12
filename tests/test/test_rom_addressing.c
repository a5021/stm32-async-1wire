/* ============================================================
 *  test_rom_addressing.c - ROM Addressing Tests
 *
 *  Tests ds18b20_select(), build_addr_prefix() and build_addr_cmd():
 *  Match ROM (0x55) + 8-byte ROM prefix (72 slots) + command byte
 *  appended at slots 72-79.
 * ============================================================ */

#include "ds18b20.h"
#include "ds18b20_test_access.h"
#include "unity.h"

#define ONE_P 5u
#define ZERO_P 60u

/* Mirror of the driver-internal DS18B20_MATCH_SLOTS so bounds tests use the
 * real slot count (= (DS18B20_ROM_BYTES + 2) * 8 = 80) rather than a literal. */
#define ADDR_CMD_SLOTS ((DS18B20_ROM_BYTES + 2) * 8)

void test_rom_addressing_select_NULL_clears_mode(void) {
    uint8_t rom[8] = {0x28, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07};
    ds18b20_select(rom);
    TEST_ASSERT_EQUAL_UINT8(1, ds18b20_test_get_address_mode());

    ds18b20_select(NULL);
    TEST_ASSERT_EQUAL_UINT8(0, ds18b20_test_get_address_mode());
}

void test_rom_addressing_select_copies_rom(void) {
    uint8_t rom[8] = {0x28, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07};
    ds18b20_select(rom);

    uint8_t selected_rom[8];
    ds18b20_test_get_selected_rom(selected_rom);
    for (int i = 0; i < 8; i++) {
        TEST_ASSERT_EQUAL_UINT8(rom[i], selected_rom[i]);
    }
    TEST_ASSERT_EQUAL_UINT8(1, ds18b20_test_get_address_mode());
}

void test_rom_addressing_prefix_starts_with_match_rom(void) {
    uint8_t rom[8] = {0x28, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07};
    ds18b20_select(rom);

    /* 0x55 = 01010101, LSB first: 1,0,1,0,1,0,1,0 */
    for (int i = 0; i < 8; i++) {
        TEST_ASSERT_EQUAL_UINT8((i & 1) ? ZERO_P : ONE_P, ds18b20_test_get_addr_cmd((uint8_t)i));
    }
}

void test_rom_addressing_prefix_includes_rom(void) {
    uint8_t rom[8] = {0x28, 0xAA, 0x55, 0xFF, 0x00, 0x12, 0x34, 0x56};
    ds18b20_select(rom);

    /* ROM byte 0 (0x28 = 00101000, LSB first: 0,0,0,1,0,1,0,0) at slots 8-15 */
    uint8_t exp[8] = {ZERO_P, ZERO_P, ZERO_P, ONE_P, ZERO_P, ONE_P, ZERO_P, ZERO_P};
    for (int i = 0; i < 8; i++) {
        TEST_ASSERT_EQUAL_UINT8(exp[i], ds18b20_test_get_addr_cmd((uint8_t)(8 + i)));
    }
}

void test_rom_addressing_cmd_overwrites_last_8_slots(void) {
    uint8_t rom[8] = {0x28, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07};
    ds18b20_select(rom);
    ds18b20_test_build_addr_cmd(DS18B20_CONVERT_T); /* 0x44 */

    /* 0x44 = 01000100, LSB first: 0,0,1,0,0,0,1,0 at slots 72-79 */
    uint8_t exp[8] = {ZERO_P, ZERO_P, ONE_P, ZERO_P, ZERO_P, ZERO_P, ONE_P, ZERO_P};
    for (int i = 0; i < 8; i++) {
        TEST_ASSERT_EQUAL_UINT8(exp[i], ds18b20_test_get_addr_cmd((uint8_t)(72 + i)));
    }
}

void test_rom_addressing_prefix_unchanged_after_cmd(void) {
    uint8_t rom[8] = {0x28, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07};
    ds18b20_select(rom);

    uint8_t saved_prefix[72];
    for (int i = 0; i < 72; i++) {
        saved_prefix[i] = ds18b20_test_get_addr_cmd((uint8_t)i);
    }

    ds18b20_test_build_addr_cmd(DS18B20_READ_SCRATCHPAD);

    for (int i = 0; i < 72; i++) {
        TEST_ASSERT_EQUAL_UINT8(saved_prefix[i], ds18b20_test_get_addr_cmd((uint8_t)i));
    }
}

void test_rom_addressing_read_scratchpad_encoding(void) {
    uint8_t rom[8] = {0x28, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07};
    ds18b20_select(rom);
    ds18b20_test_build_addr_cmd(DS18B20_READ_SCRATCHPAD); /* 0xBE */

    /* 0xBE = 10111110, LSB first: 0,1,1,1,1,1,0,1 at slots 72-79 */
    uint8_t exp[8] = {ZERO_P, ONE_P, ONE_P, ONE_P, ONE_P, ONE_P, ZERO_P, ONE_P};
    for (int i = 0; i < 8; i++) {
        TEST_ASSERT_EQUAL_UINT8(exp[i], ds18b20_test_get_addr_cmd((uint8_t)(72 + i)));
    }
}

void test_rom_addressing_different_roms_different_prefixes(void) {
    uint8_t rom1[8] = {0x28, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07};
    uint8_t rom2[8] = {0x28, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77};

    ds18b20_select(rom1);
    uint8_t prefix1[80];
    for (int i = 0; i < 80; i++) {
        prefix1[i] = ds18b20_test_get_addr_cmd((uint8_t)i);
    }

    ds18b20_select(rom2);
    int differs = 0;
    for (int i = 8; i < 72; i++) {
        if (ds18b20_test_get_addr_cmd((uint8_t)i) != prefix1[i]) {
            differs = 1;
            break;
        }
    }
    TEST_ASSERT_TRUE(differs);
}

/*-------------------------------------------------------------
 *  Test: select() is ignored mid-cycle, applied at IDLE
 *  (R2 fix: applying it mid-cycle would corrupt in-flight Match ROM DMA)
 * -----------------------------------------------------------*/
void test_rom_addressing_select_ignored_mid_cycle(void) {
    ds18b20_init();
    ds18b20_test_reset_ctx();

    uint8_t rom[8] = {0x28, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07};

    /* Mid-cycle (non-IDLE state): selection must be rejected */
    ds18b20_test_set_state(DS18B20_ST_WAIT);
    ds18b20_select(rom);
    TEST_ASSERT_EQUAL_UINT8(0, ds18b20_test_get_address_mode());

    /* Back to IDLE: selection is applied */
    ds18b20_test_set_state(DS18B20_ST_IDLE);
    ds18b20_select(rom);
    TEST_ASSERT_EQUAL_UINT8(1, ds18b20_test_get_address_mode());

    uint8_t selected_rom[8];
    ds18b20_test_get_selected_rom(selected_rom);
    for (int i = 0; i < 8; i++) {
        TEST_ASSERT_EQUAL_UINT8(rom[i], selected_rom[i]);
    }
}

/*-------------------------------------------------------------
 *  Test: B1 - trailing bus-release sentinel (addr_cmd[DS18B20_MATCH_SLOTS])
 *  stays 0. send_command_n() reads that slot as the final zero-pulse that
 *  releases the 1-Wire bus; if it were ever non-zero or written out of
 *  bounds the last slot would glitch. build_addr_prefix() now zeroes it
 *  explicitly and the buffer is sized DS18B20_MATCH_SLOTS + 1.
 * -----------------------------------------------------------*/
void test_rom_addressing_trailing_bus_release_sentinel(void) {
    uint8_t rom[8] = {0x28, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07};
    ds18b20_select(rom);
    ds18b20_test_build_addr_cmd(DS18B20_CONVERT_T);

    /* Sentinel at index DS18B20_MATCH_SLOTS (== ADDR_CMD_SLOTS) must be 0. */
    TEST_ASSERT_EQUAL_UINT8(0, ds18b20_test_get_addr_cmd((uint8_t)ADDR_CMD_SLOTS));

    /* And the address bytes themselves must not spill past slot 79. */
    uint8_t marker = ds18b20_test_get_addr_cmd((uint8_t)(ADDR_CMD_SLOTS - 1));
    TEST_ASSERT_TRUE(marker == ONE_P || marker == ZERO_P);
}

void run_test_rom_addressing(void) {
    TEST_RUN(test_rom_addressing_select_NULL_clears_mode);
    TEST_RUN(test_rom_addressing_select_copies_rom);
    TEST_RUN(test_rom_addressing_prefix_starts_with_match_rom);
    TEST_RUN(test_rom_addressing_prefix_includes_rom);
    TEST_RUN(test_rom_addressing_cmd_overwrites_last_8_slots);
    TEST_RUN(test_rom_addressing_prefix_unchanged_after_cmd);
    TEST_RUN(test_rom_addressing_read_scratchpad_encoding);
    TEST_RUN(test_rom_addressing_different_roms_different_prefixes);
    TEST_RUN(test_rom_addressing_select_ignored_mid_cycle);
    TEST_RUN(test_rom_addressing_trailing_bus_release_sentinel);
}
