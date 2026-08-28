/* ============================================================
 *  test_main_active.c - Runner for the active-drive test build
 *
 *  Mirrors tests/test/test_main.c but runs only the active-drive
 *  test set, and is compiled with -DOW_DRIVE_ACTIVE. The full suite
 *  is intentionally excluded: several pin-regression assertions in the
 *  standard suite assume the pin is never toggled outside the parasite
 *  strong-pull-up path, which the active-drive write path also does.
 * ============================================================ */

#include "ds18b20_test_access.h"
#include "ds18b20_test_spy.h"
#include "hw_model.h"
#include "ow_stats.h"
#include "onewire.h"
#include "unity.h"

int unity_failures = 0;

void setUp(void) {
    hw_reset_all();
    ds18b20_test_register_buffers();
    ds18b20_test_reset_ctx();
    ds18b20_test_reset_search();
    ds18b20_test_reset_resolution();
    ds18b20_test_reset_txn();
    test_spy_reset();
    ds18b20_test_set_gap_us(0);
    ow_stats_init();
}

void tearDown(void) {
    /* nothing to clean up */
}

extern void run_test_active_drive(void);

int main(void) {
    run_test_active_drive();
    printf("%s: %d failure(s)\n", unity_failures ? "FAIL" : "PASS", unity_failures);
    return unity_failures ? 1 : 0;
}
