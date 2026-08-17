/* ============================================================
 *  ds18b20_test_spy.c - Shared callback spies
 *
 *  Provides the strong ds18b20_complete()/ds18b20_busy()
 *  definitions that override the driver's weak defaults, plus a
 *  record of every invocation. See ds18b20_test_spy.h.
 * ============================================================ */

#include "ds18b20_test_spy.h"
#include "ds18b20.h"

int16_t test_spy_complete_value;
uint8_t test_spy_complete_called;
uint8_t test_spy_complete_count;
int16_t test_spy_complete_values[TEST_SPY_MAX_COMPLETES];
uint8_t test_spy_complete_indices[TEST_SPY_MAX_COMPLETES];
uint8_t test_spy_busy_calls;
unsigned test_spy_busy_last_action;
void (*test_spy_on_complete_hook)(void); /* optional per-test hook invoked inside ds18b20_complete */

void test_spy_reset(void) {
    test_spy_complete_value = 0;
    test_spy_complete_called = 0;
    test_spy_complete_count = 0;
    for (uint8_t i = 0; i < TEST_SPY_MAX_COMPLETES; i++) {
        test_spy_complete_values[i] = 0;
        test_spy_complete_indices[i] = 0;
    }
    test_spy_busy_calls = 0;
    test_spy_busy_last_action = 0;
    test_spy_on_complete_hook = 0;
}

void ds18b20_complete(int16_t temp) {
    test_spy_complete_value = temp;
    test_spy_complete_called = 1;
    if (test_spy_complete_count < TEST_SPY_MAX_COMPLETES) {
        test_spy_complete_values[test_spy_complete_count] = temp;
        test_spy_complete_indices[test_spy_complete_count] = ds18b20_scan_index();
    }
    test_spy_complete_count++;
    if (test_spy_on_complete_hook) {
        test_spy_on_complete_hook();
    }
}

void ds18b20_busy(unsigned action) {
    test_spy_busy_calls++;
    test_spy_busy_last_action = action;
}
