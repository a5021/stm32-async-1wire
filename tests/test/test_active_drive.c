/* ============================================================
 *  test_active_drive.c - Active-drive (push-pull write) tests
 *
 *  Build:  make test-active        (F1 mock)
 *          make test-active-f0     (F0 mock)
 *          make test-active-g0     (G0 mock)
 *  Compiled ONLY with -DOW_DRIVE_ACTIVE=1; verifies the bus pin is
 *  switched to push-pull during pure-write transactions and reverted
 *  to open-drain for every read/reset/slave-response phase.
 * ============================================================ */

#include "ds18b20.h"
#include "ds18b20_test_access.h"
#include "hw_model.h"
#include "mock_target.h"
#include "onewire.h"
#include "ow_port.h"
#include "unity.h"

/* The driver toggles the GPIO output-stage topology; which register field
 * carries that is family-specific, so ask through mock_target.h. */
#define PIN_IS_PP() MOCK_PIN_IS_PP()
#define PIN_IS_OD() MOCK_PIN_IS_OD()

/* A trivial 8-slot command buffer of '1' bits (value irrelevant for the
 * pin-mode assertion; only the fact that it is a write transaction matters). */
static void send_8x_one(void) {
    ow_pulse_t cmd[9];
    for (uint8_t i = 0; i < 8; i++) {
        cmd[i] = ONEWIRE_ONE_PULSE;
    }
    cmd[8] = 0; /* trailing release slot */
    test_bus_send_command_n(cmd, 8);
}

static void active_drive_init_baseline_is_open_drain(void) {
    ds18b20_init();
    TEST_ASSERT_TRUE(PIN_IS_OD());
}

static void active_drive_write_slots_engage_push_pull(void) {
    ds18b20_init();
    send_8x_one();
    TEST_ASSERT_TRUE(PIN_IS_PP());
}

static void active_drive_write_bit_engage_push_pull(void) {
    ds18b20_init();
    test_bus_write_bit(1);
    TEST_ASSERT_TRUE(PIN_IS_PP());
}

static void active_drive_read_restores_open_drain(void) {
    ds18b20_init();
    test_bus_write_bit(1);
    TEST_ASSERT_TRUE(PIN_IS_PP());
    test_bus_read_data();
    TEST_ASSERT_TRUE(PIN_IS_OD());
}

static void active_drive_reset_restores_open_drain(void) {
    ds18b20_init();
    test_bus_write_bit(1);
    TEST_ASSERT_TRUE(PIN_IS_PP());
    test_bus_reset();
    TEST_ASSERT_TRUE(PIN_IS_OD());
}

static void active_drive_write_then_read_stays_open_drain(void) {
    ds18b20_init();
    /* The merged write+read op must stay open-drain so the read half is safe. */
    test_bus_write_then_read(1);
    TEST_ASSERT_TRUE(PIN_IS_OD());
}

static void active_drive_strong_pullup_toggles_mode(void) {
    ds18b20_init();
    onewire_strong_pullup(1);
    TEST_ASSERT_TRUE(PIN_IS_PP());
    onewire_strong_pullup(0);
    TEST_ASSERT_TRUE(PIN_IS_OD());
}

static void active_drive_write_then_read_sequence_leaves_open_drain(void) {
    ds18b20_init();
    /* write command (PP), then a read phase (OD): by construction there is no
     * window where the master drives HIGH while a slave could pull LOW. */
    send_8x_one();
    TEST_ASSERT_TRUE(PIN_IS_PP());
    test_bus_read_data();
    TEST_ASSERT_TRUE(PIN_IS_OD());
}

void run_test_active_drive(void) {
    TEST_RUN(active_drive_init_baseline_is_open_drain);
    TEST_RUN(active_drive_write_slots_engage_push_pull);
    TEST_RUN(active_drive_write_bit_engage_push_pull);
    TEST_RUN(active_drive_read_restores_open_drain);
    TEST_RUN(active_drive_reset_restores_open_drain);
    TEST_RUN(active_drive_write_then_read_stays_open_drain);
    TEST_RUN(active_drive_strong_pullup_toggles_mode);
    TEST_RUN(active_drive_write_then_read_sequence_leaves_open_drain);
}
