/* ============================================================
 *  test_alarm_search.c - Alarm Search (0xEC) Integration Tests
 *
 *  Runs the non-blocking Maxim Alarm Search ROM (0xEC) state
 *  machine against a simulated single DS18B20. The engine is the
 *  same Maxim search as the device search, so the tests pin down
 *  the differences: the 0xEC command byte on the wire, the count
 *  semantics, and that the scan-mode device table is untouched.
 * ============================================================ */

#include "ds18b20.h"
#include "ds18b20_test_access.h"
#include "hw_model.h"
#include "stm32f1xx.h"
#include "unity.h"
#include <string.h>

#define ONE 5u
#define ZERO 60u

static uint8_t g_rom[8];
static uint8_t g_found_roms[4][8];
static uint8_t g_found_count;
static uint8_t g_wr_bit; /* bit whose pair the next merged write+read returns */

static uint8_t sink(const uint8_t* rom) {
    memcpy(g_found_roms[g_found_count++], rom, 8);
    return 0;
}

/* Run the alarm search engine until it reports completion. */
static void run_loop(void) {
    uint16_t guard = 0;
    for (;;) {
        if (ds18b20_alarm_search_poll()) {
            break;
        }
        if (mock_tim1.CR1 & TIM_CR1_CEN) {
            TEST_ASSERT_TRUE(hw_run_until_uif(100));
        }
        if (++guard > 500) {
            break;
        }
    }
    TEST_ASSERT_TRUE(guard <= 500);
}

/* Infer the running operation from the mock timer and answer its captures:
 * a single ROM that is currently in alarm. */
static uint8_t g_rom_b[8];
static uint8_t g_pass;

/* Two alarmed devices on the bus (differ at bit 1 of byte 1), exercising the
 * ds18b20-layer alarm search with more than one device. */
static uint16_t two_alarm_capture_src(uint32_t idx) {
    uint8_t rcr = (uint8_t)mock_tim1.RCR;
    if (rcr == 0) {
        if (idx == 0) g_pass++;
        return idx == 0 ? 510u : 700u;
    }
    if (rcr == 1) {
        g_wr_bit = 2;
        uint8_t b = (g_rom[0] >> 0) & 1u;
        return (idx == 0) ? (b ? ONE : ZERO) : (b ? ZERO : ONE);
    }
    uint8_t b;
    if (g_wr_bit == 9) {
        b = 2u; /* discrepancy: id=0, cmp=0 -> both devices disagree */
    } else if (g_wr_bit < 9) {
        uint8_t byte = (g_wr_bit - 1u) / 8u;
        uint8_t bit = (g_wr_bit - 1u) % 8u;
        b = (g_rom[byte] >> bit) & 1u;
    } else {
        const uint8_t* rom = (g_pass == 1) ? g_rom : g_rom_b;
        uint8_t byte = (g_wr_bit - 1u) / 8u;
        uint8_t bit = (g_wr_bit - 1u) % 8u;
        b = (rom[byte] >> bit) & 1u;
    }
    if (idx == 0) return 0u;
    if (idx == 1) return b == 2u ? ZERO : (b ? ONE : ZERO);
    g_wr_bit++;
    return b == 2u ? ZERO : (b ? ZERO : ONE);
}

static uint16_t alarm_capture_src(uint32_t idx) {
    uint8_t rcr = (uint8_t)mock_tim1.RCR;
    if (rcr == 0) {
        return idx == 0 ? 510u : 700u; /* reset + presence pulse */
    }
    if (rcr == 1) { /* first read pair: bit 1 */
        uint8_t b = (g_rom[0] >> 0) & 1u;
        return (idx == 0) ? (b ? ONE : ZERO) : (b ? ZERO : ONE);
    }
    /* merged write+read capturing bit g_wr_bit (idx0 = write edge, ignored) */
    uint8_t byte = (g_wr_bit - 1u) / 8u;
    uint8_t bit = (g_wr_bit - 1u) % 8u;
    uint8_t b = (g_rom[byte] >> bit) & 1u;
    if (idx == 0) return 0u;
    if (idx == 1) return b ? ONE : ZERO;
    g_wr_bit++; /* last capture of this op: next op answers the next bit */
    return b ? ZERO : ONE;
}

