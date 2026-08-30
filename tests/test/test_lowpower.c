/* ============================================================
 *  test_lowpower.c - Opt-in low-power WFE path tests
 *
 *  Verifies the OW_PORT_LOW_POWER behaviour that the default
 *  busy-poll test build cannot reach (see CHANGELOG):
 *    - onewire_init() arms SEVONPEND in SCB.SCR
 *    - a "long" stage (> 1 ms) sets ow_long_pending and enables
 *      TIM1 UIE in DIER
 *    - ow_port_bus_done() clears ow_long_pending on completion.
 *
 *  Compiled ONLY with -DOW_PORT_LOW_POWER (make test-lowpower);
 *  without the flag this TU is empty so `make test` stays
 *  a pure busy-poll build. The real __WFE()/__SEV() instructions
 *  are host stubs; we assert the observable driver state, not the
 *  blocking sleep itself.
 * ============================================================ */

#include "ds18b20_test_access.h"
#include "mock_target.h"
#include "onewire.h"
#include "ow_port.h"
#include "unity.h"

#ifdef OW_PORT_LOW_POWER

/*-------------------------------------------------------------
 *  init arms SEVONPEND in SCB.SCR
 * -----------------------------------------------------------*/
void test_lowpower_init_sets_sevonpend(void) {
    TEST_ASSERT_EQUAL_UINT32(0, mock_scb.SCR);
    ds18b20_init(); /* -> onewire_init() -> SCB->SCR |= SEVONPEND */
    TEST_ASSERT_TRUE(mock_scb.SCR & SCB_SCR_SEVONPEND_Msk);
}

/*-------------------------------------------------------------
 *  Conversion wait is a long stage: pending set + UIE enabled
 * -----------------------------------------------------------*/
void test_lowpower_conversion_wait_sets_pending_and_uie(void) {
    test_bus_wait_conversion();
    TEST_ASSERT_EQUAL_UINT8(1, ow_port_long_wait_pending());
    TEST_ASSERT_TRUE(mock_tim1.DIER & TIM_DIER_UIE);
}

/*-------------------------------------------------------------
 *  Inter-measurement pause (5 s) is a long stage too
 * -----------------------------------------------------------*/
void test_lowpower_cycle_pause_sets_pending_and_uie(void) {
    test_bus_start_cycle_pause();
    TEST_ASSERT_EQUAL_UINT8(1, ow_port_long_wait_pending());
    TEST_ASSERT_TRUE(mock_tim1.DIER & TIM_DIER_UIE);
}

/*-------------------------------------------------------------
 *  Short stage must NOT set the long-pending flag
 * -----------------------------------------------------------*/
void test_lowpower_short_op_keeps_pending_clear(void) {
    /* A reset/presence operation is < 1 ms and must not arm the sleep flag.
     * Clear the flag first so the test is independent of test ordering. */
    ow_long_pending = 0;
    test_bus_reset();
    TEST_ASSERT_EQUAL_UINT8(0, ow_port_long_wait_pending());
}

/*-------------------------------------------------------------
 *  Completion clears the long-pending flag
 * -----------------------------------------------------------*/
void test_lowpower_bus_done_clears_pending(void) {
    test_bus_wait_conversion();
    TEST_ASSERT_EQUAL_UINT8(1, ow_port_long_wait_pending());

    /* Simulate the timer update event (operation complete). */
    mock_tim1.SR |= TIM_SR_UIF;
    TEST_ASSERT_EQUAL_UINT8(1, test_ds18b20_bus_done());
    TEST_ASSERT_EQUAL_UINT8(0, ow_port_long_wait_pending());
}

void run_test_lowpower(void) {
    TEST_RUN(test_lowpower_init_sets_sevonpend);
    TEST_RUN(test_lowpower_conversion_wait_sets_pending_and_uie);
    TEST_RUN(test_lowpower_cycle_pause_sets_pending_and_uie);
    TEST_RUN(test_lowpower_short_op_keeps_pending_clear);
    TEST_RUN(test_lowpower_bus_done_clears_pending);
}

#endif /* OW_PORT_LOW_POWER */
