/* ============================================================
 *  test_state_machine.c - State Machine Tests
 *
 *  Tests the ds18b20_poll() state machine transitions.
 *  Since poll() checks T1.SR UIF flag, we manipulate the
 *  mock register to simulate hardware completion.
 *
 *  States: IDLE(0) -> START(1) -> CONVERT(2) -> WAIT(3) ->
 *          CONTINUE(4) -> REQUEST(5) -> READ(6) -> DECODE(7) -> IDLE(0)
 * ============================================================ */

#include "ds18b20.h"
#include "ds18b20_test_access.h"
#include "ds18b20_test_spy.h"
#include "hw_model.h"
#include "stm32f1xx.h"
#include "unity.h"
#include <string.h>

/*-------------------------------------------------------------
 *  Callback spy for ds18b20_complete/ds18b20_busy
 *  (shared module: tests/mock/ds18b20_test_spy.c)
 * -----------------------------------------------------------*/

void spy_reset(void) { test_spy_reset(); }

/*-------------------------------------------------------------
 *  Test: Initial state after ds18b20_init()
 * -----------------------------------------------------------*/
void test_state_machine_initial_state_after_init(void) {
    /* Initialize the driver */
    ds18b20_init();

    /* State should be 0 (IDLE) and UIF should be set */
    TEST_ASSERT_EQUAL_UINT8(0, ds18b20_test_get_state());
}

/*-------------------------------------------------------------
 *  Test: First poll transitions IDLE -> START -> CONVERT
 *  (with UIF set, simulates hardware completion)
 * -----------------------------------------------------------*/
void test_state_machine_first_poll_transitions_to_START(void) {
    ds18b20_init();
    ds18b20_test_reset_ctx();

    /* Simulate UIF set (hardware operation complete) */
    mock_tim1.SR |= TIM_SR_UIF;

    /* First poll: IDLE(0) -> START(1) */
    ds18b20_poll();

    /* State should now be 2 (CONVERT) because START falls through */
    TEST_ASSERT_EQUAL_UINT8(2, ds18b20_test_get_state());
}

/*-------------------------------------------------------------
 *  Test: UIF clear means poll returns immediately
 * -----------------------------------------------------------*/
void test_state_machine_UIF_clear_no_transition(void) {
    ds18b20_init();
    ds18b20_test_reset_ctx();

    /* UIF is NOT set */
    mock_tim1.SR &= ~TIM_SR_UIF;

    /* Poll should return immediately without changing state */
    uint8_t state_before = ds18b20_test_get_state();
    ds18b20_poll();
    uint8_t state_after = ds18b20_test_get_state();

    TEST_ASSERT_EQUAL_UINT8(state_before, state_after);
}

/*-------------------------------------------------------------
 *  Test: CONVERT state with presence check pass
 * -----------------------------------------------------------*/
void test_state_machine_convert_with_presence_pass(void) {
    ds18b20_init();
    ds18b20_test_reset_ctx();

    /* Set state to CONVERT */
    ds18b20_test_set_state(2);

    /* Set presence edges to valid values */
    ds18b20_test_set_edge(0, 510); /* Reset pulse within range */
    ds18b20_test_set_edge(1, 700); /* Presence pulse within range */

    /* Simulate UIF set */
    mock_tim1.SR |= TIM_SR_UIF;

    /* Poll should send convert command and transition to WAIT */
    ds18b20_poll();

    TEST_ASSERT_EQUAL_UINT8(3, ds18b20_test_get_state());
}

/*-------------------------------------------------------------
 *  Test: CONVERT state with presence check fail
 * -----------------------------------------------------------*/
void test_state_machine_convert_with_presence_fail(void) {
    spy_reset();
    ds18b20_init();
    ds18b20_test_reset_ctx();

    /* Set state to CONVERT */
    ds18b20_test_set_state(2);

    /* Set presence edges to invalid values (device not present) */
    ds18b20_test_set_edge(0, 100); /* Too short */
    ds18b20_test_set_edge(1, 100); /* Too short */

    /* Simulate UIF set */
    mock_tim1.SR |= TIM_SR_UIF;

    /* Poll should report NO_SENSOR error and go to pause */
    ds18b20_poll();

    /* After failure, state goes back to IDLE (0) with pause timer running */
    TEST_ASSERT_EQUAL_UINT8(0, ds18b20_test_get_state());
    /* Busy indicator must be cleared even though DECODE was skipped. */
    TEST_ASSERT_EQUAL_UINT8(0, test_spy_busy_last_action);
}

/*-------------------------------------------------------------
 *  Test: WAIT state transitions to CONTINUE
 * -----------------------------------------------------------*/
void test_state_machine_wait_transitions_to_continue(void) {
    ds18b20_init();
    ds18b20_test_reset_ctx();

    /* Set state to WAIT */
    ds18b20_test_set_state(3);

    /* Simulate UIF set (750ms timer complete) */
    mock_tim1.SR |= TIM_SR_UIF;

    /* Poll should reset bus and transition to CONTINUE */
    ds18b20_poll();

    TEST_ASSERT_EQUAL_UINT8(4, ds18b20_test_get_state());
}

/*-------------------------------------------------------------
 *  Test: CONTINUE state transitions to REQUEST
 * -----------------------------------------------------------*/