static void setup_single_device(void) {
    uint8_t serial[7] = {0x28, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06};
    memcpy(g_rom, serial, 7);
    g_rom[7] = ds18b20_crc8(g_rom, 7);
    g_found_count = 0;
    g_wr_bit = 2;
    hw_set_capture_source(alarm_capture_src);
}

/*-------------------------------------------------------------
 *  The alarm search finds the single alarmed device and puts the
 *  0xEC command byte on the wire. The CCR1 feed is pulses[1..7]
 *  of the LSB-first 0xEC encoding plus the trailing bus-release
 *  zero: 0xEC = 1110 1100 -> LSB-first bits 0,0,1,1,0,1,1,1
 *  -> pulses ZERO,ZERO,ONE,ONE,ZERO,ONE,ONE,ONE -> feed
 *  [ZERO,ONE,ONE,ZERO,ONE,ONE,ONE,0].
 * -----------------------------------------------------------*/
void test_alarm_search_finds_device_sends_0xEC(void) {
    setup_single_device();
    ds18b20_alarm_search_start(sink, 1);

    uint16_t guard = 0;
    uint8_t ec_feed_checked = 0;
    for (;;) {
        if (ds18b20_alarm_search_poll()) {
            break;
        }
        if (mock_tim1.CR1 & TIM_CR1_CEN) {
            uint8_t ok = hw_run_until_uif(100);
            TEST_ASSERT_TRUE(ok);
            if (!ec_feed_checked) {
                const hw_ccr1_feed_log_t* log = hw_ccr1_feed_log();
                if (log->count == 8) { /* the 0xEC command feed */
                    ec_feed_checked = 1;
                    static const uint16_t expected[8] = {ZERO, ONE, ONE, ZERO, ONE, ONE, ONE, 0};
                    for (uint8_t i = 0; i < 8; i++) {
                        TEST_ASSERT_EQUAL_UINT16(expected[i], log->values[i]);
                    }
                }
            }
        }
        if (++guard > 500) {
            break;
        }
    }
    TEST_ASSERT_TRUE(guard <= 500);
    TEST_ASSERT_TRUE(ec_feed_checked);

    TEST_ASSERT_EQUAL_UINT8(1, ds18b20_alarm_search_count());
    TEST_ASSERT_EQUAL_UINT8(1, g_found_count);
    for (int i = 0; i < 8; i++) {
        TEST_ASSERT_EQUAL_HEX8(g_rom[i], g_found_roms[0][i]);
    }
}

/*-------------------------------------------------------------
 *  No device on the bus (no presence pulse): alarm search
 *  finishes immediately without finding anything.
 * -----------------------------------------------------------*/
static uint16_t no_presence_src(uint32_t i) {
    (void)i;
    return 100u;
}

void test_alarm_search_no_presence_finds_nothing(void) {
    g_found_count = 0;
    g_wr_bit = 2;
    hw_set_capture_source(no_presence_src);
    ds18b20_alarm_search_start(sink, 4);
    run_loop();

    TEST_ASSERT_EQUAL_UINT8(0, ds18b20_alarm_search_count());
    TEST_ASSERT_EQUAL_UINT8(0, g_found_count);
}

/*-------------------------------------------------------------
 *  max_devices == 0 aborts the alarm search immediately: no
 *  hardware operation is scheduled and DONE is reported.
 * -----------------------------------------------------------*/
void test_alarm_search_max_zero_aborts(void) {
    hw_reset_all();
    g_found_count = 0;
    g_wr_bit = 2;

    ds18b20_alarm_search_start(sink, 0);

    TEST_ASSERT_FALSE(mock_tim1.CR1 & TIM_CR1_CEN);
    TEST_ASSERT_EQUAL_UINT8(1, ds18b20_alarm_search_poll());
    TEST_ASSERT_EQUAL_UINT8(0, ds18b20_alarm_search_count());
    TEST_ASSERT_EQUAL_UINT8(0, g_found_count);
}

