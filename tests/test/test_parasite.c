/* ============================================================
 *  test_parasite.c - Parasite-Power Strong Pull-Up Tests
 *
 *  Covers ds18b20_set_parasite() and the strong pull-up windows:
 *   - measurement cycle: pull-up engaged on entering the
 *     conversion wait (ST_WAIT), released before the post-
 *     conversion reset (top of ST_CONTINUE)
 *   - command transactions: engaged for the t_COPY / t_RECALL
 *     hold-off (TXN_WRITE -> TXN_WAIT branch), released at
 *     TXN_WAIT entry
 *   - default flag off: GPIO registers never modified
 *   - mid-window flag clear still releases (unconditional release)
 *
 *  The mocks expose a real register struct (mock_gpioa), so the
 *  assertions are backend register checks, guarded per family
 *  exactly like test_state_machine.c.
 * ============================================================ */

#include "ds18b20.h"
#include "ds18b20_test_access.h"
#include "ds18b20_test_spy.h"
#include "hw_model.h"
#include "mock_target.h"
#include "unity.h"
#include <string.h>

#define ONE 5u
#define ZERO 60u

/*-------------------------------------------------------------
 *  Register-state helpers (per family)
 * -----------------------------------------------------------*/

#if defined(OW_PORT_TARGET_F0)

/* PA10 in timer-driven AF open-drain mode (post-init baseline). */
static uint8_t pu_idle_af_od(void) {
    return (mock_gpioa.MODER & GPIO_MODER_MODER10_1) &&
           !(mock_gpioa.MODER & GPIO_MODER_MODER10_0) &&
           (mock_gpioa.OTYPER & GPIO_OTYPER_OT_10);
}

/* PA10 as generic push-pull output driven HIGH. */
static uint8_t pu_engaged(void) {
    return (mock_gpioa.MODER & GPIO_MODER_MODER10_0) &&
           !(mock_gpioa.MODER & GPIO_MODER_MODER10_1) &&
           !(mock_gpioa.OTYPER & GPIO_OTYPER_OT_10) &&
           (mock_gpioa.BSRR & GPIO_BSRR_BS_10);
}

#else /* OW_PORT_TARGET_F1 */

static uint8_t pu_idle_af_od(void) {
    return (mock_gpioa.CRH & (GPIO_CRH_CNF10_0 | GPIO_CRH_CNF10_1)) ==
           (GPIO_CRH_CNF10_0 | GPIO_CRH_CNF10_1);
}

static uint8_t pu_engaged(void) {
    return !(mock_gpioa.CRH & (GPIO_CRH_CNF10_0 | GPIO_CRH_CNF10_1)) &&
           (mock_gpioa.CRH & GPIO_CRH_MODE10_1) &&
           (mock_gpioa.BSRR & GPIO_BSRR_BS10);
}

#endif

/* Snapshot / compare of every bus-pin configuration register the
 * strong pull-up touches; used for the "flag off -> registers stay
 * bit-identical" regressions. */
#if defined(OW_PORT_TARGET_F0)

typedef struct {
    uint32_t moder;
    uint32_t otyper;
} pu_regs_t;

static pu_regs_t pu_snapshot(void) {
    pu_regs_t r = {mock_gpioa.MODER, mock_gpioa.OTYPER};
    return r;
}

static void pu_assert_unchanged(pu_regs_t before) {
    TEST_ASSERT_EQUAL_UINT32(before.moder, mock_gpioa.MODER);
    TEST_ASSERT_EQUAL_UINT32(before.otyper, mock_gpioa.OTYPER);
}

#else /* OW_PORT_TARGET_F1 */

typedef struct {
    uint32_t crh;
} pu_regs_t;

static pu_regs_t pu_snapshot(void) {
    pu_regs_t r = {mock_gpioa.CRH};
    return r;
}

static void pu_assert_unchanged(pu_regs_t before) {
    TEST_ASSERT_EQUAL_UINT32(before.crh, mock_gpioa.CRH);
}

#endif

/*-------------------------------------------------------------
 *  Measurement-cycle driver (Skip ROM, valid scratchpad)
 *  Steps poll-by-poll so the caller can inspect registers
 *  inside the conversion-wait window.
 * -----------------------------------------------------------*/

static void inject_presence(void) {
    ds18b20_test_set_edge(0, 510);
    ds18b20_test_set_edge(1, 700);
}

static void inject_scratchpad(void) {
    uint8_t sd[9] = {0x64, 0x01, 0x4B, 0x46, 0x7F, 0xFF, 0x08, 0x10, 0};
    sd[8] = ds18b20_crc8(sd, 8);
    for (uint8_t i = 0; i < 9; i++) {
        for (uint8_t b = 0; b < 8; b++) {
            ds18b20_test_set_pulse(i * 8 + b, ((sd[i] >> b) & 1u) ? ONE : ZERO);
        }
    }
}

/* IDLE -> START+CONVERT -> WAIT; returns with state == CONTINUE (4),
 * i.e. right after the strong pull-up window was entered. */
