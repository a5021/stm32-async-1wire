/* ============================================================
 *  test_broadcast.c - Simultaneous Multi-Device Conversion Tests
 *
 *  Covers scan mode (ds18b20_scan_start()): one broadcast Convert T
 *  (Skip ROM) converts every sensor in parallel, then each device is
 *  read back via Match ROM in device-table order. Per device the
 *  ds18b20_complete() callback fires with ds18b20_scan_index() /
 *  ds18b20_device_rom() identifying the sensor:
 *   - scan_start() ownership guards and device-table requirement
 *   - broadcast CONVERT forces Skip ROM even with a device selected
 *   - per-device REQUEST addresses the current device via Match ROM
 *   - a full round converts once and reads N times, ending at IDLE
 *   - a missing device reports NO_SENSOR and the scan continues
 *   - ds18b20_select() (single-device) clears scan mode
 * ============================================================ */

#include "ds18b20.h"
#include "ds18b20_test_access.h"
#include "ds18b20_test_spy.h"
#include "hw_model.h"
#include "mock_target.h"
#include "unity.h"

#define ONE 5u
#define ZERO 60u

/* Three fake device ROMs (LSB first; the driver does not re-validate them). */
static const uint8_t k_roms[3][DS18B20_ROM_BYTES] = {
    {0x28, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07},
    {0x28, 0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17},
    {0x28, 0x21, 0x22, 0x23, 0x24, 0x25, 0x26, 0x27},
};

/*-------------------------------------------------------------
 *  Shared helpers
 * -----------------------------------------------------------*/

static void set_presence_ok(void) {
    ds18b20_test_set_edge(0, 510); /* valid reset pulse */
    ds18b20_test_set_edge(1, 700); /* valid presence pulse */
}

/* Feed one valid scratchpad image into the pulse array exactly as
 * read_data() + decode_scratchpad() would reconstruct it. */
static void set_scratchpad_ok(void) {
    uint8_t sd[9] = {0x64, 0x01, 0x4B, 0x46, 0x7F, 0xFF, 0x08, 0x10, 0};
    sd[8] = ds18b20_crc8(sd, 8);
    for (int i = 0; i < 9; i++) {
        for (int b = 0; b < 8; b++) {
            ds18b20_test_set_pulse(i * 8 + b, ((sd[i] >> b) & 1u) ? ONE : ZERO);
        }
    }
}

/* All-0xFF scratchpad: a Match ROM addressed an absent device, so nobody
 * drives the bus after the address and the readback is all '1' bits. */
static void set_scratchpad_absent(void) {
    for (int i = 0; i < 9; i++) {
        for (int b = 0; b < 8; b++) {
            ds18b20_test_set_pulse(i * 8 + b, ONE);
        }
    }
}

/* Fresh driver + device table of n devices + scan mode armed. */
static void setup_scan(uint8_t n) {
    test_spy_reset();
    ds18b20_init();
    ds18b20_test_reset_ctx();
    ds18b20_test_reset_search();
    ds18b20_test_reset_resolution();
    for (uint8_t i = 0; i < n; i++) {
        ds18b20_test_set_device(i, k_roms[i]);
    }
    ds18b20_test_set_device_count(n);
    ds18b20_scan_start();
    TEST_ASSERT_EQUAL_UINT8(1, ds18b20_test_get_scan_mode());
    TEST_ASSERT_EQUAL_UINT8(0, ds18b20_test_get_scan_index());
}

/* Drive one device read-back in scan mode: CONTINUE -> REQUEST -> READ ->
 * DECODE. Expects a valid scratchpad and records the completion. After DECODE
 * the state is CONTINUE when more devices remain, IDLE for the last one.
 * (The REQUEST state addresses the device and issues the read in a single
 * poll, so its Match ROM addressing is only observable after that poll.) */
