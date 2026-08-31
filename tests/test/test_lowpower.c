/* ============================================================
 *  test_lowpower.c - Opt-in low-power WFE path tests
 *
 *  Verifies the OW_PORT_LOW_POWER behaviour that the default
 *  busy-poll test build cannot reach (see CHANGELOG):
 *    - onewire_init() arms SEVONPEND in SCB.SCR
 *    - a "long" stage (> 1 ms) sets ow_long_pending and enables
 *      TIM1 UIE in DIER
 *    - ow_port_bus_done() clears ow_long_pending on completion.
 *    - ow_port_sleep_until_done() exits cleanly when UIF is set
 *    - ow_port_capture() long threshold boundary (14 vs 15 slots)
 *    - UIE is enabled on short ops (read_pair, write_bit,
 *      write_then_read, feed, single-slot write)
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

/*-------------------------------------------------------------
 *  ow_port_sleep_until_done() smoke: exits without hanging
 *
 *  The function loops on `while (!(T1.SR & TIM_SR(UIF))) { __WFE(); }`.
 *  Since __WFE() is a host no-op, setting UIF before the call makes
 *  the loop exit on the first iteration.  We verify:
 *    - the call returns (did not hang)
 *    - pending is cleared (NVIC_ClearPendingIRQ consumed the event)
 * -----------------------------------------------------------*/
void test_lowpower_sleep_until_done_smoke(void) {
    /* Start a long operation so the driver state is consistent. */
    test_bus_wait_conversion();
    TEST_ASSERT_EQUAL_UINT8(1, ow_port_long_wait_pending());

    /* Set UIF so the while-loop exits immediately on the host. */
    mock_tim1.SR |= TIM_SR_UIF;
    ow_port_sleep_until_done();

    /* After sleep, bus_done should still work (pending was cleared). */
    TEST_ASSERT_EQUAL_UINT8(1, test_ds18b20_bus_done());
    TEST_ASSERT_EQUAL_UINT8(0, ow_port_long_wait_pending());
}

/*-------------------------------------------------------------
 *  ow_port_capture() long threshold: 15 slots (> 1000 us)
 *
 *  Standard timing: 5 + 60 + 5 = 70 us per slot.
 *  15 * 70 = 1050 us > 1000 => ow_long_pending must be set.
 * -----------------------------------------------------------*/
void test_lowpower_capture_long_sets_pending(void) {
    ow_long_pending = 0;
    /* 15 slots: 15 * 70 = 1050 us > 1000 us threshold. */
    test_bus_arm_capture_n(15);
    TEST_ASSERT_EQUAL_UINT8(1, ow_port_long_wait_pending());
    TEST_ASSERT_TRUE(mock_tim1.DIER & TIM_DIER_UIE);
}

/*-------------------------------------------------------------
 *  ow_port_capture() boundary: 14 slots (<= 1000 us)
 *
 *  14 * 70 = 980 us <= 1000 => ow_long_pending must NOT be set.
 *  (UIE is still enabled — that is always-on in the low-power path.)
 * -----------------------------------------------------------*/
void test_lowpower_capture_short_keeps_pending_clear(void) {
    ow_long_pending = 0;
    /* 14 slots: 14 * 70 = 980 us <= 1000 us threshold. */
    test_bus_arm_capture_n(14);
    TEST_ASSERT_EQUAL_UINT8(0, ow_port_long_wait_pending());
    TEST_ASSERT_TRUE(mock_tim1.DIER & TIM_DIER_UIE);
}

/*-------------------------------------------------------------
 *  UIE enabled after read_pair (2-slot capture)
 * -----------------------------------------------------------*/
void test_lowpower_read_pair_enables_uie(void) {
    mock_tim1.DIER = 0;
    test_bus_read_pair();
    TEST_ASSERT_TRUE(mock_tim1.DIER & TIM_DIER_UIE);
    TEST_ASSERT_EQUAL_UINT8(0, ow_port_long_wait_pending());
}

/*-------------------------------------------------------------
 *  UIE enabled after write_then_read (3-slot merged op)
 * -----------------------------------------------------------*/
void test_lowpower_write_then_read_enables_uie(void) {
    mock_tim1.DIER = 0;
    test_bus_write_then_read(1);
    TEST_ASSERT_TRUE(mock_tim1.DIER & TIM_DIER_UIE);
    TEST_ASSERT_EQUAL_UINT8(0, ow_port_long_wait_pending());
}

/*-------------------------------------------------------------
 *  Feed (multi-slot write) enables UIE but NOT ow_long_pending
 *
 *  ow_port_feed() always sets CC2DE + UIE in DIER but never
 *  sets ow_long_pending — feed operations are short by design.
 * -----------------------------------------------------------*/
void test_lowpower_feed_sets_uie_without_pending(void) {
    /* A 16-slot command (e.g. Match ROM) exercises ow_port_feed(). */
    static const uint8_t cmd[17] = {
        0x55, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 0};
    ow_long_pending = 0;
    mock_tim1.DIER = 0;
    test_bus_send_command_n(cmd, 16);
    TEST_ASSERT_TRUE(mock_tim1.DIER & TIM_DIER_UIE);
    TEST_ASSERT_TRUE(mock_tim1.DIER & TIM_DIER_CC2DE);
    TEST_ASSERT_EQUAL_UINT8(0, ow_port_long_wait_pending());
}

/*-------------------------------------------------------------
 *  Single-slot write enables UIE (no DMA, UIE-only DIER)
 * -----------------------------------------------------------*/
void test_lowpower_single_slot_write_enables_uie(void) {
    mock_tim1.DIER = 0;
    test_bus_write_bit(1);
    TEST_ASSERT_TRUE(mock_tim1.DIER & TIM_DIER_UIE);
    TEST_ASSERT_EQUAL_UINT8(0, ow_port_long_wait_pending());
}

void run_test_lowpower(void) {
    TEST_RUN(test_lowpower_init_sets_sevonpend);
    TEST_RUN(test_lowpower_conversion_wait_sets_pending_and_uie);
    TEST_RUN(test_lowpower_cycle_pause_sets_pending_and_uie);
    TEST_RUN(test_lowpower_short_op_keeps_pending_clear);
    TEST_RUN(test_lowpower_bus_done_clears_pending);
    TEST_RUN(test_lowpower_sleep_until_done_smoke);
    TEST_RUN(test_lowpower_capture_long_sets_pending);
    TEST_RUN(test_lowpower_capture_short_keeps_pending_clear);
    TEST_RUN(test_lowpower_read_pair_enables_uie);
    TEST_RUN(test_lowpower_write_then_read_enables_uie);
    TEST_RUN(test_lowpower_feed_sets_uie_without_pending);
    TEST_RUN(test_lowpower_single_slot_write_enables_uie);
}

#endif /* OW_PORT_LOW_POWER */
