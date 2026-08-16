/* ============================================================
 *  test_read_rom.c - Read ROM (0x33) Transaction Tests
 *
 *  Covers the non-blocking ds18b20_read_rom()/poll() pair:
 *   - the bare command (no addressing prefix) pulse build and the
 *     trailing bus-release zero in the CCR1 feed
 *   - a full read-back of the 8-byte ROM over the TIM1/DMA model
 *   - presence-abort leaves the result buffer untouched
 *   - ownership guards (mid-measurement, search, resolution, re-entry)
 *   - ds18b20_last_command_ok() reflects the transaction outcome
 * ============================================================ */

#include "ds18b20.h"
#include "ds18b20_test_access.h"
#include "hw_model.h"
#include "stm32f1xx.h"
#include "unity.h"
#include <string.h>

#define ONE 5u
#define ZERO 60u

/*-------------------------------------------------------------
 *  Shared helpers
 * -----------------------------------------------------------*/

/* Presence-present capture source: valid reset (510) and presence
 * pulse (700) for every reset operation (RCR == 0). */
static uint16_t rom_capture_present(uint32_t idx) {
    return idx == 0 ? 510u : 700u;
}

/* Presence-absent capture source: both edges out of spec. */
static uint16_t rom_capture_absent(uint32_t idx) {
    (void)idx;
    return 100u;
}

/* Read data capture source: pulse durations for a byte stream. */
static uint8_t rom_read_pulses[DS18B20_ROM_BYTES * 8];
static uint16_t rom_read_capture(uint32_t idx) { return rom_read_pulses[idx]; }

static void set_read_bytes(const uint8_t* data, uint8_t len) {
    for (uint8_t i = 0; i < len; i++) {
        for (uint8_t b = 0; b < DS18B20_BITS_PER_BYTE; b++) {
            rom_read_pulses[i * 8 + b] = ((data[i] >> b) & 1u) ? ONE : ZERO;
        }
    }
}

/* Run the currently scheduled hardware operation to completion. Read ops
 * (more than the 2 reset captures) use the byte-stream capture source; every
 * other op keeps the capture source the test configured. */
static void run_current_op(void) {
    if (mock_tim1.CR1 & TIM_CR1_CEN) {
        if ((mock_dma1_ch3.CCR & DMA_CCR_EN) && mock_dma1_ch3.CNDTR > 2) {
            hw_set_capture_source(rom_read_capture);
        }
        TEST_ASSERT_TRUE(hw_run_until_uif(256));
    }
}

/* Drive the transaction poll loop until it reports completion. */
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

/* A plausible 8-byte ROM with a valid trailing CRC. */
static void make_rom(uint8_t* rom) {
    uint8_t r[DS18B20_ROM_BYTES] = {0x28, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x00};
    r[DS18B20_ROM_BYTES - 1] = ds18b20_crc8(r, DS18B20_ROM_BYTES - 1);
    memcpy(rom, r, DS18B20_ROM_BYTES);
}

/*-------------------------------------------------------------
 *  1. Bare command pulse build
 * -----------------------------------------------------------*/

void test_read_rom_bare_command_built(void) {
    uint8_t rom[DS18B20_ROM_BYTES];
    ds18b20_read_rom(rom);
    /* Exactly the Read ROM byte, no addressing prefix. */
    TEST_ASSERT_EQUAL_UINT8(8, ds18b20_test_get_txn_slots());
    for (uint8_t b = 0; b < DS18B20_BITS_PER_BYTE; b++) {
        uint16_t want = ((DS18B20_READ_ROM >> b) & 1u) ? ONE : ZERO;
        TEST_ASSERT_EQUAL_UINT16(want, ds18b20_test_get_txn_pulse(b));
    }
    /* trailing bus-release zero at the bare slot count (8) */
    TEST_ASSERT_EQUAL_UINT8(0, ds18b20_test_get_txn_pulse(8));
}

/*-------------------------------------------------------------
 *  2. Full non-blocking read-back
 * -----------------------------------------------------------*/

void test_read_rom_full_readback(void) {
    uint8_t want[DS18B20_ROM_BYTES];
    uint8_t got[DS18B20_ROM_BYTES];
    make_rom(want);
    set_read_bytes(want, DS18B20_ROM_BYTES);
    hw_set_capture_source(rom_capture_present);

    ds18b20_read_rom(got);
    drive_txn(ds18b20_read_rom_poll);

    TEST_ASSERT_EQUAL_UINT8(1, ds18b20_test_get_txn_finished());
    TEST_ASSERT_EQUAL_UINT8(1, ds18b20_last_command_ok());
    for (int i = 0; i < DS18B20_ROM_BYTES; i++) {
        TEST_ASSERT_EQUAL_HEX8(want[i], got[i]);
    }
}

void test_read_rom_crc_valid(void) {
    uint8_t want[DS18B20_ROM_BYTES];
    uint8_t got[DS18B20_ROM_BYTES];
    make_rom(want);
    set_read_bytes(want, DS18B20_ROM_BYTES);
    hw_set_capture_source(rom_capture_present);

    ds18b20_read_rom(got);
    drive_txn(ds18b20_read_rom_poll);

    /* The driver hands back the raw ROM; the CRC must check out so the
     * application can trust the address. */
    TEST_ASSERT_EQUAL_HEX8(ds18b20_crc8(got, DS18B20_ROM_BYTES - 1),
                           got[DS18B20_ROM_BYTES - 1]);
}