static void read_one_device(uint8_t expected_index, uint8_t expect_more,
                            int16_t expected_temp) {
    uint8_t before = test_spy_complete_count;

    /* CONTINUE -> REQUEST: state entry, addressing not applied yet. */
    set_presence_ok();
    mock_tim1.SR |= TIM_SR_UIF;
    ds18b20_poll();
    TEST_ASSERT_EQUAL_UINT8(DS18B20_ST_REQUEST, ds18b20_test_get_state());
    TEST_ASSERT_EQUAL_UINT8(expected_index, ds18b20_test_get_scan_index());

    /* REQUEST -> READ: Match ROM address for the current device. */
    set_presence_ok();
    mock_tim1.SR |= TIM_SR_UIF;
    ds18b20_poll();
    TEST_ASSERT_EQUAL_UINT8(DS18B20_ST_READ, ds18b20_test_get_state());
    TEST_ASSERT_EQUAL_UINT8(1, ds18b20_test_get_address_mode());
    uint8_t sel[DS18B20_ROM_BYTES];
    ds18b20_test_get_selected_rom(sel);
    for (int i = 0; i < DS18B20_ROM_BYTES; i++) {
        TEST_ASSERT_EQUAL_UINT8(k_roms[expected_index][i], sel[i]);
    }

    /* READ -> DECODE */
    mock_tim1.SR |= TIM_SR_UIF;
    ds18b20_poll();
    TEST_ASSERT_EQUAL_UINT8(DS18B20_ST_DECODE, ds18b20_test_get_state());

    /* DECODE -> next CONTINUE or IDLE, reporting this device. */
    set_scratchpad_ok();
    mock_tim1.SR |= TIM_SR_UIF;
    ds18b20_poll();
    TEST_ASSERT_TRUE(test_spy_complete_count > before);
    TEST_ASSERT_EQUAL_INT(expected_temp, test_spy_complete_values[before]);
    TEST_ASSERT_EQUAL_UINT8(expected_index, test_spy_complete_indices[before]);
    if (expect_more) {
        TEST_ASSERT_EQUAL_UINT8(DS18B20_ST_CONTINUE, ds18b20_test_get_state());
        /* The scheduling bridge timer must be armed: DECODE armed nothing, and
         * without it no UIF would ever drive CONTINUE (the original stall). */
        TEST_ASSERT_TRUE(mock_tim1.CR1 & TIM_CR1_CEN);
    } else {
        TEST_ASSERT_EQUAL_UINT8(DS18B20_ST_IDLE, ds18b20_test_get_state());
        /* Inter-measurement pause armed after the last device. */
        TEST_ASSERT_TRUE(mock_tim1.CR1 & TIM_CR1_CEN);
    }
}

/*-------------------------------------------------------------
 *  1. scan_start ownership guards and device-table requirement
 * -----------------------------------------------------------*/

void test_broadcast_start_guards(void) {
    ds18b20_init();
    ds18b20_test_reset_ctx();
    ds18b20_test_reset_search();
    ds18b20_test_reset_resolution();

    /* Mid-measurement: rejected. */
    ds18b20_test_set_state(DS18B20_ST_CONVERT);
    ds18b20_scan_start();
    TEST_ASSERT_EQUAL_UINT8(0, ds18b20_test_get_scan_mode());
    ds18b20_test_set_state(DS18B20_ST_IDLE);

    /* A running device search owns the timer: rejected. */
    ds18b20_search_start(NULL, 1);
    ds18b20_scan_start();
    TEST_ASSERT_EQUAL_UINT8(0, ds18b20_test_get_scan_mode());
    ds18b20_test_reset_search();

    /* A running resolution change owns the timer: rejected. */
    ds18b20_set_resolution(9);
    ds18b20_scan_start();
    TEST_ASSERT_EQUAL_UINT8(0, ds18b20_test_get_scan_mode());
    ds18b20_test_reset_resolution();

    /* Idle but nothing discovered: rejected (no device to convert). */
    ds18b20_scan_start();
    TEST_ASSERT_EQUAL_UINT8(0, ds18b20_test_get_scan_mode());

    /* One device discovered: accepted. */
    ds18b20_test_set_device(0, k_roms[0]);
    ds18b20_test_set_device_count(1);
    ds18b20_scan_start();
    TEST_ASSERT_EQUAL_UINT8(1, ds18b20_test_get_scan_mode());
    TEST_ASSERT_EQUAL_UINT8(0, ds18b20_test_get_scan_index());
}