void test_state_machine_continue_transitions_to_request(void) {
    ds18b20_init();
    ds18b20_test_reset_ctx();

    /* Set state to CONTINUE */
    ds18b20_test_set_state(4);

    /* Simulate UIF set (reset complete) */
    mock_tim1.SR |= TIM_SR_UIF;

    /* Poll should check presence and transition to REQUEST */
    ds18b20_poll();

    /* With default edges (0), presence check fails -> goes to IDLE */
    /* To test successful path, we need valid edges */
    ds18b20_test_set_edge(0, 510);
    ds18b20_test_set_edge(1, 700);

    /* Reset state to CONTINUE and try again */
    ds18b20_test_set_state(4);
    mock_tim1.SR |= TIM_SR_UIF;

    ds18b20_poll();

    TEST_ASSERT_EQUAL_UINT8(5, ds18b20_test_get_state());
}

/*-------------------------------------------------------------
 *  Test: REQUEST state with presence check pass
 * -----------------------------------------------------------*/
void test_state_machine_request_with_presence_pass(void) {
    ds18b20_init();
    ds18b20_test_reset_ctx();

    /* Set state to REQUEST */
    ds18b20_test_set_state(5);

    /* Set valid presence edges */
    ds18b20_test_set_edge(0, 510);
    ds18b20_test_set_edge(1, 700);

    /* Simulate UIF set */
    mock_tim1.SR |= TIM_SR_UIF;

    /* Poll should send read command and transition to READ */
    ds18b20_poll();

    TEST_ASSERT_EQUAL_UINT8(6, ds18b20_test_get_state());
}

/*-------------------------------------------------------------
 *  Test: READ state transitions to DECODE
 * -----------------------------------------------------------*/
void test_state_machine_read_transitions_to_decode(void) {
    ds18b20_init();
    ds18b20_test_reset_ctx();

    /* Set state to READ */
    ds18b20_test_set_state(6);

    /* Simulate UIF set (read complete) */
    mock_tim1.SR |= TIM_SR_UIF;

    /* Poll should transition to DECODE state */
    ds18b20_poll();

    /* State should be DECODE (7) */
    TEST_ASSERT_EQUAL_UINT8(7, ds18b20_test_get_state());
}

/*-------------------------------------------------------------
 *  Test: DECODE state with valid CRC
 * -----------------------------------------------------------*/
void test_state_machine_decode_with_valid_crc(void) {
    ds18b20_init();
    ds18b20_test_reset_ctx();

    /* Set state to DECODE */
    ds18b20_test_set_state(7);

    /* Set up scratchpad with valid CRC */
    /* Temperature: 22.25°C -> raw = 0x0164 */
    uint8_t temp_data[9] = {0x64, 0x01, 0x4B, 0x46, 0x7F, 0xFF, 0x08, 0x10, 0};
    temp_data[8] = ds18b20_crc8(temp_data, 8); /* Compute valid CRC */

    for (int i = 0; i < 9; i++) {
        ds18b20_test_set_scratchpad(i, temp_data[i]);
    }

    /* Simulate UIF set */
    mock_tim1.SR |= TIM_SR_UIF;

    /* Poll should report valid temperature */
    ds18b20_poll();

    /* State should return to IDLE */
    TEST_ASSERT_EQUAL_UINT8(0, ds18b20_test_get_state());
}

/*-------------------------------------------------------------
 *  Test: DECODE state with invalid CRC
 * -----------------------------------------------------------*/
void test_state_machine_decode_with_invalid_crc(void) {
    ds18b20_init();
    ds18b20_test_reset_ctx();

    /* Set state to DECODE */
    ds18b20_test_set_state(7);

    /* Set up scratchpad with invalid CRC */
    uint8_t temp_data[9] = {0x64, 0x01, 0x4B, 0x46, 0x7F, 0xFF, 0x08, 0x10, 0xFF};
    /* CRC byte is 0xFF (incorrect) */

    for (int i = 0; i < 9; i++) {
        ds18b20_test_set_scratchpad(i, temp_data[i]);
    }

    /* Simulate UIF set */
    mock_tim1.SR |= TIM_SR_UIF;

    /* Poll should report CRC error */
    ds18b20_poll();

    /* State should return to IDLE */
    TEST_ASSERT_EQUAL_UINT8(0, ds18b20_test_get_state());
}

/*-------------------------------------------------------------
 *  Test: Invalid state triggers generic error
 * -----------------------------------------------------------*/
void test_state_machine_invalid_state_triggers_error(void) {
    ds18b20_init();
    ds18b20_test_reset_ctx();

    /* Set an invalid state (e.g., 255) */
    ds18b20_test_set_state(255);

    /* Simulate UIF set */
    mock_tim1.SR |= TIM_SR_UIF;

    /* Poll should report generic error and reset to IDLE */
    ds18b20_poll();

    TEST_ASSERT_EQUAL_UINT8(0, ds18b20_test_get_state());
}

/*-------------------------------------------------------------
 *  Test: Full cycle - Skip ROM mode
 * -----------------------------------------------------------*/
