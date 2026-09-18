/* ============================================================
 *  test_rom_addressing.c - ROM Addressing Tests
 *
 *  Tests ds18b20_select(), build_addr_prefix() and build_addr_cmd():
 *  Match ROM (0x55) + 8-byte ROM prefix + command byte, laid out as
 *  the 10-byte command array ctx.addr_bytes[0..9].
 * ============================================================ */

#include "ds18b20.h"
#include "ds18b20_test_access.h"
#include "onewire.h"
#include "unity.h"

/* ctx.addr_bytes holds 0x55 + 8 ROM bytes + 1 command byte. */
#define ADDR_CMD_BYTES (DS18B20_ROM_BYTES + 2) /* = 10 */

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

    TEST_ASSERT_EQUAL_HEX8(DS18B20_MATCH_ROM, ds18b20_test_get_addr_byte(0));
}

void test_rom_addressing_prefix_includes_rom(void) {
    uint8_t rom[8] = {0x28, 0xAA, 0x55, 0xFF, 0x00, 0x12, 0x34, 0x56};
    ds18b20_select(rom);

    /* ROM byte i is stored verbatim at addr_bytes[1 + i] */
    for (int i = 0; i < 8; i++) {
        TEST_ASSERT_EQUAL_HEX8(rom[i], ds18b20_test_get_addr_byte((uint8_t)(1 + i)));
    }
}

void test_rom_addressing_cmd_overwrites_last_byte(void) {
    uint8_t rom[8] = {0x28, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07};
    ds18b20_select(rom);
    ds18b20_test_build_addr_cmd(DS18B20_CONVERT_T); /* 0x44 */

    TEST_ASSERT_EQUAL_HEX8(DS18B20_CONVERT_T,
                           ds18b20_test_get_addr_byte((uint8_t)(DS18B20_ROM_BYTES + 1)));
}

void test_rom_addressing_prefix_unchanged_after_cmd(void) {
    uint8_t rom[8] = {0x28, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07};
    ds18b20_select(rom);

    uint8_t saved_prefix[DS18B20_ROM_BYTES + 1];
    for (int i = 0; i < DS18B20_ROM_BYTES + 1; i++) {
        saved_prefix[i] = ds18b20_test_get_addr_byte((uint8_t)i);
    }

    ds18b20_test_build_addr_cmd(DS18B20_READ_SCRATCHPAD);

    for (int i = 0; i < DS18B20_ROM_BYTES + 1; i++) {
        TEST_ASSERT_EQUAL_HEX8(saved_prefix[i], ds18b20_test_get_addr_byte((uint8_t)i));
    }
}

void test_rom_addressing_read_scratchpad_encoding(void) {
    uint8_t rom[8] = {0x28, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07};
    ds18b20_select(rom);
    ds18b20_test_build_addr_cmd(DS18B20_READ_SCRATCHPAD); /* 0xBE */

    TEST_ASSERT_EQUAL_HEX8(DS18B20_READ_SCRATCHPAD,
                           ds18b20_test_get_addr_byte((uint8_t)(DS18B20_ROM_BYTES + 1)));
}