/*-------------------------------------------------------------
 *  2. Broadcast CONVERT forces Skip ROM
 * -----------------------------------------------------------*/

void test_broadcast_convert_forces_skip_rom(void) {
    setup_scan(2);
    /* Select a device first (single-device Match ROM addressing) to prove the
     * scan broadcast overrides it for the conversion. */
    ds18b20_select(k_roms[0]);
    TEST_ASSERT_EQUAL_UINT8(0, ds18b20_test_get_scan_mode()); /* cleared */
    ds18b20_scan_start();
    TEST_ASSERT_EQUAL_UINT8(1, ds18b20_test_get_scan_mode());
    TEST_ASSERT_EQUAL_UINT8(1, ds18b20_test_get_address_mode()); /* still selected */

    /* IDLE -> CONVERT */
    mock_tim1.SR |= TIM_SR_UIF;
    ds18b20_poll();
    TEST_ASSERT_EQUAL_UINT8(DS18B20_ST_CONVERT, ds18b20_test_get_state());

    /* CONVERT -> WAIT: the broadcast must force Skip ROM. */
    set_presence_ok();
    mock_tim1.SR |= TIM_SR_UIF;
    ds18b20_poll();
    TEST_ASSERT_EQUAL_UINT8(DS18B20_ST_WAIT, ds18b20_test_get_state());
    TEST_ASSERT_EQUAL_UINT8(0, ds18b20_test_get_address_mode());
    TEST_ASSERT_EQUAL_UINT8(0, ds18b20_test_get_scan_index());
}

/*-------------------------------------------------------------
 *  3. Per-device REQUEST addresses the current device
 * -----------------------------------------------------------*/

void test_broadcast_request_match_rom_per_device(void) {
    setup_scan(2);

    /* IDLE -> CONVERT -> WAIT -> CONTINUE */
    mock_tim1.SR |= TIM_SR_UIF;
    ds18b20_poll();
    set_presence_ok();
    mock_tim1.SR |= TIM_SR_UIF;
    ds18b20_poll();
    mock_tim1.SR |= TIM_SR_UIF;
    ds18b20_poll();
    TEST_ASSERT_EQUAL_UINT8(DS18B20_ST_CONTINUE, ds18b20_test_get_state());

    /* CONTINUE -> REQUEST: state entry only, addressing not applied yet. */
    set_presence_ok();
    mock_tim1.SR |= TIM_SR_UIF;
    ds18b20_poll();
    TEST_ASSERT_EQUAL_UINT8(DS18B20_ST_REQUEST, ds18b20_test_get_state());

    /* REQUEST -> READ: the device is addressed via Match ROM and read. */
    set_presence_ok();
    mock_tim1.SR |= TIM_SR_UIF;
    ds18b20_poll();
    TEST_ASSERT_EQUAL_UINT8(DS18B20_ST_READ, ds18b20_test_get_state());
    TEST_ASSERT_EQUAL_UINT8(1, ds18b20_test_get_address_mode());
    uint8_t sel[DS18B20_ROM_BYTES];
    ds18b20_test_get_selected_rom(sel);
    for (int i = 0; i < DS18B20_ROM_BYTES; i++) {
        TEST_ASSERT_EQUAL_UINT8(k_roms[0][i], sel[i]);
    }
}

/*-------------------------------------------------------------
 *  4. Full round: one convert, three reads, back to IDLE
 * -----------------------------------------------------------*/

