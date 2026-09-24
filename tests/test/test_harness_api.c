/* ============================================================
 *  test_harness_api.c - Test-support accessor smoke tests
 *
 *  Exercises the ds18b20_test_access / ow_stats_test_access helper
 *  surface that is compiled into the test binary but not otherwise
 *  reached by the functional tests, so the whole test scaffold is
 *  covered.  These are pure accessors; the assertions only guard
 *  against accidental signature drift.
 *
 *  Also covers the Phase-7 public API additions: start-function
 *  return codes + ds18b20_last_status(), ds18b20_set_callbacks()
 *  (runtime handlers override weak symbols), and ds18b20_deinit().
 * ============================================================ */

#include "ds18b20.h"
#include "ds18b20_test_access.h"
#include "ds18b20_test_spy.h"
#include "hw_model.h"
#include "mock_target.h"
#include "ow_stats.h"
#include "ow_stats_test_access.h"
#include "unity.h"

void test_harness_accessor_smoke(void) {
    uint8_t rom[DS18B20_ROM_BYTES] = {0x28, 1, 2, 3, 4, 5, 6, 7};
    uint8_t out[DS18B20_ROM_BYTES];

    ds18b20_test_set_selected_rom(rom);
    ds18b20_test_get_selected_rom(out);
    for (uint8_t i = 0; i < DS18B20_ROM_BYTES; i++) {
        TEST_ASSERT_EQUAL_UINT8(rom[i], out[i]);
    }

    ds18b20_test_set_addr_cmd(0, 0x55);
    TEST_ASSERT_EQUAL_UINT8(0x55, ds18b20_test_get_addr_cmd(0));

    /* build_addr_prefix() derives the Match-ROM prefix from the selection. */
    ds18b20_test_build_addr_prefix();

    TEST_ASSERT_EQUAL_UINT8(0, test_ds18b20_bus_done());
    ds18b20_test_set_search_pulse3(0, 1234);
    TEST_ASSERT_EQUAL_UINT16(1234, test_search_pulse(0));

    ds18b20_test_set_device_count(3);
    TEST_ASSERT_EQUAL_UINT8(3, ds18b20_test_get_device_count());

    /* Out-of-range device index must be ignored (early return). */
    ds18b20_test_set_device(DS18B20_MAX_DEVICES, rom);
}

void test_ow_stats_accessor_bounds(void) {
    /* Out-of-range indices return the safe defaults. */
    TEST_ASSERT_TRUE(ow_stats_test_get_sensor(255) == NULL);
    TEST_ASSERT_EQUAL_UINT32(0, ow_stats_test_get_histogram(255));
    (void)ow_stats_test_get_dump_sensor();
}

/* hw_run_until_uif() has two branches not hit by the functional tests:
 *  - timer not enabled -> returns the raw UIF status (line 142)
 *  - requested slot count clamped below RCR+1 -> returns 0, no terminal UIF */
void test_hw_run_until_uif_branches(void) {
    /* Timer idle (CEN cleared by reset): returns UIF status, no DMA work. */
    hw_reset_all();
    TEST_ASSERT_FALSE(hw_run_until_uif(1));

    /* Arm a 16-slot op (RCR=15) with CEN set, then ask for fewer slots than
     * exist. The slot count is clamped and the loop ends without a terminal
     * update event, exercising the clamp + early-return paths. */
    uint8_t cmd[17];
    for (int i = 0; i < 16; i++) {
        cmd[i] = (i & 1u) ? 5u : 60u;
    }
    cmd[16] = 0;
    test_bus_send_command_n(cmd, 16);
    TEST_ASSERT_FALSE(hw_run_until_uif(1));
}

/* ------------------------------------------------------------
 *  Phase-7 API: start returns, last_status, set_callbacks, deinit
 * ------------------------------------------------------------ */

static int16_t cb_complete_temp;
static uint8_t cb_complete_calls;
static uint8_t cb_busy_action;
static uint8_t cb_busy_calls;
static int cb_user_marker;

static void api_busy_cb(uint8_t action, void* user_ctx) {
    cb_busy_action = action;
    cb_busy_calls++;
    TEST_ASSERT_TRUE(user_ctx == &cb_user_marker);
}

static void api_complete_cb(int16_t temp, void* user_ctx) {
    cb_complete_temp = temp;
    cb_complete_calls++;
    TEST_ASSERT_TRUE(user_ctx == &cb_user_marker);
}

