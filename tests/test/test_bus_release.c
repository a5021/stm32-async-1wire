/* ============================================================
 *  test_bus_release.c - Hardware Bus Release Tests
 *
 *  Regression suite for the "no software bus release" rework.
 *  Every 1-Wire operation must leave the bus idle HIGH at the
 *  moment ds18b20_bus_done() reports completion, so the next
 *  slot's write starts from a clean falling edge regardless of
 *  how long software takes to poll:
 *
 *    - DMA-fed writes (send_command_n, write_then_read) append
 *      a trailing 0 to the CCR3 feed -> last DMA value is 0.
 *    - direct-write/capture ops (reset, read, single slot) use
 *      an OC3PE preload of 0 -> shadow CCR3 is 0 at the
 *      terminal update event (OPM stop).
 *
 *  The model asserts the resulting invariant directly:
 *  after completion, hw_effective_ccr1() == 0 (line idle HIGH)
 *  and the timer has stopped (CR1.CEN == 0).
 * ============================================================ */

#include "ds18b20.h"
#include "ds18b20_test_access.h"
#include "hw_model.h"
#include "mock_target.h"
#include "unity.h"

#define ONE 5u
#define ZERO 60u

static void complete_op(uint32_t max_slots) {
    uint8_t ok = hw_run_until_uif(max_slots);
    TEST_ASSERT_TRUE(ok);
}

static void assert_bus_released(void) {
    TEST_ASSERT_EQUAL_UINT16(0, hw_effective_ccr1());
    TEST_ASSERT_FALSE(mock_tim1.CR1 & TIM_CR1_CEN);
}

/* --- capture sources --- */
static uint16_t src_reset_present(uint32_t i) { return i == 0 ? 510u : 700u; }
static uint16_t src_reset_absent(uint32_t i) {
    (void)i;
    return 100u;
}
static uint16_t src_wr_read_one(uint32_t i) { return i == 0 ? 0u : (i == 1 ? ONE : ZERO); }
static uint16_t src_wr_read_zero(uint32_t i) { return i == 0 ? 0u : (i == 1 ? ZERO : ONE); }
static uint16_t src_pair_id_one(uint32_t i) { return i == 0 ? ONE : ZERO; }
static uint16_t src_pair_id_zero(uint32_t i) { return i == 0 ? ZERO : ONE; }

static const uint8_t* g_scratch;
static uint16_t src_read_scratchpad(uint32_t i) {
    uint8_t bit = (g_scratch[i / 8u] >> (i % 8u)) & 1u;
    return bit ? (uint16_t)ONE : (uint16_t)ZERO;
}

/*-------------------------------------------------------------
 *  DMA-fed 2-byte command (16 slots): trailing 0 in the feed,
 *  all 16 data pulses correct, bus released on completion.
 * -----------------------------------------------------------*/
void test_write_command_trailing_zero_release(void) {
    uint8_t cmd[17];
    for (int i = 0; i < 16; i++) {
        cmd[i] = (i & 1) ? (uint8_t)ONE : (uint8_t)ZERO;
    }
    cmd[16] = 0; /* trailing bus-release zero at index `slots` */

    hw_register_buf(&cmd[1]); /* the driver feeds CCR3 from &cmd[1] */
    hw_set_capture_source(NULL);
    test_bus_send_command_n(cmd, 16);
    complete_op(20);

    const hw_ccr1_feed_log_t* log = hw_ccr1_feed_log();
    TEST_ASSERT_EQUAL_UINT8(16, log->count);
    TEST_ASSERT_EQUAL_UINT16(0, log->values[15]); /* last DMA feed releases the bus */
    for (int i = 0; i < 15; i++) {
        TEST_ASSERT_EQUAL_UINT16(cmd[i + 1], log->values[i]);
    }
    assert_bus_released();
}

/*-------------------------------------------------------------
 *  Single-slot write (no DMA): OC3PE preload 0 armed before
 *  the timer starts; bus released when the timer stops.
 * -----------------------------------------------------------*/
