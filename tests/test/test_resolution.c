/* ============================================================
 *  test_resolution.c - Non-Blocking Resolution Change Tests
 *
 *  Covers the resolution-aware conversion wait and the
 *  ds18b20_set_resolution()/poll()/get_resolution() state
 *  machine against the TIM1/DMA hardware model:
 *   - wait timing table for 9/10/11/12 bit (exact minimum waits)
 *   - the config write sequence (Skip ROM and Match ROM) with the
 *     trailing bus-release zero in the CCR1 feed
 *   - ownership guards (mid-measurement, search, re-entry, range)
 *   - presence-abort without adopting the new resolution
 *   - auto-derivation of the resolution from a valid scratchpad
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
 * pulse (700) for every reset operation (RCR == 0). The config
 * write never captures, so nothing else is needed. */
static uint16_t res_capture_present(uint32_t idx) {
    return idx == 0 ? 510u : 700u;
}

/* Presence-absent capture source: both edges out of spec. */
static uint16_t res_capture_absent(uint32_t idx) {
    (void)idx;
    return 100u;
}

/* Poll the resolution state machine until it reports completion,
 * running the simulated timer between polls. */
static void drive_poll_until_done(void) {
    uint16_t guard = 0;
    for (;;) {
        if (ds18b20_set_resolution_poll()) {
            break;
        }
        if (mock_tim1.CR1 & TIM_CR1_CEN) {
            TEST_ASSERT_TRUE(hw_run_until_uif(120));
        }
        if (++guard > 500) {
            break;
        }
    }
    TEST_ASSERT_TRUE(guard <= 500);
}

static void drive_res_change(uint8_t bits) {
    ds18b20_set_resolution(bits);
    drive_poll_until_done();
}

/* Assert that res_ctx.pulses[0..8*len) encodes the expected bytes. */
static void assert_res_pulses_bytes(const uint8_t* bytes, uint8_t len) {
    for (uint8_t i = 0; i < len; i++) {
        for (uint8_t b = 0; b < DS18B20_BITS_PER_BYTE; b++) {
            uint16_t want = ((bytes[i] >> b) & 1u) ? ONE : ZERO;
            TEST_ASSERT_EQUAL_UINT16(want, ds18b20_test_get_res_pulse((uint8_t)(i * 8 + b)));
        }
    }
}

/*-------------------------------------------------------------
 *  1. Default resolution and wait timing table
 * -----------------------------------------------------------*/

void test_resolution_default_12_after_init(void) {
    ds18b20_init();
    TEST_ASSERT_EQUAL_UINT8(12, ds18b20_get_resolution());
    TEST_ASSERT_EQUAL_UINT8(1, ds18b20_set_resolution_poll()); /* idle */
}

void test_resolution_wait_timing_matches_table(void) {
    static const struct {
        uint8_t res;
        uint16_t arr;
        uint8_t rcr;
        uint32_t us;
    } k_wait[] = {
        {9, 9375, 9, 93750}, /* 10 x 9.375ms = 93.75ms */
        {10, 18750, 9, 187500}, /* 10 x 18.75ms = 187.5ms */
        {11, 18750, 19, 375000}, /* 20 x 18.75ms = 375ms */
        {12, 62500, 11, 750000}, /* 12 x 62.5ms = 750ms */
    };
    for (unsigned i = 0; i < sizeof(k_wait) / sizeof(k_wait[0]); i++) {
        hw_reset_all();
        ds18b20_test_set_resolution(k_wait[i].res);
        test_bus_wait_conversion();
        TEST_ASSERT_EQUAL_UINT32(k_wait[i].arr, mock_tim1.ARR);
        TEST_ASSERT_EQUAL_UINT32(k_wait[i].rcr, mock_tim1.RCR);
        /* invariant: (RCR+1) x ARR must equal the datasheet wait in µs */
        TEST_ASSERT_EQUAL_UINT32(k_wait[i].us,
                                 (uint32_t)(mock_tim1.RCR + 1) * mock_tim1.ARR);
    }
}

/*-------------------------------------------------------------
 *  2. Config write pre-build: Skip ROM and Match ROM
 * -----------------------------------------------------------*/