/*-------------------------------------------------------------
 *  A bad-ROM family code (not 0x28) is filtered out even in
 *  alarm mode: search completes but reports zero devices.
 * -----------------------------------------------------------*/
void test_alarm_search_filters_non_ds18b20_family(void) {
    uint8_t serial[7] = {0x10, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06};
    memcpy(g_rom, serial, 7);
    g_rom[7] = ds18b20_crc8(g_rom, 7);

    g_found_count = 0;
    g_wr_bit = 2;
    hw_set_capture_source(alarm_capture_src);
    ds18b20_alarm_search_start(sink, 1);
    run_loop();

    TEST_ASSERT_EQUAL_UINT8(0, ds18b20_alarm_search_count());
    TEST_ASSERT_EQUAL_UINT8(0, g_found_count);
}

/*-------------------------------------------------------------
 *  The alarm search must NOT touch the scan-mode device table:
 *  a device table filled by a previous scan keeps its ROMs and
 *  count even though the alarm search finds its own device.
 * -----------------------------------------------------------*/
void test_alarm_search_leaves_device_table_untouched(void) {
    uint8_t roms[2][8] = {{0x28, 0xAA, 0, 0, 0, 0, 0, 0}, {0x28, 0xBB, 0, 0, 0, 0, 0, 0}};
    for (int d = 0; d < 2; d++) {
        roms[d][7] = ds18b20_crc8(roms[d], 7);
        ds18b20_test_set_device(d, roms[d]);
    }
    ds18b20_test_set_device_count(2);

    setup_single_device();
    ds18b20_alarm_search_start(sink, 1);
    run_loop();

    TEST_ASSERT_EQUAL_UINT8(1, ds18b20_alarm_search_count());
    TEST_ASSERT_EQUAL_UINT8(1, g_found_count);

    /* The scan table from the previous device search is untouched. */
    TEST_ASSERT_EQUAL_UINT8(2, ds18b20_device_count());
    for (int i = 0; i < 8; i++) {
        TEST_ASSERT_EQUAL_HEX8(roms[0][i], ds18b20_device_rom(0)[i]);
        TEST_ASSERT_EQUAL_HEX8(roms[1][i], ds18b20_device_rom(1)[i]);
    }
}

/*-------------------------------------------------------------
 *  After an alarm search the device search still sends the
 *  regular 0xF0 command and repopulates the scan device table.
 * -----------------------------------------------------------*/
void test_alarm_then_device_search_repopulates_table(void) {
    setup_single_device();
    ds18b20_alarm_search_start(sink, 1);
    run_loop();

    /* Now run a device search: it must reset and repopulate the table. */
    g_found_count = 0;
    g_wr_bit = 2;
    ds18b20_search_start(sink, 1);
    run_loop();

    TEST_ASSERT_EQUAL_UINT8(1, ds18b20_search_count());
    TEST_ASSERT_EQUAL_UINT8(1, ds18b20_device_count());
    for (int i = 0; i < 8; i++) {
        TEST_ASSERT_EQUAL_HEX8(g_rom[i], ds18b20_device_rom(0)[i]);
    }
}

/*-------------------------------------------------------------
 *  A sink that returns non-zero aborts the alarm search after
 *  the first device, even when max_devices allows more.
 * -----------------------------------------------------------*/
static uint8_t early_stop_calls;
static uint8_t early_stop_sink(const uint8_t* rom) {
    memcpy(g_found_roms[0], rom, 8);
    early_stop_calls++;
    return 1; /* stop the search */
}

void test_alarm_search_sink_early_stop(void) {
    setup_single_device();
    early_stop_calls = 0;
    ds18b20_alarm_search_start(early_stop_sink, 4);
    run_loop();

    TEST_ASSERT_EQUAL_UINT8(1, early_stop_calls);
    TEST_ASSERT_EQUAL_UINT8(1, ds18b20_alarm_search_count());
    for (int i = 0; i < 8; i++) {
        TEST_ASSERT_EQUAL_HEX8(g_rom[i], g_found_roms[0][i]);
    }
}

