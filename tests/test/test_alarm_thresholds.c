/* ============================================================
 *  test_alarm_thresholds.c - TH/TL Threshold Write and Raw
 *  Scratchpad Read Tests
 *
 *  Covers the non-blocking ds18b20_set_alarm_thresholds()/poll()
 *  and ds18b20_read_scratchpad()/poll() pairs:
 *   - the Write Scratchpad (0x4E) pulse build in Skip and Match
 *     ROM modes, with TH/TL in the user payload and the current
 *     resolution preserved in the CFG byte
 *   - a full non-blocking threshold write over the TIM1/DMA model
 *     with the trailing bus-release zero in the CCR3 feed
 *   - raw 9-byte scratchpad read-back including TH/TL and the CRC
 *   - resolution auto-derivation from a valid config byte only
 *   - presence-abort and re-entry guards
 * ============================================================ */

#include "ds18b20.h"
#include "ds18b20_test_access.h"
#include "hw_model.h"
#include "mock_target.h"
#include "unity.h"
#include <string.h>

#define ONE 5u
#define ZERO 60u

/*-------------------------------------------------------------
 *  Shared helpers
 * -----------------------------------------------------------*/

static uint16_t th_capture_present(uint32_t idx) {
    return idx == 0 ? 510u : 700u;
}

static uint16_t th_capture_absent(uint32_t idx) {
    (void)idx;
    return 100u;
}

static uint8_t th_read_pulses[9 * 8];
static uint16_t th_read_capture(uint32_t idx) { return th_read_pulses[idx]; }

static void set_read_bytes(const uint8_t* data, uint8_t len) {
    for (uint8_t i = 0; i < len; i++) {
        for (uint8_t b = 0; b < DS18B20_BITS_PER_BYTE; b++) {
            th_read_pulses[i * 8 + b] = ((data[i] >> b) & 1u) ? ONE : ZERO;
        }
    }
}

static void run_current_op(void) {
    if (mock_tim1.CR1 & TIM_CR1_CEN) {
        if ((mock_dma1_ch3.CCR & DMA_CCR_EN) && mock_dma1_ch3.CNDTR > 2) {
            hw_set_capture_source(th_read_capture);
        }
        TEST_ASSERT_TRUE(hw_run_until_uif(256));
    }
}

static void drive_txn(uint8_t (*poll)(void)) {
    uint16_t guard = 0;
    for (;;) {
        if (poll()) {
            break;
        }
        run_current_op();
        if (++guard > 500) {
            break;
        }
    }
    TEST_ASSERT_TRUE(guard <= 500);
}

/* Assert that txn_ctx.pulses[0..8*len) encodes the expected bytes. */
static void assert_txn_pulses_bytes(const uint8_t* bytes, uint8_t len) {
    for (uint8_t i = 0; i < len; i++) {
        for (uint8_t b = 0; b < DS18B20_BITS_PER_BYTE; b++) {
            uint16_t want = ((bytes[i] >> b) & 1u) ? ONE : ZERO;
            TEST_ASSERT_EQUAL_UINT16(want, ds18b20_test_get_txn_pulse((uint8_t)(i * 8 + b)));
        }
    }
}

/*-------------------------------------------------------------
 *  1. Write Scratchpad pulse build (Skip / Match ROM)
 * -----------------------------------------------------------*/

void test_thresholds_skip_rom_pulses_built(void) {
    ds18b20_test_set_address_mode(0); /* Skip ROM */
    ds18b20_test_set_resolution(9);
    ds18b20_set_alarm_thresholds(0x4B, 0x46);
    /* 0xCC + 0x4E + TH + TL + CFG(9 bit -> 0x1F) */
    static const uint8_t k_bytes[] = {0xCC, 0x4E, 0x4B, 0x46, 0x1F};
    assert_txn_pulses_bytes(k_bytes, sizeof(k_bytes));
    TEST_ASSERT_EQUAL_UINT8(40, ds18b20_test_get_txn_slots());
    /* trailing bus-release zero at the Skip ROM slot count (40) */
    TEST_ASSERT_EQUAL_UINT8(0, ds18b20_test_get_txn_pulse(40));
}

void test_thresholds_match_rom_pulses_built(void) {
    uint8_t rom[DS18B20_ROM_BYTES] = {0x28, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07};
    ds18b20_select(rom); /* -> Match ROM addressing */
    ds18b20_test_set_resolution(12);
    ds18b20_set_alarm_thresholds(0x4B, 0x46);
    /* 0x55 + ROM + 0x4E + TH + TL + CFG(0x7F) */
    uint8_t k_bytes[1 + DS18B20_ROM_BYTES + 1 + 3];
    k_bytes[0] = 0x55;
    memcpy(&k_bytes[1], rom, DS18B20_ROM_BYTES);
    k_bytes[9] = 0x4E;
    k_bytes[10] = 0x4B;
    k_bytes[11] = 0x46;
    k_bytes[12] = 0x7F;
    assert_txn_pulses_bytes(k_bytes, sizeof(k_bytes));
    TEST_ASSERT_EQUAL_UINT8(104, ds18b20_test_get_txn_slots());
    /* trailing bus-release zero at the Match ROM slot count (104) */
    TEST_ASSERT_EQUAL_UINT8(0, ds18b20_test_get_txn_pulse(104));
}

