/* ============================================================
 *  test_rom_addressing.c - ROM Addressing Tests
 *
 *  Tests ds18b20_select() and build_addr_cmd(): select() only stores
 *  the ROM, and each build_addr_cmd() call rebuilds the full table —
 *  Match ROM (0x55) + 8-byte ROM prefix (72 slots) + command byte
 *  appended at slots 72-79 — so a transaction reusing the shared
 *  phase union between operations can never leave a stale prefix.
 * ============================================================ */

#include "ds18b20.h"
#include "ds18b20_test_access.h"
#include "hw_model.h"
#include "mock_target.h"
#include "onewire.h"
#include "unity.h"

#define ONE_P ONEWIRE_ONE_PULSE
#define ZERO_P ONEWIRE_ZERO_PULSE

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
    ds18b20_test_build_addr_cmd(DS18B20_CONVERT_T);

    /* 0x55 = 01010101, LSB first: 1,0,1,0,1,0,1,0 */
    for (int i = 0; i < 8; i++) {
        TEST_ASSERT_EQUAL_UINT8((i & 1) ? ZERO_P : ONE_P, ds18b20_test_get_addr_cmd((uint8_t)i));
    }
}

void test_rom_addressing_prefix_includes_rom(void) {
    uint8_t rom[8] = {0x28, 0xAA, 0x55, 0xFF, 0x00, 0x12, 0x34, 0x56};
    ds18b20_select(rom);
    ds18b20_test_build_addr_cmd(DS18B20_CONVERT_T);

    /* ROM byte 0 (0x28 = 00101000, LSB first: 0,0,0,1,0,1,0,0) at slots 8-15 */
    for (int i = 0; i < 8; i++) {
        uint8_t expected = ((0x28u >> i) & 1u) ? (uint8_t)ONE_P : (uint8_t)ZERO_P;
        TEST_ASSERT_EQUAL_UINT8(expected, ds18b20_test_get_addr_cmd((uint8_t)(8 + i)));
    }
}

void test_rom_addressing_cmd_overwrites_last_8_slots(void) {
    uint8_t rom[8] = {0x28, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07};
    ds18b20_select(rom);
    ds18b20_test_build_addr_cmd(DS18B20_CONVERT_T); /* 0x44 */

    /* 0x44 = 01000100, LSB first: 0,0,1,0,0,0,1,0 at slots 72-79 */
    for (int i = 0; i < 8; i++) {
        uint8_t expected = ((0x44u >> i) & 1u) ? (uint8_t)ONE_P : (uint8_t)ZERO_P;
        TEST_ASSERT_EQUAL_UINT8(expected, ds18b20_test_get_addr_cmd((uint8_t)(72 + i)));
    }
}

void test_rom_addressing_prefix_unchanged_after_cmd(void) {
    uint8_t rom[8] = {0x28, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07};
    ds18b20_select(rom);
    ds18b20_test_build_addr_cmd(DS18B20_CONVERT_T);

    uint8_t saved_prefix[72];
    for (int i = 0; i < 72; i++) {
        saved_prefix[i] = ds18b20_test_get_addr_cmd((uint8_t)i);
    }

    /* A rebuild for another command re-encodes the same prefix from the
     * stored selection: slots 0..71 must be identical, only 72..79 change. */
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
    for (int i = 0; i < 8; i++) {
        uint8_t expected = ((0xBEu >> i) & 1u) ? (uint8_t)ONE_P : (uint8_t)ZERO_P;
        TEST_ASSERT_EQUAL_UINT8(expected, ds18b20_test_get_addr_cmd((uint8_t)(72 + i)));
    }
}