/*-------------------------------------------------------------
 *  A (id=1, cmp=1) pair means no device follows the path: the
 *  alarm search tree is exhausted and the search terminates.
 * -----------------------------------------------------------*/
static uint16_t exhausted_src(uint32_t idx) {
    uint8_t rcr = (uint8_t)mock_tim1.RCR;
    if (rcr == 0) {
        return idx == 0 ? 510u : 700u; /* presence present */
    }
    if (rcr == 1) { /* first read pair: id=1, cmp=1 */
        g_wr_bit = 2;
        return (idx == 0) ? ONE : ONE;
    }
    return 0u; /* unreachable: search terminates before the merged ops */
}

void test_alarm_search_pair_11_terminates(void) {
    g_found_count = 0;
    g_wr_bit = 2;
    hw_set_capture_source(exhausted_src);
    ds18b20_alarm_search_start(sink, 4);
    run_loop();

    TEST_ASSERT_EQUAL_UINT8(0, ds18b20_alarm_search_count());
    TEST_ASSERT_EQUAL_UINT8(0, g_found_count);
    TEST_ASSERT_EQUAL_UINT8(1, ds18b20_alarm_search_poll()); /* finished */
}

/*-------------------------------------------------------------
 *  A NULL sink must not be dereferenced: the alarm search runs
 *  to completion and counts alarmed devices internally.
 * -----------------------------------------------------------*/
void test_alarm_search_null_sink_completes(void) {
    setup_single_device();
    ds18b20_alarm_search_start(NULL, 1);
    run_loop();

    TEST_ASSERT_EQUAL_UINT8(1, ds18b20_alarm_search_count());
    TEST_ASSERT_EQUAL_UINT8(0, g_found_count);
}

/*-------------------------------------------------------------
 *  A device whose ROM CRC is wrong is rejected: the alarm search
 *  runs the full 64 bits, then discards the ROM and reports
 *  nothing.
 * -----------------------------------------------------------*/
void test_alarm_search_rejects_bad_crc_rom(void) {
    uint8_t serial[7] = {0x28, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06};
    memcpy(g_rom, serial, 7);
    g_rom[7] = (uint8_t)(ds18b20_crc8(g_rom, 7) ^ 0xFF); /* corrupt CRC */

    g_found_count = 0;
    g_wr_bit = 2;
    hw_set_capture_source(alarm_capture_src);
    ds18b20_alarm_search_start(sink, 1);
    run_loop();

    TEST_ASSERT_EQUAL_UINT8(0, ds18b20_alarm_search_count());
    TEST_ASSERT_EQUAL_UINT8(0, g_found_count);
}

/*-------------------------------------------------------------
 *  ds18b20_alarm_search_start() while a search is already
 *  running is ignored: the running search keeps its sink.
 * -----------------------------------------------------------*/
static uint8_t g_reentry_b_calls;
static uint8_t reentry_sink_b(const uint8_t* rom) {
    (void)rom;
    g_reentry_b_calls++;
    return 0;
}

void test_alarm_search_reentry_ignored(void) {
    setup_single_device();
    g_reentry_b_calls = 0;
    ds18b20_alarm_search_start(sink, 1);

    ds18b20_alarm_search_start(reentry_sink_b, 4); /* ignored */
    run_loop();

    TEST_ASSERT_EQUAL_UINT8(1, ds18b20_alarm_search_count());
    TEST_ASSERT_EQUAL_UINT8(1, g_found_count);
    TEST_ASSERT_EQUAL_UINT8(0, g_reentry_b_calls);
}

/*-------------------------------------------------------------
 *  ds18b20_alarm_search_start() mid-measurement (non-IDLE state)
 *  is ignored: nothing is scheduled and no search is started.
 * -----------------------------------------------------------*/