void test_state_machine_full_cycle_skip_rom(void) {
    ds18b20_init();
    ds18b20_test_reset_ctx();

    /* Ensure Skip ROM mode (no device selected) */
    ds18b20_test_set_address_mode(0);

    /* Simulate a full measurement cycle */
    /* Step 1: IDLE -> START -> CONVERT */
    mock_tim1.SR |= TIM_SR_UIF;
    ds18b20_poll();
    TEST_ASSERT_EQUAL_UINT8(2, ds18b20_test_get_state());

    /* Step 2: CONVERT -> WAIT (with valid presence) */
    ds18b20_test_set_edge(0, 510);
    ds18b20_test_set_edge(1, 700);
    mock_tim1.SR |= TIM_SR_UIF;
    ds18b20_poll();
    TEST_ASSERT_EQUAL_UINT8(3, ds18b20_test_get_state());

    /* Step 3: WAIT -> CONTINUE */
    mock_tim1.SR |= TIM_SR_UIF;
    ds18b20_poll();
    TEST_ASSERT_EQUAL_UINT8(4, ds18b20_test_get_state());

    /* Step 4: CONTINUE -> REQUEST (with valid presence) */
    ds18b20_test_set_edge(0, 510);
    ds18b20_test_set_edge(1, 700);
    mock_tim1.SR |= TIM_SR_UIF;
    ds18b20_poll();
    TEST_ASSERT_EQUAL_UINT8(5, ds18b20_test_get_state());

    /* Step 5: REQUEST -> READ (with valid presence) */
    ds18b20_test_set_edge(0, 510);
    ds18b20_test_set_edge(1, 700);
    mock_tim1.SR |= TIM_SR_UIF;
    ds18b20_poll();
    TEST_ASSERT_EQUAL_UINT8(6, ds18b20_test_get_state());

    /* Step 6: READ -> DECODE (with valid CRC) */
    /* Set up valid scratchpad */
    uint8_t temp_data[9] = {0x64, 0x01, 0x4B, 0x46, 0x7F, 0xFF, 0x08, 0x10, 0};
    temp_data[8] = ds18b20_crc8(temp_data, 8);
    for (int i = 0; i < 9; i++) {
        ds18b20_test_set_scratchpad(i, temp_data[i]);
    }

    mock_tim1.SR |= TIM_SR_UIF;
    ds18b20_poll();

    /* READ transitions to DECODE (7) */
    TEST_ASSERT_EQUAL_UINT8(7, ds18b20_test_get_state());

    /* Step 7: DECODE -> IDLE (no UIF needed, processes immediately) */
    mock_tim1.SR |= TIM_SR_UIF;
    ds18b20_poll();

    /* DECODE processes and returns to IDLE */
    TEST_ASSERT_EQUAL_UINT8(0, ds18b20_test_get_state());
}

/*-------------------------------------------------------------
 *  Test: State machine with Match ROM (address_mode = 1)
 * -----------------------------------------------------------*/
void test_state_machine_match_rom_mode(void) {
    ds18b20_init();
    ds18b20_test_reset_ctx();

    /* Select a device */
    uint8_t rom[8] = {0x28, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07};
    ds18b20_select(rom);

    /* Verify address mode is set */
    TEST_ASSERT_EQUAL_UINT8(1, ds18b20_test_get_address_mode());

    /* Go through CONVERT state in Match ROM mode */
    ds18b20_test_set_state(2);
    ds18b20_test_set_edge(0, 510);
    ds18b20_test_set_edge(1, 700);

    mock_tim1.SR |= TIM_SR_UIF;
    ds18b20_poll();

    /* Should transition to WAIT */
    TEST_ASSERT_EQUAL_UINT8(3, ds18b20_test_get_state());
}

/*-------------------------------------------------------------
 *  Test: DECODE with all-zero scratchpad reports CRC error
 *  (BUG-3 fix: reserved bytes validation)
 * -----------------------------------------------------------*/
void test_state_machine_decode_all_zero_reports_error(void) {
    spy_reset();
    ds18b20_init();
    ds18b20_test_reset_ctx();

    /* Set state to DECODE */
    ds18b20_test_set_state(7);

    /* Set up all-zero scratchpad (bus fault condition) */
    for (int i = 0; i < 9; i++) {
        ds18b20_test_set_scratchpad(i, 0x00);
    }

    mock_tim1.SR |= TIM_SR_UIF;
    ds18b20_poll();

    /* Callback must be called with CRC error */
    TEST_ASSERT_TRUE(test_spy_complete_called);
    TEST_ASSERT_EQUAL_INT(DS18B20_TEMP_ERROR_CRC_FAIL, test_spy_complete_value);

    /* State should return to IDLE */
    TEST_ASSERT_EQUAL_UINT8(0, ds18b20_test_get_state());
}

/*-------------------------------------------------------------
 *  Test: DECODE with all-0xFF scratchpad reports CRC error
 *  (BUG-3 fix: byte 7 must be 0x10, not 0xFF)
 * -----------------------------------------------------------*/
void test_state_machine_decode_all_FF_reports_error(void) {
    spy_reset();
    ds18b20_init();
    ds18b20_test_reset_ctx();

    /* Set state to DECODE */
    ds18b20_test_set_state(7);

    /* Set up all-0xFF scratchpad (bus stuck high) */
    for (int i = 0; i < 9; i++) {
        ds18b20_test_set_scratchpad(i, 0xFF);
    }

    mock_tim1.SR |= TIM_SR_UIF;
    ds18b20_poll();

    /* Callback must be called with CRC error */
    TEST_ASSERT_TRUE(test_spy_complete_called);
    TEST_ASSERT_EQUAL_INT(DS18B20_TEMP_ERROR_CRC_FAIL, test_spy_complete_value);

    /* State should return to IDLE */
    TEST_ASSERT_EQUAL_UINT8(0, ds18b20_test_get_state());
}