void test_api_start_returns_and_last_status(void) {
    static const uint8_t rom0[DS18B20_ROM_BYTES] = {0x28, 1, 2, 3, 4, 5, 6, 7};

    ds18b20_init();
    ds18b20_test_reset_ctx();
    ds18b20_test_reset_search();
    ds18b20_test_reset_resolution();
    ds18b20_test_reset_txn();

    /* Accepted start reports 1 + OK. */
    TEST_ASSERT_EQUAL_UINT8(1, ds18b20_set_resolution(9));
    TEST_ASSERT_EQUAL_INT(DS18B20_STATUS_OK, ds18b20_last_status());

    /* Re-entry while the resolution change runs: 0 + OWNER. */
    TEST_ASSERT_EQUAL_UINT8(0, ds18b20_set_resolution(10));
    TEST_ASSERT_EQUAL_INT(DS18B20_STATUS_OWNER, ds18b20_last_status());
    ds18b20_test_reset_resolution();

    /* Out-of-range resolution: 0 + INVALID. */
    TEST_ASSERT_EQUAL_UINT8(0, ds18b20_set_resolution(8));
    TEST_ASSERT_EQUAL_INT(DS18B20_STATUS_INVALID, ds18b20_last_status());

    /* Mid-measurement: 0 + BUSY. */
    ds18b20_test_set_state(DS18B20_ST_CONVERT);
    TEST_ASSERT_EQUAL_UINT8(0, ds18b20_search_start(NULL, 1));
    TEST_ASSERT_EQUAL_INT(DS18B20_STATUS_BUSY, ds18b20_last_status());
    ds18b20_test_set_state(DS18B20_ST_IDLE);

    /* Empty device table scan: 0 + EMPTY. */
    ds18b20_test_set_device_count(0);
    TEST_ASSERT_EQUAL_UINT8(0, ds18b20_scan_start());
    TEST_ASSERT_EQUAL_INT(DS18B20_STATUS_EMPTY, ds18b20_last_status());

    /* Non-empty table: accepted. */
    ds18b20_test_set_device(0, rom0);
    ds18b20_test_set_device_count(1);
    TEST_ASSERT_EQUAL_UINT8(1, ds18b20_scan_start());
    TEST_ASSERT_EQUAL_INT(DS18B20_STATUS_OK, ds18b20_last_status());
    ds18b20_test_set_scan_mode(0);
}

void test_api_set_callbacks_overrides_weak(void) {
    cb_complete_temp = 0;
    cb_complete_calls = 0;
    cb_busy_action = 0xFF;
    cb_busy_calls = 0;

    ds18b20_init();
    ds18b20_test_reset_ctx();
    ds18b20_test_reset_search();
    ds18b20_test_reset_resolution();
    ds18b20_test_reset_txn();
    test_spy_reset();
    ds18b20_set_callbacks(api_busy_cb, api_complete_cb, &cb_user_marker);

    /* Dispatch goes through the registered handlers (not the weak/spy). */
    ds18b20_test_set_state(DS18B20_ST_START);
    mock_tim1.SR |= TIM_SR_UIF;
    ds18b20_poll();
    TEST_ASSERT_EQUAL_UINT8(1, cb_busy_calls);
    TEST_ASSERT_EQUAL_UINT8(1, cb_busy_action);
    TEST_ASSERT_EQUAL_UINT8(0, test_spy_busy_calls);

    /* Complete via presence-fail path (issue_command early exit). */
    ds18b20_test_set_state(DS18B20_ST_CONVERT);
    ds18b20_test_set_capture_pulse(0, 100);
    ds18b20_test_set_capture_pulse(1, 100);
    mock_tim1.SR |= TIM_SR_UIF;
    ds18b20_poll();
    TEST_ASSERT_EQUAL_UINT8(1, cb_complete_calls);
    TEST_ASSERT_EQUAL_INT16(DS18B20_TEMP_ERROR_NO_SENSOR, cb_complete_temp);
    TEST_ASSERT_EQUAL_UINT8(0, test_spy_complete_count);

    /* NULL side falls back to weak (spy again). */
    ds18b20_set_callbacks(NULL, NULL, NULL);
    ds18b20_test_reset_ctx();
    test_spy_reset();
    ds18b20_test_set_state(DS18B20_ST_START);
    mock_tim1.SR |= TIM_SR_UIF;
    ds18b20_poll();
    TEST_ASSERT_EQUAL_UINT8(1, test_spy_busy_calls);
}

void test_api_deinit_resets_state(void) {
    ds18b20_init();
    ds18b20_test_reset_ctx();
    ds18b20_test_reset_search();
    ds18b20_test_reset_resolution();
    ds18b20_test_reset_txn();
    ds18b20_set_callbacks(api_busy_cb, api_complete_cb, &cb_user_marker);

    /* Put the driver into a non-idle shape. */
    ds18b20_test_set_state(DS18B20_ST_CONVERT);
    ds18b20_test_set_device_count(2);
    ds18b20_test_set_scan_mode(1);

    ds18b20_deinit();

    TEST_ASSERT_EQUAL_INT(DS18B20_ST_IDLE, ds18b20_test_get_state());
    TEST_ASSERT_EQUAL_UINT8(0, ds18b20_test_get_scan_mode());
    TEST_ASSERT_EQUAL_UINT8(0, ds18b20_device_count());
    TEST_ASSERT_EQUAL_INT(DS18B20_STATUS_OK, ds18b20_last_status());
    TEST_ASSERT_EQUAL_UINT8(1, ds18b20_test_get_txn_finished());

    /* Registered callbacks were cleared: weak spy is used again. */
    test_spy_reset();
    ds18b20_test_set_state(DS18B20_ST_START);
    mock_tim1.SR |= TIM_SR_UIF;
    ds18b20_poll();
    TEST_ASSERT_EQUAL_UINT8(1, test_spy_busy_calls);
}

void run_test_harness_api(void) {
    TEST_RUN(test_harness_accessor_smoke);
    TEST_RUN(test_ow_stats_accessor_bounds);
    TEST_RUN(test_hw_run_until_uif_branches);
    TEST_RUN(test_api_start_returns_and_last_status);
    TEST_RUN(test_api_set_callbacks_overrides_weak);
    TEST_RUN(test_api_deinit_resets_state);
}