void test_resolution_skip_rom_pulses_built(void) {
    ds18b20_test_set_address_mode(0); /* Skip ROM */
    ds18b20_set_resolution(9);
    /* 0xCC (Skip ROM) + 0x4E (Write Scratchpad) + TH + TL + CFG(0x1F) */
    static const uint8_t k_bytes[] = {0xCC, 0x4E, 0x00, 0x00, 0x1F};
    assert_res_pulses_bytes(k_bytes, sizeof(k_bytes));
    /* trailing bus-release zero at the Skip ROM slot count (40) */
    TEST_ASSERT_EQUAL_UINT8(0, ds18b20_test_get_res_pulse(40));
}

void test_resolution_match_rom_pulses_built(void) {
    uint8_t rom[DS18B20_ROM_BYTES] = {0x28, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07};
    ds18b20_select(rom); /* -> Match ROM addressing */
    ds18b20_set_resolution(12);
    /* 0x55 + ROM + 0x4E + TH + TL + CFG(0x7F) */
    uint8_t k_bytes[1 + DS18B20_ROM_BYTES + 1 + 3];
    k_bytes[0] = 0x55;
    memcpy(&k_bytes[1], rom, DS18B20_ROM_BYTES);
    k_bytes[9] = 0x4E;
    k_bytes[10] = 0x00;
    k_bytes[11] = 0x00;
    k_bytes[12] = 0x7F;
    assert_res_pulses_bytes(k_bytes, sizeof(k_bytes));
    /* trailing bus-release zero at the Match ROM slot count (104) */
    TEST_ASSERT_EQUAL_UINT8(0, ds18b20_test_get_res_pulse(104));
}

/*-------------------------------------------------------------
 *  3. Full non-blocking change over the hardware model
 * -----------------------------------------------------------*/

void test_resolution_change_all_four_values(void) {
    hw_set_capture_source(res_capture_present);
    static const uint8_t k_res[] = {9, 10, 11, 12};
    for (unsigned i = 0; i < sizeof(k_res) / sizeof(k_res[0]); i++) {
        drive_res_change(k_res[i]);
        TEST_ASSERT_EQUAL_UINT8(k_res[i], ds18b20_get_resolution());
        TEST_ASSERT_EQUAL_UINT8(1, ds18b20_set_resolution_poll()); /* finished */
    }
}

void test_resolution_skip_rom_feed_release(void) {
    hw_set_capture_source(res_capture_present);
    ds18b20_test_set_address_mode(0);
    drive_res_change(9);
    TEST_ASSERT_EQUAL_UINT8(9, ds18b20_get_resolution());

    /* The last simulated operation is the config write: exactly 40 slot
     * pulses are DMA-fed and the feed must end with the trailing 0 that
     * releases the bus HIGH in hardware. */
    const hw_ccr1_feed_log_t* log = hw_ccr1_feed_log();
    TEST_ASSERT_EQUAL_UINT8(40, log->count);
    TEST_ASSERT_EQUAL_UINT16(0, log->values[39]);
    TEST_ASSERT_TRUE(log->values[0] != 0);
    TEST_ASSERT_EQUAL_UINT16(0, hw_effective_ccr1());
    TEST_ASSERT_FALSE(mock_tim1.CR1 & TIM_CR1_CEN);
}

void test_resolution_match_rom_feed_release(void) {
    uint8_t rom[DS18B20_ROM_BYTES] = {0x28, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07};
    ds18b20_select(rom); /* -> Match ROM addressing */
    hw_set_capture_source(res_capture_present);
    drive_res_change(10);
    TEST_ASSERT_EQUAL_UINT8(10, ds18b20_get_resolution());

    const hw_ccr1_feed_log_t* log = hw_ccr1_feed_log();
    TEST_ASSERT_EQUAL_UINT8(104, log->count); /* 13 bytes x 8 slots */
    TEST_ASSERT_EQUAL_UINT16(0, log->values[103]);
    TEST_ASSERT_EQUAL_UINT16(0, hw_effective_ccr1());
    TEST_ASSERT_FALSE(mock_tim1.CR1 & TIM_CR1_CEN);
}

/*-------------------------------------------------------------
 *  4. Guards: range, state, search ownership, re-entry
 * -----------------------------------------------------------*/

void test_resolution_invalid_values_ignored(void) {
    static const uint8_t k_bad[] = {0, 8, 13, 255};
    for (unsigned i = 0; i < sizeof(k_bad) / sizeof(k_bad[0]); i++) {
        ds18b20_set_resolution(k_bad[i]);
        TEST_ASSERT_EQUAL_UINT8(12, ds18b20_get_resolution()); /* unchanged */
        TEST_ASSERT_EQUAL_UINT8(1, ds18b20_set_resolution_poll()); /* idle */
        TEST_ASSERT_FALSE(mock_tim1.CR1 & TIM_CR1_CEN); /* nothing scheduled */
    }
}

