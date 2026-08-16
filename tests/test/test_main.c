/* ============================================================
 *  test_main.c - Host test runner
 *
 *  Build:  make test            (host toolchain, runs on the PC)
 *  Driver: tests/mock/ds18b20_test_access.c (compiles src/ds18b20.c)
 *  HW:     tests/mock/hw_model.c (TIM1/DMA behavioural model)
 * ============================================================ */

#include "ds18b20_test_access.h"
#include "ds18b20_test_spy.h"
#include "hw_model.h"
#include "unity.h"

int unity_failures = 0;

void setUp(void) {
    hw_reset_all();
    ds18b20_test_register_buffers();
    ds18b20_test_reset_ctx();
    ds18b20_test_reset_search();
    ds18b20_test_reset_resolution();
    test_spy_reset();
    ds18b20_test_set_gap_us(0);
}

void tearDown(void) {
    /* nothing to clean up */
}

extern void run_test_scratchpad(void);
extern void run_test_state_machine(void);
extern void run_test_bus_release(void);
extern void run_test_search(void);
extern void run_test_crc8(void);
extern void run_test_pulse_encoding(void);
extern void run_test_presence(void);
extern void run_test_rom_addressing(void);
extern void run_test_timing(void);
extern void run_test_temperature(void);
extern void run_test_resolution(void);
extern void run_test_broadcast(void);

int main(void) {
    run_test_scratchpad();
    run_test_state_machine();
    run_test_bus_release();
    run_test_search();
    run_test_crc8();
    run_test_pulse_encoding();
    run_test_presence();
    run_test_rom_addressing();
    run_test_timing();
    run_test_temperature();
    run_test_resolution();
    run_test_broadcast();
    printf("%s: %d failure(s)\n", unity_failures ? "FAIL" : "PASS", unity_failures);
    return unity_failures ? 1 : 0;
}