void test_alarm_search_blocked_mid_measurement(void) {
    ds18b20_test_set_state(DS18B20_ST_CONVERT);

    g_found_count = 0;
    g_wr_bit = 2;
    ds18b20_alarm_search_start(sink, 1);

    TEST_ASSERT_FALSE(mock_tim1.CR1 & TIM_CR1_CEN);
    TEST_ASSERT_EQUAL_UINT8(1, ds18b20_alarm_search_poll());
    TEST_ASSERT_EQUAL_UINT8(0, ds18b20_alarm_search_count());
    TEST_ASSERT_EQUAL_UINT8(0, g_found_count);
}

/*-------------------------------------------------------------
 *  Run all alarm search tests
 * -----------------------------------------------------------*/
void test_alarm_search_rejected_while_txn_running(void) {
    ds18b20_init();
    ds18b20_test_reset_ctx();
    ds18b20_test_reset_txn();

    uint8_t buf[8];
    ds18b20_read_rom(buf);
    TEST_ASSERT_EQUAL_UINT8(0, ds18b20_test_get_txn_finished());

    g_found_count = 0;
    ds18b20_alarm_search_start(sink, 1);
    TEST_ASSERT_EQUAL_UINT8(1, ds18b20_alarm_search_poll());
    TEST_ASSERT_EQUAL_UINT8(0, ds18b20_alarm_search_count());

    ds18b20_test_reset_txn();
}

void test_alarm_search_two_devices_found(void) {
    uint8_t romA[8] = {DS18B20_FAMILY_CODE, 0x00, 0x11, 0x22, 0x33, 0x44, 0x55, 0x00};
    uint8_t romB[8] = {DS18B20_FAMILY_CODE, 0x01, 0x11, 0x22, 0x33, 0x44, 0x55, 0x00};
    romA[7] = ds18b20_crc8(romA, 7);
    romB[7] = ds18b20_crc8(romB, 7);
    memcpy(g_rom, romA, 8);
    memcpy(g_rom_b, romB, 8);

    g_found_count = 0;
    g_wr_bit = 2;
    g_pass = 0;
    hw_set_capture_source(two_alarm_capture_src);
    ds18b20_alarm_search_start(sink, 2);

    uint16_t guard = 0;
    for (;;) {
        if (ds18b20_alarm_search_poll()) {
            break;
        }
        if (mock_tim1.CR1 & TIM_CR1_CEN) {
            TEST_ASSERT_TRUE(hw_run_until_uif(100));
        }
        if (++guard > 500) {
            break;
        }
    }
    TEST_ASSERT_TRUE(guard <= 500);

    TEST_ASSERT_EQUAL_UINT8(2, ds18b20_alarm_search_count());
    TEST_ASSERT_EQUAL_UINT8(2, g_found_count);
    for (int i = 0; i < 8; i++) {
        TEST_ASSERT_EQUAL_HEX8(romA[i], g_found_roms[0][i]);
        TEST_ASSERT_EQUAL_HEX8(romB[i], g_found_roms[1][i]);
    }
}

void run_test_alarm_search(void) {
    TEST_RUN(test_alarm_search_finds_device_sends_0xEC);
    TEST_RUN(test_alarm_search_no_presence_finds_nothing);
    TEST_RUN(test_alarm_search_max_zero_aborts);
    TEST_RUN(test_alarm_search_filters_non_ds18b20_family);
    TEST_RUN(test_alarm_search_leaves_device_table_untouched);
    TEST_RUN(test_alarm_then_device_search_repopulates_table);
    TEST_RUN(test_alarm_search_sink_early_stop);
    TEST_RUN(test_alarm_search_pair_11_terminates);
    TEST_RUN(test_alarm_search_null_sink_completes);
    TEST_RUN(test_alarm_search_rejects_bad_crc_rom);
    TEST_RUN(test_alarm_search_reentry_ignored);
    TEST_RUN(test_alarm_search_blocked_mid_measurement);
    TEST_RUN(test_alarm_search_rejected_while_txn_running);
    TEST_RUN(test_alarm_search_two_devices_found);
}