/*-------------------------------------------------------------
 *  Test: DECODE with all-0xFF scratchpad in Match ROM mode
 *  reports NO_SENSOR (B2: the addressed device is absent and
 *  nothing drives the bus after the Match ROM address)
 * -----------------------------------------------------------*/
void test_state_machine_decode_all_FF_match_rom_reports_no_sensor(void) {
    spy_reset();
    ds18b20_init();
    ds18b20_test_reset_ctx();

    /* Enable Match ROM addressing (a device is selected) */
    uint8_t rom[8] = {0x28, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07};
    ds18b20_select(rom);
    TEST_ASSERT_EQUAL_UINT8(1, ds18b20_test_get_address_mode());

    /* Set state to DECODE */
    ds18b20_test_set_state(7);

    /* All-0xFF scratchpad: simulate a bus that nobody drives after the
     * Match ROM address (undriven line reads back as all '1' bits) */
    for (int i = 0; i < 9; i++) {
        for (int b = 0; b < 8; b++) {
            ds18b20_test_set_pulse(i * 8 + b, 5u);
        }
    }

    mock_tim1.SR |= TIM_SR_UIF;
    ds18b20_poll();

    /* Must be NO_SENSOR, not a bogus CRC error */
    TEST_ASSERT_TRUE(test_spy_complete_called);
    TEST_ASSERT_EQUAL_INT(DS18B20_TEMP_ERROR_NO_SENSOR, test_spy_complete_value);

    /* State should return to IDLE */
    TEST_ASSERT_EQUAL_UINT8(0, ds18b20_test_get_state());
}

/*-------------------------------------------------------------
 *  Test: DECODE with corrupted (non 0xFF) scratchpad in Match ROM
 *  mode still reports CRC error (B2 heuristic must not swallow
 *  genuine communication corruption)
 * -----------------------------------------------------------*/
void test_state_machine_decode_corrupt_match_rom_reports_crc(void) {
    spy_reset();
    ds18b20_init();
    ds18b20_test_reset_ctx();

    /* Enable Match ROM addressing (a device is selected) */
    uint8_t rom[8] = {0x28, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07};
    ds18b20_select(rom);

    /* Set state to DECODE */
    ds18b20_test_set_state(7);

    /* Scratchpad that is NOT all 0xFF but fails the reserved-byte check */
    uint8_t bad_data[9] = {0x64, 0x01, 0x4B, 0x46, 0x7F, 0x00, 0x08, 0x10, 0x10};
    for (int i = 0; i < 9; i++) {
        for (int b = 0; b < 8; b++) {
            ds18b20_test_set_pulse(i * 8 + b, (bad_data[i] >> b) & 1u ? 5u : 60u);
        }
    }

    mock_tim1.SR |= TIM_SR_UIF;
    ds18b20_poll();

    /* Must remain a CRC error */
    TEST_ASSERT_TRUE(test_spy_complete_called);
    TEST_ASSERT_EQUAL_INT(DS18B20_TEMP_ERROR_CRC_FAIL, test_spy_complete_value);

    /* State should return to IDLE */
    TEST_ASSERT_EQUAL_UINT8(0, ds18b20_test_get_state());
}
void test_state_machine_decode_wrong_byte5_reports_error(void) {
    spy_reset();
    ds18b20_init();
    ds18b20_test_reset_ctx();

    /* Set state to DECODE */
    ds18b20_test_set_state(7);

    /* Set up scratchpad with byte 5 = 0x00 (should be 0xFF) */
    uint8_t bad_data[9] = {0x64, 0x01, 0x4B, 0x46, 0x7F, 0x00, 0x08, 0x10, 0x10};
    for (int i = 0; i < 9; i++) {
        ds18b20_test_set_scratchpad(i, bad_data[i]);
    }

    mock_tim1.SR |= TIM_SR_UIF;
    ds18b20_poll();

    /* Callback must be called with CRC error */
    TEST_ASSERT_TRUE(test_spy_complete_called);
    TEST_ASSERT_EQUAL_INT(DS18B20_TEMP_ERROR_CRC_FAIL, test_spy_complete_value);

    /* State should return to IDLE */
    TEST_ASSERT_EQUAL_UINT8(0, ds18b20_test_get_state());
}

/*-------------------------------------------------------------
 *  E2E: bus search finds one device, select it (Match ROM),
 *  then run the full measurement cycle and get a temperature.
 * -----------------------------------------------------------*/
#define E2E_ONE 5u
#define E2E_ZERO 60u

static uint8_t g_e2e_rom[8];
static uint8_t g_e2e_wr_bit;

static uint8_t e2e_sink(const uint8_t* rom) {
    memcpy(g_e2e_rom, rom, 8);
    return 0;
}

static uint16_t e2e_capture_src(uint32_t idx) {
    uint8_t rcr = (uint8_t)mock_tim1.RCR;
    if (rcr == 0) {
        return idx == 0 ? 510u : 700u; /* reset + presence pulse */
    }
    if (rcr == 1) { /* first read pair: bit 1 */
        uint8_t b = (g_e2e_rom[0] >> 0) & 1u;
        return (idx == 0) ? (b ? E2E_ONE : E2E_ZERO) : (b ? E2E_ZERO : E2E_ONE);
    }
    /* merged write+read capturing bit g_e2e_wr_bit */
    uint8_t byte = (g_e2e_wr_bit - 1u) / 8u;
    uint8_t bit = (g_e2e_wr_bit - 1u) % 8u;
    uint8_t b = (g_e2e_rom[byte] >> bit) & 1u;
    if (idx == 0) return 0u;
    if (idx == 1) return b ? E2E_ONE : E2E_ZERO;
    g_e2e_wr_bit++;
    return b ? E2E_ZERO : E2E_ONE;
}

