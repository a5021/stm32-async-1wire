/* ============================================================
 *  test_clock_check.c - startup clock-agreement check tests
 *
 *  onewire_clock_ok() compares a runtime MHz reading against the clock the
 *  port timing was built for. The main suite always defines -DOW_SYSCLK_MHZ,
 *  so these tests cover that branch: the exact value passes, anything else
 *  (including an uninitialized zero) fails. The SystemCoreClock auto-path
 *  branch is a nonzero check by construction (see the @note in onewire.h)
 *  and is exercised by tests/test/test_sysclk_auto.c instead.
 * ============================================================ */

#include "onewire.h"
#include "unity.h"

void test_clock_check_exact_match_ok(void) {
    TEST_ASSERT_EQUAL_UINT8(1u, onewire_clock_ok((uint32_t)(OW_SYSCLK_MHZ)));
}

void test_clock_check_off_by_one_rejected(void) {
    TEST_ASSERT_EQUAL_UINT8(0u, onewire_clock_ok((uint32_t)(OW_SYSCLK_MHZ) + 1u));
}

void test_clock_check_zero_rejected(void) {
    TEST_ASSERT_EQUAL_UINT8(0u, onewire_clock_ok(0u));
}

void run_test_clock_check(void) {
    TEST_RUN(test_clock_check_exact_match_ok);
    TEST_RUN(test_clock_check_off_by_one_rejected);
    TEST_RUN(test_clock_check_zero_rejected);
}
