/* ============================================================
 *  test_main.c - Host test runner
 *
 *  Build:  make test            (host toolchain, runs on the PC)
 *  Driver: tests/mock/ds18b20_test_access.c (compiles src/ds18b20.c)
 *  HW:     tests/mock/hw_model.c (TIM1/DMA behavioural model)
 * ============================================================ */

#include "unity.h"
#include "hw_model.h"
#include "ds18b20_test_access.h"

int unity_failures = 0;

void setUp(void) {
    hw_reset_all();
    ds18b20_test_register_buffers();
    ds18b20_test_reset_ctx();
}

void tearDown(void) {
    /* nothing to clean up */
}

extern void run_test_scratchpad(void);
extern void run_test_state_machine(void);
extern void run_test_bus_release(void);
extern void run_test_search(void);

int main(void) {
    run_test_scratchpad();
    run_test_state_machine();
    run_test_bus_release();
    run_test_search();
    printf("%s: %d failure(s)\n", unity_failures ? "FAIL" : "PASS", unity_failures);
    return unity_failures ? 1 : 0;
}