void test_single_slot_write_preload_zero(void) {
    hw_set_capture_source(NULL);
    test_bus_write_bit(1);
    TEST_ASSERT_TRUE(MOCK_TIM_OUT_CCMR & MOCK_TIM_OUT_PE);
    TEST_ASSERT_EQUAL_UINT16(0, MOCK_TIM_OUT_CCR); /* preload 0 */
    complete_op(4);
    assert_bus_released();
}

/*-------------------------------------------------------------
 *  Reset: capture count, presence decode, and bus release.
 * -----------------------------------------------------------*/
void test_reset_releases_and_presence_detect(void) {
    hw_set_capture_source(src_reset_present);
    test_bus_reset();
    complete_op(4);
    assert_bus_released();
    TEST_ASSERT_TRUE(MOCK_TIM_OUT_CCMR & MOCK_TIM_OUT_PE);
    TEST_ASSERT_EQUAL_UINT32(2, hw_capture_count());
    TEST_ASSERT_TRUE(test_bus_present());

    hw_set_capture_source(src_reset_absent);
    test_bus_reset();
    complete_op(4);
    TEST_ASSERT_FALSE(test_bus_present());
}

/*-------------------------------------------------------------
 *  Merged write+read (3 slots): feed is [read, read, 0] and
 *  the trailing zero lands in the last slot (hardware release),
 *  while the captured id/cmp pair decodes correctly.
 * -----------------------------------------------------------*/
void test_merged_write_read_trailing_zero(void) {
    hw_set_capture_source(src_wr_read_one);
    test_bus_write_then_read(1);
    complete_op(6);

    const hw_ccr1_feed_log_t* log = hw_ccr1_feed_log();
    TEST_ASSERT_EQUAL_UINT8(3, log->count);
    TEST_ASSERT_EQUAL_UINT16(ONE, log->values[0]);
    TEST_ASSERT_EQUAL_UINT16(ONE, log->values[1]);
    TEST_ASSERT_EQUAL_UINT16(0, log->values[2]); /* trailing release zero */
    TEST_ASSERT_EQUAL_UINT32(3, hw_capture_count());
    assert_bus_released();

    TEST_ASSERT_EQUAL_UINT16(ONE, test_search_edge(1)); /* id bit = 1 */
    TEST_ASSERT_EQUAL_UINT16(ZERO, test_search_edge(2)); /* cmp bit = 0 */
}

/*-------------------------------------------------------------
 *  Merged write+read with the opposite bit (id=0, cmp=1).
 * -----------------------------------------------------------*/
void test_merged_write_read_decodes_zeros(void) {
    hw_set_capture_source(src_wr_read_zero);
    test_bus_write_then_read(0);
    complete_op(6);
    assert_bus_released();
    TEST_ASSERT_EQUAL_UINT16(ZERO, test_search_edge(1));
    TEST_ASSERT_EQUAL_UINT16(ONE, test_search_edge(2));
}

/*-------------------------------------------------------------
 *  End-to-end read: real read_data() hardware path captures
 *  72 slots into ctx.pulse and decode_scratchpad() reconstructs
 *  the exact scratchpad (incl. a valid CRC byte).
 * -----------------------------------------------------------*/
void test_read_data_hardware_path_decode(void) {
    uint8_t sp[9] = {0x64, 0x01, 0x4B, 0x46, 0x7F, 0xFF, 0x08, 0x10, 0};
    sp[8] = ds18b20_crc8(sp, 8);
    g_scratch = sp;

    hw_set_capture_source(src_read_scratchpad);
    test_bus_read_data();
    complete_op(100);
    assert_bus_released();
    TEST_ASSERT_EQUAL_UINT32(72, hw_capture_count());

    ds18b20_test_decode_scratchpad();
    for (int i = 0; i < 9; i++) {
        TEST_ASSERT_EQUAL_HEX8(sp[i], ds18b20_test_get_scratchpad(i));
    }
}

/*-------------------------------------------------------------
 *  Full measurement-cycle scheduling: reset -> command -> read,
 *  with the bus verified idle-HIGH after every single operation
 *  (the guarantee that makes a slow polling loop safe).
 * -----------------------------------------------------------*/
