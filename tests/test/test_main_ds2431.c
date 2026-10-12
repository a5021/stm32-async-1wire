/* ============================================================
 *  test_main_ds2431.c - Host test runner for the DS2431 driver
 *
 *  Build:  make test-ds2431    (host toolchain, runs on the PC)
 *  Driver: tests/mock/ds2431_test_access.c (compiles src/ds2431.c)
 *  HW:     tests/mock/hw_model.c (TIM1/DMA behavioural model)
 * ============================================================ */

#include "ds2431.h"
#include "ds2431_test_access.h"
#include "hw_model.h"
#include "unity.h"

int unity_failures = 0;

void setUp(void) {
    hw_reset_all();
    ds2431_test_register_buffers();
    ds2431_init();
}

void tearDown(void) {
    /* nothing to clean up */
}

extern void run_test_ds2431(void);

int main(void) {
    run_test_ds2431();
    printf("%s: %d failure(s)\n", unity_failures ? "FAIL" : "PASS", unity_failures);
    return unity_failures ? 1 : 0;
}