static void drive_to_wait(void) {
    mock_tim1.SR |= TIM_SR_UIF;
    ds18b20_poll();
    TEST_ASSERT_EQUAL_UINT8(2, ds18b20_test_get_state());

    inject_presence();
    mock_tim1.SR |= TIM_SR_UIF;
    ds18b20_poll();
    TEST_ASSERT_EQUAL_UINT8(3, ds18b20_test_get_state());

    mock_tim1.SR |= TIM_SR_UIF;
    ds18b20_poll();
    TEST_ASSERT_EQUAL_UINT8(4, ds18b20_test_get_state());
}

/* REQUEST -> READ -> DECODE -> IDLE.
 * Entry state: REQUEST (5), i.e. the post-conversion reset already ran. */
static void drive_request_to_end(void) {
    inject_presence();
    mock_tim1.SR |= TIM_SR_UIF;
    ds18b20_poll();
    TEST_ASSERT_EQUAL_UINT8(6, ds18b20_test_get_state());

    inject_scratchpad();
    mock_tim1.SR |= TIM_SR_UIF;
    ds18b20_poll();
    TEST_ASSERT_EQUAL_UINT8(7, ds18b20_test_get_state());

    mock_tim1.SR |= TIM_SR_UIF;
    ds18b20_poll();
    TEST_ASSERT_EQUAL_UINT8(0, ds18b20_test_get_state());
}

/* CONTINUE -> REQUEST, then the rest of the cycle.
 * Entry state: CONTINUE (4), i.e. right after the conversion-wait
 * window was entered. */
static void drive_from_continue_to_end(void) {
    inject_presence();
    mock_tim1.SR |= TIM_SR_UIF;
    ds18b20_poll();
    TEST_ASSERT_EQUAL_UINT8(5, ds18b20_test_get_state());
    drive_request_to_end();
}

/*-------------------------------------------------------------
 *  Command-transaction step driver (Copy / Recall shape):
 *  RESET -> WRITE -> [wait window] -> DONE
 * -----------------------------------------------------------*/

static uint16_t pp_capture_present(uint32_t idx) {
    return idx == 0 ? 510u : 700u;
}

static void run_op(void) {
    if (mock_tim1.CR1 & TIM_CR1_CEN) {
        TEST_ASSERT_TRUE(hw_run_until_uif(256));
    }
}

/* Drive a Copy/Recall transaction up to the hold-off window:
 * each txn_poll() call processes exactly one completed hardware
 * operation, with run_op() simulating the scheduled op in between.
 * Leaves the transaction inside TXN_WAIT with the pull-up engaged. */
static void txn_drive_to_wait_window(uint8_t (*poll)(void)) {
    hw_set_capture_source(pp_capture_present);
    run_op(); /* reset completes */
    TEST_ASSERT_FALSE(poll()); /* presence ok -> command write scheduled */
    run_op(); /* write completes */
    TEST_ASSERT_FALSE(poll()); /* wait branch: engage pull-up, arm hold-off */
}

/* Finish the hold-off window: returns 1 when the transaction finished.
 * The poll that processes TXN_WAIT only moves the phase to DONE and
 * returns 0; the next call performs the timer hand-back and finishes. */
static uint8_t txn_finish_wait(uint8_t (*poll)(void)) {
    run_op(); /* hold-off timer completes */
    if (!poll()) { /* TXN_WAIT entry: release pull-up, phase -> DONE */
        return poll();
    }
    return 1;
}

/*-------------------------------------------------------------
 *  1. Default (flag off): no GPIO changes anywhere
 * -----------------------------------------------------------*/

void test_parasite_default_off_cycle_leaves_gpio_alone(void) {
    ds18b20_init();

    /* Snapshot the post-init bus pin configuration */
    pu_regs_t regs = pu_snapshot();

    spy_reset();
    drive_to_wait();
    TEST_ASSERT_FALSE(pu_engaged());
    drive_from_continue_to_end();

    /* Values must be bit-identical after the whole cycle (the
     * unconditional release writes are idempotent ORs/restores). */
    pu_assert_unchanged(regs);

    /* Measurement itself is unaffected by the feature being off */
    TEST_ASSERT_TRUE(test_spy_complete_called);
    TEST_ASSERT_EQUAL_INT(223, test_spy_complete_value);
}

void test_parasite_default_off_txn_leaves_gpio_alone(void) {
    ds18b20_init();
    pu_regs_t regs = pu_snapshot();

    ds18b20_copy_scratchpad();
    hw_set_capture_source(pp_capture_present);
    uint8_t finished = 0;
    uint16_t guard = 0;
    while (!finished && guard++ < 500) {
        finished = ds18b20_copy_scratchpad_poll();
        run_op();
    }
    TEST_ASSERT_TRUE(finished);
    TEST_ASSERT_FALSE(pu_engaged());
    pu_assert_unchanged(regs);
    TEST_ASSERT_EQUAL_UINT8(1, ds18b20_test_get_txn_ok());
}