void test_state_machine_search_select_measure_e2e(void) {
    spy_reset();
    ds18b20_init();
    ds18b20_test_reset_ctx();

    /* Phase 1: bus search finds exactly one device */
    uint8_t serial[7] = {0x28, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06};
    memcpy(g_e2e_rom, serial, 7);
    g_e2e_rom[7] = ds18b20_crc8(g_e2e_rom, 7);

    g_e2e_wr_bit = 2;
    hw_set_capture_source(e2e_capture_src);
    ds18b20_search_start(e2e_sink, 1);

    uint16_t guard = 0;
    for (;;) {
        if (ds18b20_search_poll()) {
            break;
        }
        if (mock_tim1.CR1 & TIM_CR1_CEN) {
            uint8_t ok = hw_run_until_uif(100);
            TEST_ASSERT_TRUE(ok);
        }
        if (++guard > 500) {
            break;
        }
    }
    TEST_ASSERT_TRUE(guard <= 500);
    TEST_ASSERT_EQUAL_UINT8(1, ds18b20_search_count());
    TEST_ASSERT_EQUAL_UINT8(1, ds18b20_search_poll()); /* finished flag */

    /* Phase 2: select the found device -> Match ROM addressing */
    ds18b20_select(g_e2e_rom);
    TEST_ASSERT_EQUAL_UINT8(1, ds18b20_test_get_address_mode());

    /* Phase 3: full measurement cycle in Match ROM mode */
    mock_tim1.SR |= TIM_SR_UIF;
    ds18b20_poll();
    TEST_ASSERT_EQUAL_UINT8(2, ds18b20_test_get_state());

    ds18b20_test_set_edge(0, 510);
    ds18b20_test_set_edge(1, 700);
    mock_tim1.SR |= TIM_SR_UIF;
    ds18b20_poll();
    TEST_ASSERT_EQUAL_UINT8(3, ds18b20_test_get_state());

    mock_tim1.SR |= TIM_SR_UIF;
    ds18b20_poll();
    TEST_ASSERT_EQUAL_UINT8(4, ds18b20_test_get_state());

    ds18b20_test_set_edge(0, 510);
    ds18b20_test_set_edge(1, 700);
    mock_tim1.SR |= TIM_SR_UIF;
    ds18b20_poll();
    TEST_ASSERT_EQUAL_UINT8(5, ds18b20_test_get_state());

    ds18b20_test_set_edge(0, 510);
    ds18b20_test_set_edge(1, 700);
    mock_tim1.SR |= TIM_SR_UIF;
    ds18b20_poll();
    TEST_ASSERT_EQUAL_UINT8(6, ds18b20_test_get_state());

    uint8_t temp_data[9] = {0x64, 0x01, 0x4B, 0x46, 0x7F, 0xFF, 0x08, 0x10, 0};
    temp_data[8] = ds18b20_crc8(temp_data, 8);
    for (int i = 0; i < 9; i++) {
        for (int b = 0; b < 8; b++) {
            ds18b20_test_set_pulse(i * 8 + b,
                                   (temp_data[i] >> b) & 1u ? E2E_ONE : E2E_ZERO);
        }
    }

    mock_tim1.SR |= TIM_SR_UIF;
    ds18b20_poll();
    TEST_ASSERT_EQUAL_UINT8(7, ds18b20_test_get_state());

    mock_tim1.SR |= TIM_SR_UIF;
    ds18b20_poll();

    TEST_ASSERT_TRUE(test_spy_complete_called);
    TEST_ASSERT_EQUAL_INT(223, test_spy_complete_value);
    TEST_ASSERT_EQUAL_UINT8(0, ds18b20_test_get_state());
}

/*-------------------------------------------------------------
 *  Repeated-cycle / re-select-in-callback helpers
 * -----------------------------------------------------------*/
static void ts_inject_scratchpad(const uint8_t* sd) {
    for (uint8_t i = 0; i < 9; i++) {
        for (uint8_t b = 0; b < 8; b++) {
            ds18b20_test_set_pulse(i * 8 + b,
                                   ((sd[i] >> b) & 1u) ? E2E_ONE : E2E_ZERO);
        }
    }
}

static int16_t ts_temp(uint16_t raw) {
    int16_t r = (int16_t)raw;
    return (int16_t)(((int32_t)r * 10 + (r < 0 ? -8 : 8)) / 16);
}

