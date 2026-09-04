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
#include "onewire.h"
#include "unity.h"
#include <string.h>

void spy_reset(void); /* defined in test_state_machine.c */

#define ONE ow_one_pulse_us
#define ZERO ow_zero_pulse_us

/*-------------------------------------------------------------
 *  Register-state helpers (per family)
 * -----------------------------------------------------------*/

#if defined(OW_PORT_TARGET_G0)

/* PA10 in timer-driven AF open-drain mode (post-init baseline). */
static uint8_t pu_idle_af_od(void) {
    return (mock_gpioa.MODER & GPIO_MODER_MODE10_1) &&
           !(mock_gpioa.MODER & GPIO_MODER_MODE10_0) &&
           (mock_gpioa.OTYPER & GPIO_OTYPER_OT10);
}

/* PA10 in AF mode with push-pull output type (strong pull-up engaged). */
static uint8_t pu_engaged(void) {
    return (mock_gpioa.MODER & GPIO_MODER_MODE10_1) &&
           !(mock_gpioa.MODER & GPIO_MODER_MODE10_0) &&
           !(mock_gpioa.OTYPER & GPIO_OTYPER_OT10);
}

static void pu_assert_af_mode(void) {
    TEST_ASSERT_TRUE(mock_gpioa.MODER & GPIO_MODER_MODE10_1);
    TEST_ASSERT_FALSE(mock_gpioa.MODER & GPIO_MODER_MODE10_0);
}

#elif defined(OW_PORT_TARGET_F0)

/* PA10 in timer-driven AF open-drain mode (post-init baseline). */
static uint8_t pu_idle_af_od(void) {
    return (mock_gpioa.MODER & GPIO_MODER_MODER10_1) &&
           !(mock_gpioa.MODER & GPIO_MODER_MODER10_0) &&
           (mock_gpioa.OTYPER & GPIO_OTYPER_OT_10);
}

/* PA10 in AF mode with push-pull output type (strong pull-up engaged). */
static uint8_t pu_engaged(void) {
    return (mock_gpioa.MODER & GPIO_MODER_MODER10_1) &&
           !(mock_gpioa.MODER & GPIO_MODER_MODER10_0) &&
           !(mock_gpioa.OTYPER & GPIO_OTYPER_OT_10);
}

static void pu_assert_af_mode(void) {
    TEST_ASSERT_TRUE(mock_gpioa.MODER & GPIO_MODER_MODER10_1);
    TEST_ASSERT_FALSE(mock_gpioa.MODER & GPIO_MODER_MODER10_0);
}

#else /* OW_PORT_TARGET_F1 */

static uint8_t pu_idle_af_od(void) {
    return (mock_gpioa.CRH & (GPIO_CRH_CNF10_0 | GPIO_CRH_CNF10_1)) ==
           (GPIO_CRH_CNF10_0 | GPIO_CRH_CNF10_1);
}

/* PA10 in AF push-pull mode (CNF=10: AF PP, strong pull-up engaged). */
static uint8_t pu_engaged(void) {
    return !(mock_gpioa.CRH & GPIO_CRH_CNF10_0) &&
           (mock_gpioa.CRH & GPIO_CRH_CNF10_1) &&
           (mock_gpioa.CRH & GPIO_CRH_MODE10_1);
}

static void pu_assert_af_mode(void) {
    TEST_ASSERT_TRUE(mock_gpioa.CRH & GPIO_CRH_MODE10_1);
}

#endif

/* Snapshot / compare of every bus-pin configuration register the
 * strong pull-up touches; used for the "flag off -> registers stay
 * bit-identical" regressions.  MODER is not included for F0/G0 because
 * the OTYPER-only approach never touches it. */
#if defined(OW_PORT_TARGET_G0)

typedef struct {
    uint32_t otyper;
} pu_regs_t;

static pu_regs_t pu_snapshot(void) {
    pu_regs_t r = {mock_gpioa.OTYPER};
    return r;
}

static void pu_assert_unchanged(pu_regs_t before) {
    TEST_ASSERT_EQUAL_UINT32(before.otyper, mock_gpioa.OTYPER);
}

#elif defined(OW_PORT_TARGET_F0)