void test_thresholds_cfg_preserves_current_resolution(void) {
    /* A threshold write must never disturb the conversion resolution. */
    ds18b20_test_set_resolution(11);
    ds18b20_set_alarm_thresholds(0x00, 0x00);
    static const uint8_t k_bytes[] = {0xCC, 0x4E, 0x00, 0x00, 0x5F}; /* 11 bit */
    assert_txn_pulses_bytes(k_bytes, sizeof(k_bytes));
}

/*-------------------------------------------------------------
 *  2. Full non-blocking threshold write
 * -----------------------------------------------------------*/

void test_thresholds_skip_rom_write_completes(void) {
    hw_set_capture_source(th_capture_present);
    ds18b20_test_set_address_mode(0);
    ds18b20_set_alarm_thresholds(0x4B, 0x46);
    drive_txn(ds18b20_set_alarm_thresholds_poll);
    TEST_ASSERT_EQUAL_UINT8(1, ds18b20_last_command_ok());
    TEST_ASSERT_EQUAL_UINT8(1, ds18b20_test_get_txn_finished());
}

void test_thresholds_skip_rom_feed_release(void) {
    hw_set_capture_source(th_capture_present);
    ds18b20_test_set_address_mode(0);
    ds18b20_set_alarm_thresholds(0x4B, 0x46);
    /* reset */
    TEST_ASSERT_FALSE(ds18b20_set_alarm_thresholds_poll());
    run_current_op();
    /* write: exactly 40 slot pulses, ending with the trailing 0 */
    TEST_ASSERT_FALSE(ds18b20_set_alarm_thresholds_poll());
    run_current_op();
    const hw_ccr1_feed_log_t* log = hw_ccr1_feed_log();
    TEST_ASSERT_EQUAL_UINT8(40, log->count);
    TEST_ASSERT_EQUAL_UINT16(0, log->values[39]);
    TEST_ASSERT_TRUE(log->values[0] != 0);
    TEST_ASSERT_EQUAL_UINT16(0, hw_effective_ccr1());
    TEST_ASSERT_FALSE(mock_tim1.CR1 & TIM_CR1_CEN);
    /* finish (the poll after the write consumed its UIF and reached DONE) */
    drive_txn(ds18b20_set_alarm_thresholds_poll);
}

void test_thresholds_match_rom_feed_release(void) {
    uint8_t rom[DS18B20_ROM_BYTES] = {0x28, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07};
    ds18b20_select(rom);
    hw_set_capture_source(th_capture_present);
    ds18b20_set_alarm_thresholds(0x4B, 0x46);
    TEST_ASSERT_FALSE(ds18b20_set_alarm_thresholds_poll());
    run_current_op();
    TEST_ASSERT_FALSE(ds18b20_set_alarm_thresholds_poll());
    run_current_op();
    const hw_ccr1_feed_log_t* log = hw_ccr1_feed_log();
    TEST_ASSERT_EQUAL_UINT8(104, log->count);
    TEST_ASSERT_EQUAL_UINT16(0, log->values[103]);
    TEST_ASSERT_EQUAL_UINT16(0, hw_effective_ccr1());
    TEST_ASSERT_FALSE(mock_tim1.CR1 & TIM_CR1_CEN);
    drive_txn(ds18b20_set_alarm_thresholds_poll);
}

/*-------------------------------------------------------------
 *  3. Raw 9-byte scratchpad read-back
 * -----------------------------------------------------------*/

/* Build a scratchpad with TH/TL set and a valid CRC. */
static void make_scratchpad(uint8_t* sd, uint8_t th, uint8_t tl, uint8_t cfg) {
    sd[0] = 0x64; /* temp LSB */
    sd[1] = 0x01; /* temp MSB */
    sd[2] = th;
    sd[3] = tl;
    sd[4] = cfg;
    sd[5] = 0xFF;
    sd[6] = 0x08;
    sd[7] = 0x10;
    sd[8] = ds18b20_crc8(sd, 8);
}

