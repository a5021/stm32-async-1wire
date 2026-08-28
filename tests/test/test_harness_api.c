/* ============================================================
 *  test_harness_api.c - Test-support accessor smoke tests
 *
 *  Exercises the ds18b20_test_access / ow_stats_test_access helper
 *  surface that is compiled into the test binary but not otherwise
 *  reached by the functional tests, so the whole test scaffold is
 *  covered.  These are pure accessors; the assertions only guard
 *  against accidental signature drift.
 * ============================================================ */

#include "ds18b20.h"
#include "ds18b20_test_access.h"
#include "ds18b20_test_spy.h"
#include "hw_model.h"
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
    ds18b20_test_set_search_edge3(0, 1234);
    TEST_ASSERT_EQUAL_UINT16(1234, test_search_edge(0));

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

void run_test_harness_api(void) {
    TEST_RUN(test_harness_accessor_smoke);
    TEST_RUN(test_ow_stats_accessor_bounds);
    TEST_RUN(test_hw_run_until_uif_branches);
}