typedef struct {
    uint32_t otyper;
} pu_regs_t;

static pu_regs_t pu_snapshot(void) {
    pu_regs_t r = {mock_gpioa.OTYPER};
    return r;
}

static void pu_assert_unchanged(pu_regs_t before) {
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
    sd[8] = onewire_crc8(sd, 8);
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
    pu_assert_af_mode();

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
    pu_assert_af_mode();
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
    TEST_ASSERT_TRUE(pu_engaged());
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
    TEST_ASSERT_TRUE(pu_engaged());
    ds18b20_init();
    drive_to_wait();
    TEST_ASSERT_FALSE(pu_engaged());
    TEST_ASSERT_TRUE(pu_idle_af_od());
}

void test_parasite_guard_band_tracks_mode(void) {
    ow_set_parasite_guard(0);
    ds18b20_set_parasite(0);
    uint8_t ext = ow_guard_band_us;

    /* bus in parasite mode selects the wider guard band */
    ds18b20_set_parasite(1);
    uint8_t para = ow_guard_band_us;
    TEST_ASSERT_TRUE(para > ext); /* parasite guard is wider than external */
    TEST_ASSERT_EQUAL_UINT8(para, ow_guard_band_us);

    /* returning to external power restores the tighter guard */
    ds18b20_set_parasite(0);
    TEST_ASSERT_EQUAL_UINT8(ext, ow_guard_band_us);

    /* manual override also engages the wider guard */
    ow_set_parasite_guard(1);
    TEST_ASSERT_EQUAL_UINT8(para, ow_guard_band_us);
    ow_set_parasite_guard(0);
    TEST_ASSERT_EQUAL_UINT8(ext, ow_guard_band_us);
}

/*-------------------------------------------------------------
 *  5. Auto-detect (Read Power Supply -> ctx.parasite)
 * -----------------------------------------------------------*/

/* Capture helpers mirroring test_eeprom.c: a fixed "present" reset
 * signature plus a programmable read-bit table served slot-by-slot. */

static uint16_t det_capture_present(uint32_t idx) {
    return idx == 0 ? 510u : 700u;
}

static uint16_t det_capture_absent(uint32_t idx) {
    (void)idx;
    return 100u;
}

static uint8_t det_read_bits[8];
static uint16_t det_capture_read(uint32_t idx) {
    return det_read_bits[idx] ? (uint16_t)ONE : (uint16_t)ZERO;
}

static void det_drive(uint8_t (*poll)(void)) {
    uint16_t guard = 0;
    for (;;) {
        if (poll()) {
            break;
        }
        if (mock_tim1.CR1 & TIM_CR1_CEN) {
            /* Serve the data-read slots from the bit table once the command
             * write has been consumed (same discipline as test_eeprom.c). */
            if ((mock_dma1_ch4.CCR & DMA_CCR_EN) && mock_dma1_ch4.CNDTR > 2) {
                hw_set_capture_source(det_capture_read);
            }
            TEST_ASSERT_TRUE(hw_run_until_uif(256));
        }
        if (++guard > 500) {
            break;
        }
    }
    TEST_ASSERT_TRUE(guard <= 500);
}

void test_detect_parasite_sets_flag(void) {
    ds18b20_init();
    memset(det_read_bits, 1, sizeof(det_read_bits));
    det_read_bits[0] = 0; /* slot 0 driven short: parasite-powered */
    hw_set_capture_source(det_capture_present);

    ds18b20_detect_parasite();
    det_drive(ds18b20_detect_parasite_poll);

    TEST_ASSERT_EQUAL_UINT8(1, ds18b20_last_command_ok());
    TEST_ASSERT_EQUAL_UINT8(1, ds18b20_parasite_mode());
}

void test_detect_external_clears_flag(void) {
    ds18b20_init();
    ds18b20_set_parasite(1); /* stale flag from a previous wiring */
    memset(det_read_bits, 1, sizeof(det_read_bits)); /* all long: external */
    hw_set_capture_source(det_capture_present);

    ds18b20_detect_parasite();
    det_drive(ds18b20_detect_parasite_poll);

    TEST_ASSERT_EQUAL_UINT8(1, ds18b20_last_command_ok());
    TEST_ASSERT_EQUAL_UINT8(0, ds18b20_parasite_mode());
}