static void ts_drive_measurement_raw(uint16_t raw) {
    uint8_t sd[9];
    sd[0] = (uint8_t)(raw & 0xFF);
    sd[1] = (uint8_t)((raw >> 8) & 0xFF);
    sd[2] = 0x4B;
    sd[3] = 0x46;
    sd[4] = 0x7F;
    sd[5] = 0xFF;
    sd[6] = 0x08;
    sd[7] = 0x10;
    sd[8] = ds18b20_crc8(sd, 8);

    ds18b20_test_set_edge(0, 510);
    ds18b20_test_set_edge(1, 700);
    mock_tim1.SR |= TIM_SR_UIF;
    ds18b20_poll();
    TEST_ASSERT_EQUAL_UINT8(2, ds18b20_test_get_state());

    ds18b20_test_set_edge(0, 510);
    ds18b20_test_set_edge(1, 700);
    mock_tim1.SR |= TIM_SR_UIF;
    ds18b20_poll();
    TEST_ASSERT_EQUAL_UINT8(3, ds18b20_test_get_state());

    mock_tim1.SR |= TIM_SR_UIF;
    ds18b20_poll();
    TEST_ASSERT_EQUAL_UINT8(4, ds18b20_test_get_state());

    ds18b20_test_set_edge(0, 510);
    ds18b20_test_set_edge(1, 700);
    mock_tim1.SR |= TIM_SR_UIF;
    ds18b20_poll();
    TEST_ASSERT_EQUAL_UINT8(5, ds18b20_test_get_state());

    ds18b20_test_set_edge(0, 510);
    ds18b20_test_set_edge(1, 700);
    mock_tim1.SR |= TIM_SR_UIF;
    ds18b20_poll();
    TEST_ASSERT_EQUAL_UINT8(6, ds18b20_test_get_state());

    ts_inject_scratchpad(sd);
    mock_tim1.SR |= TIM_SR_UIF;
    ds18b20_poll();
    TEST_ASSERT_EQUAL_UINT8(7, ds18b20_test_get_state());

    mock_tim1.SR |= TIM_SR_UIF;
    ds18b20_poll();

    TEST_ASSERT_TRUE(test_spy_complete_called);
    TEST_ASSERT_EQUAL_INT(ts_temp(raw), test_spy_complete_value);
    TEST_ASSERT_EQUAL_UINT8(0, ds18b20_test_get_state());
}

void test_state_machine_repeated_measurement_cycles(void) {
    spy_reset();
    ds18b20_init();
    ds18b20_test_reset_ctx();

    uint8_t before = test_spy_complete_count;
    ts_drive_measurement_raw(0x0164); /* 223 tenths */
    TEST_ASSERT_EQUAL_UINT8(0, ds18b20_test_get_scan_mode());
    ts_drive_measurement_raw(0x012C); /* 188 tenths */
    TEST_ASSERT_EQUAL_UINT8(0, ds18b20_test_get_scan_mode());
    ts_drive_measurement_raw(0x0164); /* 223 tenths again */

    TEST_ASSERT_EQUAL_UINT8(before + 3, test_spy_complete_count);
    TEST_ASSERT_EQUAL_INT(ts_temp(0x0164), test_spy_complete_values[0]);
    TEST_ASSERT_EQUAL_INT(ts_temp(0x012C), test_spy_complete_values[1]);
    TEST_ASSERT_EQUAL_INT(ts_temp(0x0164), test_spy_complete_values[2]);
}

static uint8_t g_reselect_called;
static uint8_t g_reselect_rom[8] = {0x28, 0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF, 0x10};
static void ts_reselect_hook(void) {
    g_reselect_called = 1;
    ds18b20_select(g_reselect_rom);
}

void test_state_machine_reselect_in_callback(void) {
    spy_reset();
    g_reselect_called = 0;
    test_spy_on_complete_hook = ts_reselect_hook;

    ds18b20_init();
    ds18b20_test_reset_ctx();

    uint8_t before = test_spy_complete_count;
    ts_drive_measurement_raw(0x0164); /* first cycle; callback re-selects a device */

    TEST_ASSERT_TRUE(g_reselect_called);
    TEST_ASSERT_EQUAL_UINT8(1, ds18b20_test_get_address_mode());
    TEST_ASSERT_EQUAL_UINT8(0, ds18b20_test_get_scan_mode());

    ts_drive_measurement_raw(0x0164); /* re-armed cycle runs again */

    TEST_ASSERT_EQUAL_UINT8(before + 2, test_spy_complete_count);

    test_spy_on_complete_hook = 0;
}

/*-------------------------------------------------------------
 *  Test: ds18b20_init() configures clocks, prescaler and GPIO
 * -----------------------------------------------------------*/
void test_state_machine_init_configures_registers(void) {
    hw_reset_all();
    ds18b20_init();

    TEST_ASSERT_BITS_HIGH(RCC_APB2ENR_IOPAEN | RCC_APB2ENR_TIM1EN, mock_rcc.APB2ENR);
    TEST_ASSERT_BITS_HIGH(RCC_AHBENR_DMA1EN, mock_rcc.AHBENR);
    TEST_ASSERT_EQUAL_UINT32(71, mock_tim1.PSC); /* 72MHz/72 = 1MHz -> 1us */
    TEST_ASSERT_BITS_HIGH(TIM_BDTR_MOE, mock_tim1.BDTR);
    TEST_ASSERT_BITS_HIGH(GPIO_CRH_CNF8_0 | GPIO_CRH_CNF8_1 | GPIO_CRH_MODE8_1, mock_gpioa.CRH);
}

/*-------------------------------------------------------------
 *  Test: ds18b20_busy() spy - START sets busy, DECODE clears it
 * -----------------------------------------------------------*/