void test_scratchpad_raw_read_returns_all_nine_bytes(void) {
    uint8_t want[9];
    uint8_t got[9];
    make_scratchpad(want, 0x4B, 0x46, 0x7F);
    set_read_bytes(want, 9);
    hw_set_capture_source(th_capture_present);

    ds18b20_read_scratchpad(got);
    drive_txn(ds18b20_read_scratchpad_poll);

    TEST_ASSERT_EQUAL_UINT8(1, ds18b20_last_command_ok());
    for (int i = 0; i < 9; i++) {
        TEST_ASSERT_EQUAL_HEX8(want[i], got[i]);
    }
    TEST_ASSERT_EQUAL_HEX8(want[8], ds18b20_crc8(got, 8));
}

void test_scratchpad_raw_read_derives_resolution(void) {
    uint8_t want[9];
    uint8_t got[9];
    make_scratchpad(want, 0x4B, 0x46, 0x3F); /* 10 bit */
    set_read_bytes(want, 9);
    ds18b20_test_set_resolution(12); /* not yet derived */
    hw_set_capture_source(th_capture_present);

    ds18b20_read_scratchpad(got);
    drive_txn(ds18b20_read_scratchpad_poll);

    /* A valid CRC lets the driver trust the config byte. */
    TEST_ASSERT_EQUAL_UINT8(10, ds18b20_get_resolution());
}

void test_scratchpad_raw_read_bad_crc_keeps_resolution(void) {
    uint8_t want[9];
    uint8_t got[9];
    make_scratchpad(want, 0x4B, 0x46, 0x3F);
    want[8] ^= 0xFF; /* corrupt the CRC */
    set_read_bytes(want, 9);
    ds18b20_test_set_resolution(12);
    hw_set_capture_source(th_capture_present);

    ds18b20_read_scratchpad(got);
    drive_txn(ds18b20_read_scratchpad_poll);

    /* The driver still hands back the raw bytes, but a corrupted config byte
     * must never change the conversion wait. */
    TEST_ASSERT_EQUAL_UINT8(12, ds18b20_get_resolution());
    for (int i = 0; i < 9; i++) {
        TEST_ASSERT_EQUAL_HEX8(want[i], got[i]);
    }
}

/*-------------------------------------------------------------
 *  4. Presence-abort and re-entry
 * -----------------------------------------------------------*/

void test_thresholds_no_presence_aborts(void) {
    hw_set_capture_source(th_capture_absent);
    ds18b20_set_alarm_thresholds(0x4B, 0x46);
    drive_txn(ds18b20_set_alarm_thresholds_poll);
    TEST_ASSERT_EQUAL_UINT8(1, ds18b20_test_get_txn_finished());
    TEST_ASSERT_EQUAL_UINT8(0, ds18b20_last_command_ok());
    TEST_ASSERT_EQUAL_UINT8(0, hw_ccr1_feed_log()->count);
}

void test_thresholds_reentry_ignored(void) {
    hw_set_capture_source(th_capture_present);
    ds18b20_set_alarm_thresholds(0x4B, 0x46);
    /* Re-entry while running must be ignored: still the 0x4B/0x46 write. */
    ds18b20_set_alarm_thresholds(0x00, 0x00);
    static const uint8_t k_bytes[] = {0xCC, 0x4E, 0x4B, 0x46, 0x7F};
    assert_txn_pulses_bytes(k_bytes, sizeof(k_bytes));
    drive_txn(ds18b20_set_alarm_thresholds_poll);
    TEST_ASSERT_EQUAL_UINT8(1, ds18b20_last_command_ok());
}

void test_scratchpad_raw_no_presence_aborts(void) {
    hw_set_capture_source(th_capture_absent);
    uint8_t got[9];
    memset(got, 0xAA, sizeof(got));
    ds18b20_read_scratchpad(got);
    drive_txn(ds18b20_read_scratchpad_poll);
    TEST_ASSERT_EQUAL_UINT8(0, ds18b20_last_command_ok());
    for (int i = 0; i < 9; i++) {
        TEST_ASSERT_EQUAL_HEX8(0xAA, got[i]);
    }
}

/*-------------------------------------------------------------
 *  Run all threshold / scratchpad tests
 * -----------------------------------------------------------*/
void run_test_alarm_thresholds(void) {
    TEST_RUN(test_thresholds_skip_rom_pulses_built);
    TEST_RUN(test_thresholds_match_rom_pulses_built);
    TEST_RUN(test_thresholds_cfg_preserves_current_resolution);
    TEST_RUN(test_thresholds_skip_rom_write_completes);
    TEST_RUN(test_thresholds_skip_rom_feed_release);
    TEST_RUN(test_thresholds_match_rom_feed_release);
    TEST_RUN(test_scratchpad_raw_read_returns_all_nine_bytes);
    TEST_RUN(test_scratchpad_raw_read_derives_resolution);
    TEST_RUN(test_scratchpad_raw_read_bad_crc_keeps_resolution);
    TEST_RUN(test_thresholds_no_presence_aborts);
    TEST_RUN(test_thresholds_reentry_ignored);
    TEST_RUN(test_scratchpad_raw_no_presence_aborts);
}