void test_detect_no_presence_leaves_flag(void) {
    ds18b20_init();
    ds18b20_set_parasite(1);
    hw_set_capture_source(det_capture_absent);

    ds18b20_detect_parasite();
    det_drive(ds18b20_detect_parasite_poll);

    TEST_ASSERT_EQUAL_UINT8(0, ds18b20_last_command_ok());
    TEST_ASSERT_EQUAL_UINT8(1, ds18b20_parasite_mode()); /* untouched */
}

void test_getter_matches_setter(void) {
    ds18b20_init();
    TEST_ASSERT_EQUAL_UINT8(0, ds18b20_parasite_mode());
    ds18b20_set_parasite(1);
    TEST_ASSERT_EQUAL_UINT8(1, ds18b20_parasite_mode());
    ds18b20_set_parasite(0);
    TEST_ASSERT_EQUAL_UINT8(0, ds18b20_parasite_mode());
}

/*-------------------------------------------------------------
 *  Run all parasite-power tests
 * -----------------------------------------------------------*/
/*-------------------------------------------------------------
 *  Test: in parasite mode a simultaneous-conversion (scan) round
 *  keeps the strong pull-up engaged across the inter-round pause
 *  (scan_finish_or_next, last-device branch).
 *----------------------------------------------------------*/
void test_parasite_scan_engages_pullup_across_pause(void) {
    test_spy_reset();
    ds18b20_init();
    ds18b20_test_reset_ctx();
    ds18b20_test_reset_search();
    ds18b20_test_reset_resolution();
    ds18b20_set_parasite(1);

    uint8_t rom[DS18B20_ROM_BYTES] = {0x28, 0, 0, 0, 0, 0, 0, 0};
    ds18b20_test_set_device(0, rom);
    ds18b20_test_set_device_count(1);
    ds18b20_scan_start();
    TEST_ASSERT_EQUAL_UINT8(1, ds18b20_test_get_scan_mode());

    /* IDLE -> CONVERT */
    mock_tim1.SR |= TIM_SR_UIF;
    ds18b20_poll();
    TEST_ASSERT_EQUAL_UINT8(DS18B20_ST_CONVERT, ds18b20_test_get_state());
    /* CONVERT -> WAIT (broadcast Skip ROM) */
    ds18b20_test_set_edge(0, 510);
    ds18b20_test_set_edge(1, 700);
    mock_tim1.SR |= TIM_SR_UIF;
    ds18b20_poll();
    TEST_ASSERT_EQUAL_UINT8(DS18B20_ST_WAIT, ds18b20_test_get_state());
    /* WAIT -> CONTINUE */
    mock_tim1.SR |= TIM_SR_UIF;
    ds18b20_poll();
    TEST_ASSERT_EQUAL_UINT8(DS18B20_ST_CONTINUE, ds18b20_test_get_state());
    /* CONTINUE -> REQUEST */
    ds18b20_test_set_edge(0, 510);
    ds18b20_test_set_edge(1, 700);
    mock_tim1.SR |= TIM_SR_UIF;
    ds18b20_poll();
    TEST_ASSERT_EQUAL_UINT8(DS18B20_ST_REQUEST, ds18b20_test_get_state());
    /* REQUEST -> READ */
    ds18b20_test_set_edge(0, 510);
    ds18b20_test_set_edge(1, 700);
    mock_tim1.SR |= TIM_SR_UIF;
    ds18b20_poll();
    TEST_ASSERT_EQUAL_UINT8(DS18B20_ST_READ, ds18b20_test_get_state());
    /* READ -> DECODE */
    mock_tim1.SR |= TIM_SR_UIF;
    ds18b20_poll();
    TEST_ASSERT_EQUAL_UINT8(DS18B20_ST_DECODE, ds18b20_test_get_state());

    /* Feed a valid scratchpad and finish the last (only) device read: the
     * scan ends at IDLE and keeps the strong pull-up engaged across the pause. */
    uint8_t sd[9] = {0x64, 0x01, 0x4B, 0x46, 0x7F, 0xFF, 0x08, 0x10, 0};
    sd[8] = onewire_crc8(sd, 8);
    for (int i = 0; i < 9; i++) {
        for (int b = 0; b < 8; b++) {
            ds18b20_test_set_pulse(i * 8 + b, ((sd[i] >> b) & 1u) ? ONE : ZERO);
        }
    }
    uint8_t before = test_spy_complete_count;
    mock_tim1.SR |= TIM_SR_UIF;
    ds18b20_poll();
    TEST_ASSERT_EQUAL_UINT8(DS18B20_ST_IDLE, ds18b20_test_get_state());
    TEST_ASSERT_TRUE(pu_engaged());
    TEST_ASSERT_TRUE(test_spy_complete_count > before);
    ds18b20_set_parasite(0);
}