void test_state_machine_busy_spy(void) {
    spy_reset();
    test_spy_busy_calls = 0;
    test_spy_busy_last_action = 0;
    ds18b20_init();
    ds18b20_test_reset_ctx();

    /* IDLE -> START -> CONVERT: busy(1) at START */
    mock_tim1.SR |= TIM_SR_UIF;
    ds18b20_poll();
    TEST_ASSERT_EQUAL_UINT8(1, test_spy_busy_calls);
    TEST_ASSERT_EQUAL_UINT8(1, test_spy_busy_last_action);

    /* Decode with valid CRC: busy(0) at DECODE */
    uint8_t temp_data[9] = {0x64, 0x01, 0x4B, 0x46, 0x7F, 0xFF, 0x08, 0x10, 0};
    temp_data[8] = ds18b20_crc8(temp_data, 8);
    for (int i = 0; i < 9; i++) {
        for (int b = 0; b < 8; b++) {
            ds18b20_test_set_pulse(i * 8 + b,
                                   (temp_data[i] >> b) & 1u ? 5u : 60u);
        }
    }
    ds18b20_test_set_state(7);
    mock_tim1.SR |= TIM_SR_UIF;
    ds18b20_poll();

    TEST_ASSERT_EQUAL_UINT8(0, test_spy_busy_last_action);
}

/*-------------------------------------------------------------
 *  Test: NO_SENSOR error in the REQUEST state (after a
 *  successful CONVERT) reports the error and returns to IDLE
 * -----------------------------------------------------------*/
void test_state_machine_request_presence_fail(void) {
    spy_reset();
    ds18b20_init();
    ds18b20_test_reset_ctx();

    ds18b20_test_set_state(5);
    ds18b20_test_set_edge(0, 100); /* bad reset */
    ds18b20_test_set_edge(1, 100); /* bad presence */

    mock_tim1.SR |= TIM_SR_UIF;
    ds18b20_poll();

    TEST_ASSERT_TRUE(test_spy_complete_called);
    TEST_ASSERT_EQUAL_INT(DS18B20_TEMP_ERROR_NO_SENSOR, test_spy_complete_value);
    TEST_ASSERT_EQUAL_UINT8(0, ds18b20_test_get_state());
    /* Busy indicator must be cleared even though DECODE was skipped. */
    TEST_ASSERT_EQUAL_UINT8(0, test_spy_busy_last_action);
}

/*-------------------------------------------------------------
 *  Test: a presence failure in issue_command (CONVERT state) must
 *  clear the busy indicator. busy(1) is asserted in START; the early
 *  error exit used to skip DECODE (where busy(0) normally lives), so
 *  it must clear busy itself.
 * -----------------------------------------------------------*/
void test_state_machine_presence_fail_clears_busy(void) {
    spy_reset();
    test_spy_busy_calls = 0;
    test_spy_busy_last_action = 0;
    ds18b20_init();
    ds18b20_test_reset_ctx();

    /* IDLE -> START: busy(1) is asserted, as in a real measurement. */
    mock_tim1.SR |= TIM_SR_UIF;
    ds18b20_poll();
    TEST_ASSERT_EQUAL_UINT8(1, test_spy_busy_last_action); /* busy ON */

    /* Now in CONVERT with no device present: issue_command must fail and
     * turn busy OFF before reporting NO_SENSOR. */
    ds18b20_test_set_edge(0, 100); /* bad reset */
    ds18b20_test_set_edge(1, 100); /* bad presence */
    mock_tim1.SR |= TIM_SR_UIF;
    ds18b20_poll();

    TEST_ASSERT_TRUE(test_spy_complete_called);
    TEST_ASSERT_EQUAL_INT(DS18B20_TEMP_ERROR_NO_SENSOR, test_spy_complete_value);
    TEST_ASSERT_EQUAL_UINT8(0, ds18b20_test_get_state());
    TEST_ASSERT_EQUAL_UINT8(0, test_spy_busy_last_action); /* busy OFF */
}

/*-------------------------------------------------------------
 *  Test: CRC_FAIL in the middle of a full cycle - CONVERT passes,
 *  then the scratchpad read returns a bad CRC
 * -----------------------------------------------------------*/
void test_state_machine_crc_fail_mid_cycle(void) {
    spy_reset();
    ds18b20_init();
    ds18b20_test_reset_ctx();

    /* Full cycle with a corrupted scratchpad read */
    mock_tim1.SR |= TIM_SR_UIF;
    ds18b20_poll();
    TEST_ASSERT_EQUAL_UINT8(2, ds18b20_test_get_state());

    ds18b20_test_set_edge(0, 510);
    ds18b20_test_set_edge(1, 700);
    mock_tim1.SR |= TIM_SR_UIF;
    ds18b20_poll();
    TEST_ASSERT_EQUAL_UINT8(3, ds18b20_test_get_state());

    mock_tim1.SR |= TIM_SR_UIF;
    ds18b20_poll();
    TEST_ASSERT_EQUAL_UINT8(4, ds18b20_test_get_state());

    ds18b20_test_set_edge(0, 510);
    ds18b20_test_set_edge(1, 700);
    mock_tim1.SR |= TIM_SR_UIF;
    ds18b20_poll();
    TEST_ASSERT_EQUAL_UINT8(5, ds18b20_test_get_state());

    ds18b20_test_set_edge(0, 510);
    ds18b20_test_set_edge(1, 700);
    mock_tim1.SR |= TIM_SR_UIF;
    ds18b20_poll();
    TEST_ASSERT_EQUAL_UINT8(6, ds18b20_test_get_state());

    /* Corrupt the data so the CRC fails */
    uint8_t temp_data[9] = {0x64, 0x01, 0x4B, 0x46, 0x7F, 0xFF, 0x08, 0x10, 0};
    temp_data[8] = (uint8_t)(ds18b20_crc8(temp_data, 8) ^ 0xFF); /* bad CRC */
    for (int i = 0; i < 9; i++) {
        for (int b = 0; b < 8; b++) {
            ds18b20_test_set_pulse(i * 8 + b,
                                   (temp_data[i] >> b) & 1u ? 5u : 60u);
        }
    }

    mock_tim1.SR |= TIM_SR_UIF;
    ds18b20_poll();
    TEST_ASSERT_EQUAL_UINT8(7, ds18b20_test_get_state());

    mock_tim1.SR |= TIM_SR_UIF;
    ds18b20_poll();

    TEST_ASSERT_TRUE(test_spy_complete_called);
    TEST_ASSERT_EQUAL_INT(DS18B20_TEMP_ERROR_CRC_FAIL, test_spy_complete_value);
    TEST_ASSERT_EQUAL_UINT8(0, ds18b20_test_get_state());
}

