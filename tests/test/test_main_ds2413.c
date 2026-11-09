/* ============================================================
 *  test_main_ds2413.c - Host test runner for the DS2413 driver
 *
 *  Build:  make test-ds2413    (host toolchain, runs on the PC)
 *  Driver: tests/mock/ds2413_test_access.c (compiles src/ds2413.c)
 *  HW:     tests/mock/hw_model.c (TIM1/DMA behavioural model)
 * ============================================================ */

#include "ds2413.h"
#include "ds2413_test_access.h"
#include "hw_model.h"
#include "unity.h"

int unity_failures = 0;

void setUp(void) {
    hw_reset_all();
    ds2413_test_register_buffers();
    ds2413_init();
}

void tearDown(void) {
    /* nothing to clean up */
}

extern void run_test_ds2413(void);

int main(void) {
    run_test_ds2413();
    printf("%s: %d failure(s)\n", unity_failures ? "FAIL" : "PASS", unity_failures);
    return unity_failures ? 1 : 0;
}