void test_rom_addressing_different_roms_different_prefixes(void) {
    uint8_t rom1[8] = {0x28, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07};
    uint8_t rom2[8] = {0x28, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77};

    ds18b20_select(rom1);
    ds18b20_test_build_addr_cmd(DS18B20_CONVERT_T);
    uint8_t prefix1[80];
    for (int i = 0; i < 80; i++) {
        prefix1[i] = ds18b20_test_get_addr_cmd((uint8_t)i);
    }

    ds18b20_select(rom2);
    ds18b20_test_build_addr_cmd(DS18B20_CONVERT_T);
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
 *  (R2 fix: applying it mid-cycle would change selected_rom under the
 *  in-flight cycle, so its next addressed operation would rebuild the
 *  Match ROM table for the wrong device)
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
 *  Test: B1 - trailing bus-release sentinel (addr[DS18B20_MATCH_SLOTS])
 *  stays 0. send_command_n() reads that slot as the final zero-pulse that
 *  releases the 1-Wire bus; if it were ever non-zero or written out of
 *  bounds the last slot would glitch. build_addr_cmd() writes it
 *  explicitly on every rebuild and the member is sized
 *  DS18B20_MATCH_SLOTS + 1.
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

/*-------------------------------------------------------------
 *  Union integration: addressed measurement -> bare txn ->
 *  addressed measurement, prefix identical on the wire twice.
 *
 *  The phase union overlays the addressed-command table with the txn
 *  buffer, so a bare transaction between two addressed measurements
 *  reuses the same RAM. The guards keep the two from overlapping, but
 *  only this scenario proves the second measurement rebuilds its Match
 *  ROM prefix on the real path (issue_command -> build_addr_cmd) instead
 *  of feeding a stale union: the CCR3 feed log of the 80-slot command
 *  write is snapshotted in both runs and compared slot for slot.
 * -----------------------------------------------------------*/

/* Presence-present capture source: valid reset (510) and presence (700). */
static uint16_t union_cap_present(uint32_t idx) { return idx == 0 ? 510u : 700u; }

/* Bulk-read pulse durations, filled per phase below (scratchpad or ROM). */
static uint16_t union_read_pulses[72];
static uint16_t union_cap_bulk(uint32_t idx) { return union_read_pulses[idx]; }

static void union_set_bytes_as_pulses(const uint8_t* data, uint8_t len) {
    for (uint8_t i = 0; i < len; i++) {
        for (uint8_t b = 0; b < 8; b++) {
            union_read_pulses[i * 8u + b] = ((data[i] >> b) & 1u) ? ONE_P : ZERO_P;
        }
    }
}

/* Pump the currently scheduled hardware pass. Bulk captures (>2 transfers)
 * read from the pulse table the test filled; every other pass (resets) uses
 * the presence source. Resets the source every pump so a reset following a
 * bulk read cannot consume stale read data. */
static void union_run_current_op(void) {
    if (mock_tim1.CR1 & TIM_CR1_CEN) {
        if ((mock_dma1_ch4.CCR & DMA_CCR_EN) &&
            MOCK_CAP_TRANSFERS(mock_dma1_ch4.CNDTR) > 2) {
            hw_set_capture_source(union_cap_bulk);
        } else {
            hw_set_capture_source(union_cap_present);
        }
        TEST_ASSERT_TRUE(hw_run_until_uif(256));
    }
}

/* Drive ds18b20_poll() until the driver parks at IDLE. After every pumped
 * pass, snapshot the CCR3 feed when it is exactly the 80-slot addressed
 * command write (Match ROM + ROM + command byte, fed from &addr[1]). */
static void union_run_measurement_snapshot(uint16_t* out80, uint8_t* captured) {
    *captured = 0;
    ds18b20_start_measure();
    /* Seed the UIF that onewire_kick() raises on hardware (EGR=UG): the
     * model does not raise UIF on UG, so without this the first poll sees
     * no completion and never schedules the reset pass. Every later pass
     * gets a real UIF from the pumped timer. Same seed as test_parasite. */
    mock_tim1.SR |= TIM_SR_UIF;
    uint16_t guard = 0;
    do {
        ds18b20_poll();
        if (mock_tim1.CR1 & TIM_CR1_CEN) {
            union_run_current_op();
            if (!*captured) {
                const hw_ccr3_feed_log_t* log = hw_ccr3_feed_log();
                if (log->total == ADDR_CMD_SLOTS) {
                    for (uint16_t i = 0; i < ADDR_CMD_SLOTS; i++) {
                        out80[i] = log->values[i];
                    }
                    *captured = 1;
                }
            }
        }
        if (++guard > 2000) {
            break;
        }
    } while (ds18b20_test_get_state() != DS18B20_ST_IDLE);
    TEST_ASSERT_TRUE(guard <= 2000);
    TEST_ASSERT_EQUAL_UINT8(DS18B20_ST_IDLE, ds18b20_test_get_state());
}

static void union_make_rom(uint8_t* rom) {
    uint8_t r[8] = {0x28, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x00};
    r[7] = ds18b20_crc8(r, 7);
    for (int i = 0; i < 8; i++) {
        rom[i] = r[i];
    }
}

void test_rom_addressing_union_rebuilt_across_txn(void) {
    uint8_t rom[8];
    union_make_rom(rom);
    /* Valid-CRC scratchpad so both measurements run clean to IDLE. */
    uint8_t sd[9] = {0x64, 0x01, 0x4B, 0x46, 0x7F, 0xFF, 0x08, 0x10, 0x00};
    sd[8] = ds18b20_crc8(sd, 8);

    hw_reset_all();
    /* hw_reset_all() wipes the DMA-address registration table (addr_count),
     * so re-register the driver buffers: every pumped pass below resolves
     * CMAR through it, and an unregistered buffer fails silently (captures
     * dropped, feed unresolved) rather than loudly. */
    ds18b20_test_register_buffers();
    ds18b20_init();
    ds18b20_test_reset_ctx();
    ds18b20_test_reset_txn();
    ds18b20_test_reset_search();
    ds18b20_test_reset_resolution();
    ds18b20_select(rom);

    /* Addressed measurement #1: snapshot the 80-slot command feed. */
    union_set_bytes_as_pulses(sd, 9);
    static uint16_t first[80];
    uint8_t got_first = 0;
    union_run_measurement_snapshot(first, &got_first);
    TEST_ASSERT_EQUAL_UINT8(1, got_first);

    /* Bare txn in between reuses the union as the txn buffer. Deselect
     * first so it stays bare (an addressed txn would rebuild the prefix
     * itself and weaken the test). */
    ds18b20_select(NULL);
    union_set_bytes_as_pulses(rom, 8);
    {
        uint8_t got[8];
        ds18b20_read_rom(got);
        uint16_t guard = 0;
        for (;;) {
            if (ds18b20_read_rom_poll()) {
                break;
            }
            union_run_current_op();
            if (++guard > 500) {
                break;
            }
        }
        TEST_ASSERT_TRUE(guard <= 500);
        TEST_ASSERT_EQUAL_UINT8(1, ds18b20_test_get_txn_finished());
        TEST_ASSERT_EQUAL_UINT8(1, ds18b20_last_command_ok());
        for (int i = 0; i < 8; i++) {
            TEST_ASSERT_EQUAL_HEX8(rom[i], got[i]);
        }
    }

    /* Addressed measurement #2 must feed the identical prefix: the union
     * held txn garbage, so anything but a rebuild shows up here. */
    ds18b20_select(rom);
    union_set_bytes_as_pulses(sd, 9);
    static uint16_t second[80];
    uint8_t got_second = 0;
    union_run_measurement_snapshot(second, &got_second);
    TEST_ASSERT_EQUAL_UINT8(1, got_second);

    /* Absolute oracle, not just self-consistency: re-encode Match ROM +
     * selected ROM + Convert T with the encoder (an independent path from
     * the table under test) and compare all 80 fed slots. The feed sources
     * from &addr[1], hence the +1 offset; slot 79 is the trailing release.
     * A rebuild from a wrong/stale ROM fails here even if both runs agree. */
    {
        ow_pulse_t expect[81];
        ds18b20_test_encode_byte_pulses(expect, DS18B20_MATCH_ROM);
        for (uint8_t b = 0; b < DS18B20_ROM_BYTES; b++) {
            ds18b20_test_encode_byte_pulses(expect + 8u + (uint16_t)b * 8u, rom[b]);
        }
        ds18b20_test_encode_byte_pulses(expect + 72u, DS18B20_CONVERT_T);
        expect[80] = ONEWIRE_RELEASE_PULSE;
        for (int i = 0; i < 80; i++) {
            TEST_ASSERT_EQUAL_UINT16(expect[i + 1], first[i]);
            TEST_ASSERT_EQUAL_UINT16(expect[i + 1], second[i]);
        }
    }
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
    TEST_RUN(test_select_rejected_while_txn_running);
    TEST_RUN(test_select_rejected_during_search);
    TEST_RUN(test_select_rejected_during_resolution_change);
    TEST_RUN(test_select_rejected_from_scan_callback);
    TEST_RUN(test_rom_addressing_union_rebuilt_across_txn);
}