void test_read_rom_feed_release(void) {
    uint8_t want[DS18B20_ROM_BYTES];
    uint8_t got[DS18B20_ROM_BYTES];
    make_rom(want);
    set_read_bytes(want, DS18B20_ROM_BYTES);
    hw_set_capture_source(rom_capture_present);

    ds18b20_read_rom(got);
    /* reset */
    TEST_ASSERT_FALSE(ds18b20_read_rom_poll());
    run_current_op();
    /* write (feed must be the bare 8 slots ending in the release 0) */
    TEST_ASSERT_FALSE(ds18b20_read_rom_poll());
    run_current_op();
    const hw_ccr1_feed_log_t* log = hw_ccr1_feed_log();
    TEST_ASSERT_EQUAL_UINT8(8, log->count);
    TEST_ASSERT_EQUAL_UINT16(0, log->values[7]);
    TEST_ASSERT_TRUE(log->values[0] != 0);
    TEST_ASSERT_EQUAL_UINT16(0, hw_effective_ccr1());
    TEST_ASSERT_FALSE(mock_tim1.CR1 & TIM_CR1_CEN);
    /* read + finish */
    drive_txn(ds18b20_read_rom_poll);
    for (int i = 0; i < DS18B20_ROM_BYTES; i++) {
        TEST_ASSERT_EQUAL_HEX8(want[i], got[i]);
    }
}

/*-------------------------------------------------------------
 *  3. Presence-abort
 * -----------------------------------------------------------*/

void test_read_rom_no_presence_aborts(void) {
    hw_set_capture_source(rom_capture_absent);
    uint8_t got[DS18B20_ROM_BYTES];
    memset(got, 0xAA, sizeof(got));

    ds18b20_read_rom(got);
    drive_txn(ds18b20_read_rom_poll);

    /* Aborted: result buffer untouched, no command was ever sent. */
    TEST_ASSERT_EQUAL_UINT8(1, ds18b20_test_get_txn_finished());
    TEST_ASSERT_EQUAL_UINT8(0, ds18b20_last_command_ok());
    for (int i = 0; i < DS18B20_ROM_BYTES; i++) {
        TEST_ASSERT_EQUAL_HEX8(0xAA, got[i]);
    }
    TEST_ASSERT_EQUAL_UINT8(0, hw_ccr1_feed_log()->count);
}

/*-------------------------------------------------------------
 *  4. Ownership guards
 * -----------------------------------------------------------*/

void test_read_rom_blocked_mid_measurement(void) {
    uint8_t rom[DS18B20_ROM_BYTES];
    ds18b20_test_set_state(DS18B20_ST_CONVERT);
    ds18b20_read_rom(rom);
    TEST_ASSERT_FALSE(mock_tim1.CR1 & TIM_CR1_CEN);
    TEST_ASSERT_EQUAL_UINT8(1, ds18b20_read_rom_poll()); /* idle */
}

void test_read_rom_blocked_during_search(void) {
    uint8_t rom[DS18B20_ROM_BYTES];
    ds18b20_search_start(NULL, 1);
    ds18b20_read_rom(rom);
    TEST_ASSERT_EQUAL_UINT8(1, ds18b20_read_rom_poll()); /* idle */
}

void test_read_rom_blocked_during_resolution(void) {
    uint8_t rom[DS18B20_ROM_BYTES];
    ds18b20_test_reset_txn();
    hw_set_capture_source(rom_capture_present);
    ds18b20_set_resolution(9); /* starts a resolution change */
    TEST_ASSERT_TRUE(mock_tim1.CR1 & TIM_CR1_CEN);
    ds18b20_read_rom(rom);
    TEST_ASSERT_EQUAL_UINT8(1, ds18b20_read_rom_poll()); /* idle */
    drive_txn(ds18b20_set_resolution_poll);
    TEST_ASSERT_EQUAL_UINT8(9, ds18b20_get_resolution());
}

void test_read_rom_reentry_ignored(void) {
    uint8_t rom[DS18B20_ROM_BYTES];
    uint8_t want[DS18B20_ROM_BYTES];
    make_rom(want);
    set_read_bytes(want, DS18B20_ROM_BYTES);
    hw_set_capture_source(rom_capture_present);

    ds18b20_read_rom(rom);
    /* Re-entry while running must be ignored: still the bare Read ROM. */
    uint8_t rom2[DS18B20_ROM_BYTES];
    ds18b20_read_rom(rom2);
    TEST_ASSERT_EQUAL_UINT8(8, ds18b20_test_get_txn_slots());
    drive_txn(ds18b20_read_rom_poll);
    for (int i = 0; i < DS18B20_ROM_BYTES; i++) {
        TEST_ASSERT_EQUAL_HEX8(want[i], rom[i]);
    }
}

/*-------------------------------------------------------------
 *  Run all Read ROM tests
 * -----------------------------------------------------------*/
void run_test_read_rom(void) {
    TEST_RUN(test_read_rom_bare_command_built);
    TEST_RUN(test_read_rom_full_readback);
    TEST_RUN(test_read_rom_crc_valid);
    TEST_RUN(test_read_rom_feed_release);
    TEST_RUN(test_read_rom_no_presence_aborts);
    TEST_RUN(test_read_rom_blocked_mid_measurement);
    TEST_RUN(test_read_rom_blocked_during_search);
    TEST_RUN(test_read_rom_blocked_during_resolution);
    TEST_RUN(test_read_rom_reentry_ignored);
}