void test_resolution_blocked_mid_measurement(void) {
    /* Non-IDLE measurement state: the timer belongs to the measurement. */
    ds18b20_test_set_state(DS18B20_ST_CONVERT);
    ds18b20_set_resolution(9);
    /* Nothing scheduled and nothing adopted. */
    TEST_ASSERT_FALSE(mock_tim1.CR1 & TIM_CR1_CEN);
    TEST_ASSERT_EQUAL_UINT8(12, ds18b20_get_resolution());
    TEST_ASSERT_EQUAL_UINT8(1, ds18b20_set_resolution_poll());
}

void test_resolution_blocked_during_search(void) {
    /* A running device search owns the timer. */
    ds18b20_search_start(NULL, 1);
    ds18b20_set_resolution(9);
    /* Ignored: the resolution context stays idle. */
    TEST_ASSERT_EQUAL_UINT8(12, ds18b20_get_resolution());
    TEST_ASSERT_EQUAL_UINT8(1, ds18b20_set_resolution_poll());
}

void test_resolution_reentry_ignored(void) {
    hw_set_capture_source(res_capture_present);
    ds18b20_set_resolution(10);
    /* Re-entry while running must be ignored: still pending 10 bit. */
    ds18b20_set_resolution(12);
    drive_poll_until_done();
    TEST_ASSERT_EQUAL_UINT8(10, ds18b20_get_resolution());
    /* The pre-built config byte is still the 10-bit one (0x3F). */
    static const uint8_t k_bytes[] = {0xCC, 0x4E, 0x00, 0x00, 0x3F};
    assert_res_pulses_bytes(k_bytes, sizeof(k_bytes));
}

/*-------------------------------------------------------------
 *  5. Presence-abort keeps the old resolution
 * -----------------------------------------------------------*/

void test_resolution_no_presence_aborts(void) {
    hw_set_capture_source(res_capture_absent);
    drive_res_change(9);
    /* No config write happened: the resolution must NOT be adopted. */
    TEST_ASSERT_EQUAL_UINT8(12, ds18b20_get_resolution());
    /* The last simulated operation was only the presence reset: no CCR1 feed
     * (i.e. no config write) was ever sent to the bus. */
    TEST_ASSERT_EQUAL_UINT8(0, hw_ccr1_feed_log()->count);
    TEST_ASSERT_EQUAL_UINT8(1, ds18b20_set_resolution_poll()); /* finished */
}

/*-------------------------------------------------------------
 *  6. Measurement stays out of the way while the change runs
 * -----------------------------------------------------------*/

void test_resolution_poll_blocked_while_running(void) {
    ds18b20_test_set_state(DS18B20_ST_IDLE);
    hw_set_capture_source(res_capture_present);
    ds18b20_set_resolution(9);

    /* Complete the reset so the change is mid-flight and UIF is set. */
    TEST_ASSERT_TRUE(hw_run_until_uif(100));

    /* Mid-change: poll() must not consume the UIF nor advance the state. */
    ds18b20_poll();
    TEST_ASSERT_EQUAL_UINT8(DS18B20_ST_IDLE, ds18b20_test_get_state());
    TEST_ASSERT_TRUE(mock_tim1.SR & TIM_SR_UIF);

    /* The change itself still consumes the same UIF and completes. */
    drive_poll_until_done();
    TEST_ASSERT_EQUAL_UINT8(9, ds18b20_get_resolution());
    TEST_ASSERT_EQUAL_UINT8(DS18B20_ST_IDLE, ds18b20_test_get_state());
}

/*-------------------------------------------------------------
 *  7. The next measurement waits for the new resolution
 * -----------------------------------------------------------*/