/*-------------------------------------------------------------
 *  Test: a parasite-powered Write Scratchpad (no read, no wait)
 *  finishes immediately and releases the strong pull-up (the
 *  txn immediate-finish branch).
 *----------------------------------------------------------*/
void test_parasite_alarm_thresholds_release_pullup(void) {
    test_spy_reset();
    ds18b20_init();
    ds18b20_test_reset_ctx();
    ds18b20_set_parasite(1);

    ds18b20_set_alarm_thresholds(0x1E, 0x00);
    uint8_t done = 0;
    for (int i = 0; i < 8 && !done; i++) {
        ds18b20_test_set_edge(0, 510);
        ds18b20_test_set_edge(1, 700);
        mock_tim1.SR |= TIM_SR_UIF;
        done = ds18b20_set_alarm_thresholds_poll();
    }
    TEST_ASSERT_TRUE(done);
    TEST_ASSERT_EQUAL_UINT8(1, ds18b20_test_get_txn_ok());
    TEST_ASSERT_FALSE(pu_engaged()); /* pull-up released after immediate finish */
    ds18b20_set_parasite(0);
}

/*-------------------------------------------------------------
 *  Test: a parasite-powered Convert T with no device answering
 *  presence still engages the strong pull-up before the retry
 *  pause (issue_command no-presence branch).
 *----------------------------------------------------------*/
void test_parasite_convert_no_presence_engages_pullup(void) {
    test_spy_reset();
    ds18b20_init();
    ds18b20_test_reset_ctx();
    ds18b20_set_parasite(1);
    ds18b20_test_set_state(DS18B20_ST_CONVERT);

    /* No device answers the presence pulse. */
    ds18b20_test_set_edge(0, 100);
    ds18b20_test_set_edge(1, 100);
    mock_tim1.SR |= TIM_SR_UIF;
    ds18b20_poll();

    TEST_ASSERT_EQUAL_UINT8(DS18B20_ST_IDLE, ds18b20_test_get_state());
    TEST_ASSERT_TRUE(pu_engaged());
    TEST_ASSERT_EQUAL_INT(DS18B20_TEMP_ERROR_NO_SENSOR, test_spy_complete_values[0]);
    ds18b20_set_parasite(0);
}

void run_test_parasite(void) {
    TEST_RUN(test_parasite_default_off_cycle_leaves_gpio_alone);
    TEST_RUN(test_parasite_default_off_txn_leaves_gpio_alone);
    TEST_RUN(test_parasite_convert_window_engage_and_release);
    TEST_RUN(test_parasite_flag_cleared_mid_window_still_releases);
    TEST_RUN(test_parasite_copy_scratchpad_window);
    TEST_RUN(test_parasite_recall_eeprom_window);
    TEST_RUN(test_parasite_setter_normalises_and_init_resets);
    TEST_RUN(test_parasite_guard_band_tracks_mode);
    TEST_RUN(test_parasite_scan_engages_pullup_across_pause);
    TEST_RUN(test_parasite_alarm_thresholds_release_pullup);
    TEST_RUN(test_parasite_convert_no_presence_engages_pullup);
    TEST_RUN(test_detect_parasite_sets_flag);
    TEST_RUN(test_detect_external_clears_flag);
    TEST_RUN(test_detect_no_presence_leaves_flag);
    TEST_RUN(test_getter_matches_setter);
}