void test_rom_addressing_different_roms_different_prefixes(void) {
    uint8_t rom1[8] = {0x28, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07};
    uint8_t rom2[8] = {0x28, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77};

    ds18b20_select(rom1);
    uint8_t prefix1[ADDR_CMD_BYTES];
    for (int i = 0; i < ADDR_CMD_BYTES; i++) {
        prefix1[i] = ds18b20_test_get_addr_byte((uint8_t)i);
    }

    ds18b20_select(rom2);
    int differs = 0;
    for (int i = 1; i <= DS18B20_ROM_BYTES; i++) {
        if (ds18b20_test_get_addr_byte((uint8_t)i) != prefix1[i]) {
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
 *  Test: B1 - the Match ROM command array ends exactly at the command
 *  byte (addr_bytes[DS18B20_ROM_BYTES + 1]); the bus-release 0 is
 *  appended by onewire_write_command() into its own internal pulse
 *  buffer, so nothing may spill past the 10-byte command array.
 * -----------------------------------------------------------*/
void test_rom_addressing_command_layout_bounded(void) {
    uint8_t rom[8] = {0x28, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07};
    ds18b20_select(rom);
    ds18b20_test_build_addr_cmd(DS18B20_CONVERT_T);

    TEST_ASSERT_EQUAL_HEX8(DS18B20_MATCH_ROM, ds18b20_test_get_addr_byte(0));
    for (int i = 0; i < 8; i++) {
        TEST_ASSERT_EQUAL_HEX8(rom[i], ds18b20_test_get_addr_byte((uint8_t)(1 + i)));
    }
    TEST_ASSERT_EQUAL_HEX8(DS18B20_CONVERT_T,
                           ds18b20_test_get_addr_byte((uint8_t)(ADDR_CMD_BYTES - 1)));
}

void test_select_rejected_while_txn_running(void) {
    ds18b20_init();
    ds18b20_test_reset_ctx();
    ds18b20_test_reset_txn();

    uint8_t rom[8] = {0x28, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07};
    uint8_t buf[8];

    /* Start a command transaction but do not drive it to completion: the bus
     * now belongs to the read_rom txn while the state machine stays IDLE. */
    ds18b20_read_rom(buf);
    TEST_ASSERT_EQUAL_UINT8(0, ds18b20_test_get_txn_finished());

    /* A select() while a command txn owns the bus must be rejected. */
    ds18b20_select(rom);
    TEST_ASSERT_EQUAL_UINT8(0, ds18b20_test_get_address_mode());

    ds18b20_test_reset_txn();
}

void test_select_rejected_during_search(void) {
    ds18b20_init();
    ds18b20_test_reset_ctx();
    ds18b20_test_reset_txn();
    ds18b20_test_reset_search();

    /* A search owns the timer: select() must be rejected even at IDLE. */
    ds18b20_search_start(0, 1);
    TEST_ASSERT_EQUAL_UINT8(0, ds18b20_search_poll()); /* search still running */

    uint8_t rom[8] = {0x28, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07};
    ds18b20_select(rom);
    TEST_ASSERT_EQUAL_UINT8(0, ds18b20_test_get_address_mode());
    ds18b20_test_reset_search();
}

void test_select_rejected_during_resolution_change(void) {
    ds18b20_init();
    ds18b20_test_reset_ctx();
    ds18b20_test_reset_txn();
    ds18b20_test_reset_search();

    /* A resolution change owns the timer: select() must be rejected. */
    ds18b20_set_resolution(9);

    uint8_t rom[8] = {0x28, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07};
    ds18b20_select(rom);
    TEST_ASSERT_EQUAL_UINT8(0, ds18b20_test_get_address_mode());
    ds18b20_test_reset_resolution();
    ds18b20_test_reset_txn();
}

void test_select_rejected_from_scan_callback(void) {
    ds18b20_init();
    ds18b20_test_reset_ctx();
    ds18b20_test_reset_txn();
    ds18b20_test_reset_search();

    /* Mimic the per-device scan callback context: state is DECODE while a scan
     * is in progress. A select() there must be rejected and the scan round must
     * continue (scan mode unchanged). */
    ds18b20_test_set_scan_mode(1);
    ds18b20_test_set_state(DS18B20_ST_DECODE);

    uint8_t rom[8] = {0x28, 0x0A, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07};
    ds18b20_select(rom);
    TEST_ASSERT_EQUAL_UINT8(0, ds18b20_test_get_address_mode());
    TEST_ASSERT_EQUAL_UINT8(1, ds18b20_test_get_scan_mode());
}

void run_test_rom_addressing(void) {
    TEST_RUN(test_rom_addressing_select_NULL_clears_mode);
    TEST_RUN(test_rom_addressing_select_copies_rom);
    TEST_RUN(test_rom_addressing_prefix_starts_with_match_rom);
    TEST_RUN(test_rom_addressing_prefix_includes_rom);
    TEST_RUN(test_rom_addressing_cmd_overwrites_last_byte);
    TEST_RUN(test_rom_addressing_prefix_unchanged_after_cmd);
    TEST_RUN(test_rom_addressing_read_scratchpad_encoding);
    TEST_RUN(test_rom_addressing_different_roms_different_prefixes);
    TEST_RUN(test_rom_addressing_select_ignored_mid_cycle);
    TEST_RUN(test_rom_addressing_command_layout_bounded);
    TEST_RUN(test_select_rejected_while_txn_running);
    TEST_RUN(test_select_rejected_during_search);
    TEST_RUN(test_select_rejected_during_resolution_change);
    TEST_RUN(test_select_rejected_from_scan_callback);
}