void test_broadcast_full_round_three_devices(void) {
    setup_scan(3);

    /* 1. IDLE -> CONVERT */
    mock_tim1.SR |= TIM_SR_UIF;
    ds18b20_poll();
    TEST_ASSERT_EQUAL_UINT8(DS18B20_ST_CONVERT, ds18b20_test_get_state());

    /* 2. CONVERT -> WAIT (broadcast: Skip ROM, no Match ROM address) */
    set_presence_ok();
    mock_tim1.SR |= TIM_SR_UIF;
    ds18b20_poll();
    TEST_ASSERT_EQUAL_UINT8(DS18B20_ST_WAIT, ds18b20_test_get_state());
    TEST_ASSERT_EQUAL_UINT8(0, ds18b20_test_get_address_mode());

    /* 3. WAIT -> CONTINUE */
    mock_tim1.SR |= TIM_SR_UIF;
    ds18b20_poll();
    TEST_ASSERT_EQUAL_UINT8(DS18B20_ST_CONTINUE, ds18b20_test_get_state());

    /* 4..6. Device 0, 1, 2 read back one by one. */
    read_one_device(0, 1, 223);
    read_one_device(1, 1, 223);
    read_one_device(2, 0, 223);

    TEST_ASSERT_EQUAL_UINT8(3, test_spy_complete_count);
    for (int i = 0; i < 3; i++) {
        TEST_ASSERT_EQUAL_UINT8((uint8_t)i, test_spy_complete_indices[i]);
        TEST_ASSERT_EQUAL_INT(223, test_spy_complete_values[i]);
    }
    TEST_ASSERT_EQUAL_UINT8(DS18B20_ST_IDLE, ds18b20_test_get_state());
    /* Inter-measurement pause armed after the last device. */
    TEST_ASSERT_TRUE(mock_tim1.CR1 & TIM_CR1_CEN);
}

/*-------------------------------------------------------------
 *  5. A missing device reports NO_SENSOR and the scan continues
 * -----------------------------------------------------------*/

void test_broadcast_missing_device_continues(void) {
    setup_scan(3);

    /* Convert + wait, ready for the first read-back. */
    mock_tim1.SR |= TIM_SR_UIF;
    ds18b20_poll();
    set_presence_ok();
    mock_tim1.SR |= TIM_SR_UIF;
    ds18b20_poll();
    mock_tim1.SR |= TIM_SR_UIF;
    ds18b20_poll();

    /* Device 0: valid reading. */
    read_one_device(0, 1, 223);

    /* Device 1: addressed but absent -> NO_SENSOR, scan moves on. */
    set_presence_ok();
    mock_tim1.SR |= TIM_SR_UIF;
    ds18b20_poll();
    set_presence_ok();
    mock_tim1.SR |= TIM_SR_UIF;
    ds18b20_poll();
    mock_tim1.SR |= TIM_SR_UIF;
    ds18b20_poll();
    set_scratchpad_absent();
    mock_tim1.SR |= TIM_SR_UIF;
    ds18b20_poll();
    TEST_ASSERT_EQUAL_INT(DS18B20_TEMP_ERROR_NO_SENSOR, test_spy_complete_values[1]);
    TEST_ASSERT_EQUAL_UINT8(1, test_spy_complete_indices[1]);
    TEST_ASSERT_EQUAL_UINT8(DS18B20_ST_CONTINUE, ds18b20_test_get_state());
    TEST_ASSERT_TRUE(mock_tim1.CR1 & TIM_CR1_CEN); /* bridge armed after NO_SENSOR too */

    /* Device 2: valid reading, round ends at IDLE. */
    read_one_device(2, 0, 223);
    TEST_ASSERT_EQUAL_UINT8(3, test_spy_complete_count);
    TEST_ASSERT_EQUAL_UINT8(DS18B20_ST_IDLE, ds18b20_test_get_state());
}

/*-------------------------------------------------------------
 *  6. Single device: round ends at IDLE (no trailing CONTINUE)
 * -----------------------------------------------------------*/

void test_broadcast_single_device_round(void) {
    setup_scan(1);

    mock_tim1.SR |= TIM_SR_UIF;
    ds18b20_poll();
    set_presence_ok();
    mock_tim1.SR |= TIM_SR_UIF;
    ds18b20_poll();
    mock_tim1.SR |= TIM_SR_UIF;
    ds18b20_poll();

    read_one_device(0, 0, 223);
    TEST_ASSERT_EQUAL_UINT8(1, test_spy_complete_count);
    TEST_ASSERT_EQUAL_UINT8(DS18B20_ST_IDLE, ds18b20_test_get_state());
}