void test_sequence_stays_released_between_ops(void) {
    hw_set_capture_source(src_reset_present);
    test_bus_reset();
    complete_op(4);
    assert_bus_released();

    uint8_t cmd[17];
    for (int i = 0; i < 16; i++) {
        cmd[i] = (i & 1) ? (uint8_t)ONE : (uint8_t)ZERO;
    }
    cmd[16] = 0;
    hw_register_buf(&cmd[1]);
    test_bus_send_command_n(cmd, 16);
    complete_op(20);
    assert_bus_released();

    uint8_t sp[9] = {0x64, 0x01, 0x4B, 0x46, 0x7F, 0xFF, 0x08, 0x10, 0};
    sp[8] = ds18b20_crc8(sp, 8);
    g_scratch = sp;
    hw_set_capture_source(src_read_scratchpad);
    test_bus_read_data();
    complete_op(100);
    assert_bus_released();
}

/*-------------------------------------------------------------
 *  Standalone read_pair (rcr=1, 2 captures): OC3PE preload 0,
 *  id/cmp decoded from ctx.edge, bus released on completion.
 * -----------------------------------------------------------*/
void test_read_pair_standalone_release(void) {
    hw_set_capture_source(src_pair_id_one);
    test_bus_read_pair();
    TEST_ASSERT_TRUE(MOCK_TIM_OUT_CCMR & MOCK_TIM_OUT_PE);
    TEST_ASSERT_EQUAL_UINT16(0, MOCK_TIM_OUT_CCR); /* preload 0 */
    complete_op(4);
    assert_bus_released();
    TEST_ASSERT_EQUAL_UINT32(2, hw_capture_count());
    TEST_ASSERT_EQUAL_UINT16(ONE, ds18b20_test_get_edge(0));
    TEST_ASSERT_EQUAL_UINT16(ZERO, ds18b20_test_get_edge(1));

    hw_set_capture_source(src_pair_id_zero);
    test_bus_read_pair();
    complete_op(4);
    assert_bus_released();
    TEST_ASSERT_EQUAL_UINT16(ZERO, ds18b20_test_get_edge(0));
    TEST_ASSERT_EQUAL_UINT16(ONE, ds18b20_test_get_edge(1));
}

/*-------------------------------------------------------------
 *  Long pure-timer waits (wait_conversion = 750ms, RCR=11;
 *  start_cycle_pause = 5s, RCR=79). They never touch CCR3,
 *  so the line stays idle HIGH for the whole wait and the bus
 *  is still released when the timer stops.
 * -----------------------------------------------------------*/
void test_wait_and_pause_keep_bus_released(void) {
    uint8_t cmd[17];
    for (int i = 0; i < 16; i++) {
        cmd[i] = (i & 1) ? (uint8_t)ONE : (uint8_t)ZERO;
    }
    cmd[16] = 0;
    hw_register_buf(&cmd[1]);
    hw_set_capture_source(NULL);
    test_bus_send_command_n(cmd, 16);
    complete_op(20);
    assert_bus_released();

    test_bus_wait_conversion();
    TEST_ASSERT_EQUAL_UINT8(11, mock_tim1.RCR);
    complete_op(100); /* 12 slots */
    assert_bus_released();

    test_bus_start_cycle_pause();
    TEST_ASSERT_EQUAL_UINT8(79, mock_tim1.RCR);
    complete_op(100); /* 80 slots */
    assert_bus_released();
}

/*-------------------------------------------------------------
 *  Run all bus-release tests
 * -----------------------------------------------------------*/
void run_test_bus_release(void) {
    TEST_RUN(test_write_command_trailing_zero_release);
    TEST_RUN(test_single_slot_write_preload_zero);
    TEST_RUN(test_reset_releases_and_presence_detect);
    TEST_RUN(test_merged_write_read_trailing_zero);
    TEST_RUN(test_merged_write_read_decodes_zeros);
    TEST_RUN(test_read_pair_standalone_release);
    TEST_RUN(test_read_data_hardware_path_decode);
    TEST_RUN(test_wait_and_pause_keep_bus_released);
    TEST_RUN(test_sequence_stays_released_between_ops);
}