/*-------------------------------------------------------------
 *  2. Conversion window: engage at WAIT, release before reset
 * -----------------------------------------------------------*/

void test_parasite_convert_window_engage_and_release(void) {
    spy_reset();
    ds18b20_init();
    ds18b20_set_parasite(1);

    drive_to_wait();
    /* Inside the window the pin actively drives HIGH */
    TEST_ASSERT_TRUE(pu_engaged());

    /* Next poll runs ST_CONTINUE: release happens BEFORE the reset
     * pulse is scheduled (sequential code in the same case block). */
    inject_presence();
    mock_tim1.SR |= TIM_SR_UIF;
    ds18b20_poll();
    TEST_ASSERT_EQUAL_UINT8(5, ds18b20_test_get_state());
    TEST_ASSERT_TRUE(pu_idle_af_od());
    TEST_ASSERT_FALSE(pu_engaged());

    /* Cycle completes normally with the pull-up released */
    drive_request_to_end();
    TEST_ASSERT_TRUE(test_spy_complete_called);
    TEST_ASSERT_EQUAL_INT(223, test_spy_complete_value);
    TEST_ASSERT_TRUE(pu_idle_af_od());
}

void test_parasite_flag_cleared_mid_window_still_releases(void) {
    spy_reset();
    ds18b20_init();
    ds18b20_set_parasite(1);

    drive_to_wait();
    TEST_ASSERT_TRUE(pu_engaged());

    /* User disables parasite power while the wait runs: the release
     * hook is unconditional exactly so the line cannot stay actively
     * driven into the readout phase. */
    ds18b20_set_parasite(0);
    inject_presence();
    mock_tim1.SR |= TIM_SR_UIF;
    ds18b20_poll();
    TEST_ASSERT_TRUE(pu_idle_af_od());
    TEST_ASSERT_FALSE(pu_engaged());
}

/*-------------------------------------------------------------
 *  3. EEPROM transactions: pull-up across the hold-off window
 * -----------------------------------------------------------*/

void test_parasite_copy_scratchpad_window(void) {
    ds18b20_init();
    ds18b20_set_parasite(1);
    TEST_ASSERT_FALSE(pu_engaged());

    ds18b20_copy_scratchpad();
    txn_drive_to_wait_window(ds18b20_copy_scratchpad_poll);

    /* WRITE completed and wait_us != 0: pull-up must be ON now */
    TEST_ASSERT_TRUE(pu_engaged());

    uint8_t finished = txn_finish_wait(ds18b20_copy_scratchpad_poll);
    TEST_ASSERT_TRUE(finished);
    /* Released again after the hold-off */
    TEST_ASSERT_TRUE(pu_idle_af_od());
    TEST_ASSERT_FALSE(pu_engaged());
    TEST_ASSERT_EQUAL_UINT8(1, ds18b20_test_get_txn_ok());
}

void test_parasite_recall_eeprom_window(void) {
    ds18b20_init();
    ds18b20_set_parasite(1);

    ds18b20_recall_eeprom();
    txn_drive_to_wait_window(ds18b20_recall_eeprom_poll);
    TEST_ASSERT_TRUE(pu_engaged());

    uint8_t finished = txn_finish_wait(ds18b20_recall_eeprom_poll);
    TEST_ASSERT_TRUE(finished);
    TEST_ASSERT_TRUE(pu_idle_af_od());
    TEST_ASSERT_EQUAL_UINT8(1, ds18b20_test_get_txn_ok());
}

/*-------------------------------------------------------------
 *  4. Setter normalisation and init default
 * -----------------------------------------------------------*/

void test_parasite_setter_normalises_and_init_resets(void) {
    ds18b20_init();
    ds18b20_set_parasite(200); /* any non-zero value means "on" */
    drive_to_wait();
    TEST_ASSERT_TRUE(pu_engaged());

    /* Finish the cycle so the machine is back at IDLE, then re-init:
     * init() must return the parasite flag to the external-power
     * default, so the next conversion window stays open-drain. */
    drive_from_continue_to_end();
    TEST_ASSERT_TRUE(pu_idle_af_od());
    ds18b20_init();
    drive_to_wait();
    TEST_ASSERT_FALSE(pu_engaged());
    TEST_ASSERT_TRUE(pu_idle_af_od());
}

/*-------------------------------------------------------------
 *  Run all parasite-power tests
 * -----------------------------------------------------------*/
void run_test_parasite(void) {
    TEST_RUN(test_parasite_default_off_cycle_leaves_gpio_alone);
    TEST_RUN(test_parasite_default_off_txn_leaves_gpio_alone);
    TEST_RUN(test_parasite_convert_window_engage_and_release);
    TEST_RUN(test_parasite_flag_cleared_mid_window_still_releases);
    TEST_RUN(test_parasite_copy_scratchpad_window);
    TEST_RUN(test_parasite_recall_eeprom_window);
    TEST_RUN(test_parasite_setter_normalises_and_init_resets);
}