/*-------------------------------------------------------------
 *  7. Single-device mode is unaffected (Match ROM stays)
 * -----------------------------------------------------------*/

void test_broadcast_select_clears_scan_mode(void) {
    setup_scan(1);

    /* Single-device selection exits scan mode; the convert stays Match ROM. */
    ds18b20_select(k_roms[0]);
    TEST_ASSERT_EQUAL_UINT8(0, ds18b20_test_get_scan_mode());
    TEST_ASSERT_EQUAL_UINT8(1, ds18b20_test_get_address_mode());

    mock_tim1.SR |= TIM_SR_UIF;
    ds18b20_poll();
    TEST_ASSERT_EQUAL_UINT8(DS18B20_ST_CONVERT, ds18b20_test_get_state());

    set_presence_ok();
    mock_tim1.SR |= TIM_SR_UIF;
    ds18b20_poll();
    TEST_ASSERT_EQUAL_UINT8(DS18B20_ST_WAIT, ds18b20_test_get_state());
    TEST_ASSERT_EQUAL_UINT8(1, ds18b20_test_get_address_mode());
}

/*-------------------------------------------------------------
 *  8. Public scan API: count, device_rom, out-of-range
 * -----------------------------------------------------------*/

void test_broadcast_public_api(void) {
    ds18b20_init();
    ds18b20_test_reset_ctx();
    ds18b20_test_reset_search();
    ds18b20_test_reset_resolution();

    TEST_ASSERT_EQUAL_UINT8(0, ds18b20_device_count());
    TEST_ASSERT_TRUE(ds18b20_device_rom(0) == 0);

    ds18b20_test_set_device(1, k_roms[1]);
    ds18b20_test_set_device_count(2);
    TEST_ASSERT_EQUAL_UINT8(2, ds18b20_device_count());
    TEST_ASSERT_TRUE(ds18b20_device_rom(0) != 0);
    TEST_ASSERT_EQUAL_UINT8(k_roms[1][3], ds18b20_device_rom(1)[3]);
    TEST_ASSERT_TRUE(ds18b20_device_rom(2) == 0);
    TEST_ASSERT_TRUE(ds18b20_device_rom(255) == 0);
    TEST_ASSERT_EQUAL_UINT8(0, ds18b20_scan_index()); /* IDLE default */
}

/*-------------------------------------------------------------
 *  Run all broadcast tests
 * -----------------------------------------------------------*/
void test_scan_rejected_while_txn_running(void) {
    ds18b20_init();
    ds18b20_test_reset_ctx();
    ds18b20_test_reset_txn();

    /* Make a scan eligible except for the in-flight command transaction. */
    uint8_t rom[8] = {0x28, 0x01, 0x00, 0x03, 0x04, 0x05, 0x06, 0x07};
    ds18b20_test_set_device(0, rom);
    ds18b20_test_set_device_count(1);

    uint8_t buf[8];
    ds18b20_read_rom(buf);
    TEST_ASSERT_EQUAL_UINT8(0, ds18b20_test_get_txn_finished());

    ds18b20_scan_start();
    TEST_ASSERT_EQUAL_UINT8(0, ds18b20_test_get_scan_mode());

    ds18b20_test_reset_txn();
}

void run_test_broadcast(void) {
    TEST_RUN(test_broadcast_start_guards);
    TEST_RUN(test_broadcast_convert_forces_skip_rom);
    TEST_RUN(test_broadcast_request_match_rom_per_device);
    TEST_RUN(test_broadcast_full_round_three_devices);
    TEST_RUN(test_broadcast_missing_device_continues);
    TEST_RUN(test_broadcast_single_device_round);
    TEST_RUN(test_broadcast_select_clears_scan_mode);
    TEST_RUN(test_broadcast_public_api);
    TEST_RUN(test_scan_rejected_while_txn_running);
}