void test_resolution_next_cycle_waits_short(void) {
    hw_set_capture_source(res_capture_present);
    drive_res_change(9);
    TEST_ASSERT_EQUAL_UINT8(9, ds18b20_get_resolution());

    /* UIF is set by the EGR handover: run one measurement cycle up to WAIT. */
    mock_tim1.SR |= TIM_SR_UIF;
    ds18b20_poll();
    TEST_ASSERT_EQUAL_UINT8(DS18B20_ST_CONVERT, ds18b20_test_get_state());

    ds18b20_test_set_edge(0, 510);
    ds18b20_test_set_edge(1, 700);
    mock_tim1.SR |= TIM_SR_UIF;
    ds18b20_poll();
    TEST_ASSERT_EQUAL_UINT8(DS18B20_ST_WAIT, ds18b20_test_get_state());

    /* wait_conversion() must now wait for 9-bit (93.75ms), not 750ms. */
    mock_tim1.SR |= TIM_SR_UIF;
    ds18b20_poll();
    TEST_ASSERT_EQUAL_UINT32(9375, mock_tim1.ARR);
    TEST_ASSERT_EQUAL_UINT32(9, mock_tim1.RCR);
    TEST_ASSERT_EQUAL_UINT8(DS18B20_ST_CONTINUE, ds18b20_test_get_state());
}

/*-------------------------------------------------------------
 *  8. Auto-derivation from a valid scratchpad read
 * -----------------------------------------------------------*/

/* Load a scratchpad image into the pulse array exactly as read_data() +
 * decode_scratchpad() would reconstruct it (bit b of byte i -> pulse[i*8+b]). */
static void set_scratchpad_via_pulses(const uint8_t* sd) {
    for (uint8_t i = 0; i < 9; i++) {
        for (uint8_t b = 0; b < DS18B20_BITS_PER_BYTE; b++) {
            ds18b20_test_set_pulse(i * 8 + b, ((sd[i] >> b) & 1u) ? ONE : ZERO);
        }
    }
}

void test_resolution_decode_derives_from_scratchpad(void) {
    uint8_t sd[9] = {0x64, 0x01, 0x4B, 0x46, 0x3F, 0xFF, 0x08, 0x10, 0};
    sd[8] = ds18b20_crc8(sd, 8);
    set_scratchpad_via_pulses(sd);
    ds18b20_test_set_state(DS18B20_ST_DECODE);
    mock_tim1.SR |= TIM_SR_UIF;
    ds18b20_poll();
    /* config byte 0x3F = 10 bit (R1/R0 = 01) */
    TEST_ASSERT_EQUAL_UINT8(10, ds18b20_get_resolution());
    TEST_ASSERT_EQUAL_UINT8(DS18B20_ST_IDLE, ds18b20_test_get_state());
}

void test_resolution_decode_bad_crc_keeps_resolution(void) {
    uint8_t sd[9] = {0x64, 0x01, 0x4B, 0x46, 0x3F, 0xFF, 0x08, 0x10, 0};
    sd[8] = (uint8_t)(ds18b20_crc8(sd, 8) ^ 0xFF);
    set_scratchpad_via_pulses(sd);
    ds18b20_test_set_state(DS18B20_ST_DECODE);
    mock_tim1.SR |= TIM_SR_UIF;
    ds18b20_poll();
    /* CRC failed: a corrupted config byte must never change the wait. */
    TEST_ASSERT_EQUAL_UINT8(12, ds18b20_get_resolution());
    TEST_ASSERT_EQUAL_UINT8(DS18B20_ST_IDLE, ds18b20_test_get_state());
}

/*-------------------------------------------------------------
 *  Run all resolution tests
 * -----------------------------------------------------------*/
void run_test_resolution(void) {
    TEST_RUN(test_resolution_default_12_after_init);
    TEST_RUN(test_resolution_wait_timing_matches_table);
    TEST_RUN(test_resolution_skip_rom_pulses_built);
    TEST_RUN(test_resolution_match_rom_pulses_built);
    TEST_RUN(test_resolution_change_all_four_values);
    TEST_RUN(test_resolution_skip_rom_feed_release);
    TEST_RUN(test_resolution_match_rom_feed_release);
    TEST_RUN(test_resolution_invalid_values_ignored);
    TEST_RUN(test_resolution_blocked_mid_measurement);
    TEST_RUN(test_resolution_blocked_during_search);
    TEST_RUN(test_resolution_reentry_ignored);
    TEST_RUN(test_resolution_no_presence_aborts);
    TEST_RUN(test_resolution_poll_blocked_while_running);
    TEST_RUN(test_resolution_next_cycle_waits_short);
    TEST_RUN(test_resolution_decode_derives_from_scratchpad);
    TEST_RUN(test_resolution_decode_bad_crc_keeps_resolution);
}