/*-------------------------------------------------------------
 *  Test: Full Skip-ROM cycle produces the exact temperature
 *  (matches the E2E Match-ROM value; Skip ROM broadcast mode)
 * -----------------------------------------------------------*/
void test_state_machine_full_cycle_skip_rom_value(void) {
    spy_reset();
    ds18b20_init();
    ds18b20_test_reset_ctx();
    ds18b20_test_set_address_mode(0);

    mock_tim1.SR |= TIM_SR_UIF;
    ds18b20_poll();
    TEST_ASSERT_EQUAL_UINT8(2, ds18b20_test_get_state());

    ds18b20_test_set_edge(0, 510);
    ds18b20_test_set_edge(1, 700);
    mock_tim1.SR |= TIM_SR_UIF;
    ds18b20_poll();
    TEST_ASSERT_EQUAL_UINT8(3, ds18b20_test_get_state());

    mock_tim1.SR |= TIM_SR_UIF;
    ds18b20_poll();
    TEST_ASSERT_EQUAL_UINT8(4, ds18b20_test_get_state());

    ds18b20_test_set_edge(0, 510);
    ds18b20_test_set_edge(1, 700);
    mock_tim1.SR |= TIM_SR_UIF;
    ds18b20_poll();
    TEST_ASSERT_EQUAL_UINT8(5, ds18b20_test_get_state());

    ds18b20_test_set_edge(0, 510);
    ds18b20_test_set_edge(1, 700);
    mock_tim1.SR |= TIM_SR_UIF;
    ds18b20_poll();
    TEST_ASSERT_EQUAL_UINT8(6, ds18b20_test_get_state());

    uint8_t temp_data[9] = {0x64, 0x01, 0x4B, 0x46, 0x7F, 0xFF, 0x08, 0x10, 0};
    temp_data[8] = ds18b20_crc8(temp_data, 8);
    for (int i = 0; i < 9; i++) {
        for (int b = 0; b < 8; b++) {
            ds18b20_test_set_pulse(i * 8 + b,
                                   (temp_data[i] >> b) & 1u ? 5u : 60u);
        }
    }

    mock_tim1.SR |= TIM_SR_UIF;
    ds18b20_poll();
    TEST_ASSERT_EQUAL_UINT8(7, ds18b20_test_get_state());

    mock_tim1.SR |= TIM_SR_UIF;
    ds18b20_poll();

    TEST_ASSERT_TRUE(test_spy_complete_called);
    TEST_ASSERT_EQUAL_INT(223, test_spy_complete_value);
    TEST_ASSERT_EQUAL_UINT8(0, ds18b20_test_get_state());
}

/*-------------------------------------------------------------
 *  Run all state machine tests
 * -----------------------------------------------------------*/
void run_test_state_machine(void) {
    TEST_RUN(test_state_machine_initial_state_after_init);
    TEST_RUN(test_state_machine_first_poll_transitions_to_START);
    TEST_RUN(test_state_machine_UIF_clear_no_transition);
    TEST_RUN(test_state_machine_convert_with_presence_pass);
    TEST_RUN(test_state_machine_convert_with_presence_fail);
    TEST_RUN(test_state_machine_wait_transitions_to_continue);
    TEST_RUN(test_state_machine_continue_transitions_to_request);
    TEST_RUN(test_state_machine_request_with_presence_pass);
    TEST_RUN(test_state_machine_read_transitions_to_decode);
    TEST_RUN(test_state_machine_decode_with_valid_crc);
    TEST_RUN(test_state_machine_decode_with_invalid_crc);
    TEST_RUN(test_state_machine_invalid_state_triggers_error);
    TEST_RUN(test_state_machine_full_cycle_skip_rom);
    TEST_RUN(test_state_machine_match_rom_mode);
    TEST_RUN(test_state_machine_decode_all_zero_reports_error);
    TEST_RUN(test_state_machine_decode_all_FF_reports_error);
    TEST_RUN(test_state_machine_decode_all_FF_match_rom_reports_no_sensor);
    TEST_RUN(test_state_machine_decode_corrupt_match_rom_reports_crc);
    TEST_RUN(test_state_machine_decode_wrong_byte5_reports_error);
    TEST_RUN(test_state_machine_search_select_measure_e2e);
    TEST_RUN(test_state_machine_init_configures_registers);
    TEST_RUN(test_state_machine_busy_spy);
    TEST_RUN(test_state_machine_request_presence_fail);
    TEST_RUN(test_state_machine_presence_fail_clears_busy);
    TEST_RUN(test_state_machine_crc_fail_mid_cycle);
    TEST_RUN(test_state_machine_full_cycle_skip_rom_value);
    TEST_RUN(test_state_machine_repeated_measurement_cycles);
    TEST_RUN(test_state_machine_reselect_in_callback);
}
