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

#include "unity.h"
#include "ds18b20.h"
#include "ds18b20_test_access.h"
#include "stm32f1xx.h"

/*-------------------------------------------------------------
 *  Callback spy for ds18b20_complete
 *  Records the last value passed to the callback
 * -----------------------------------------------------------*/
static int16_t spy_complete_value;
static int spy_complete_called;

void ds18b20_complete(int16_t temp) {
    spy_complete_value = temp;
    spy_complete_called = 1;
}

void spy_reset(void) {
    spy_complete_value = 0;
    spy_complete_called = 0;
}

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
    ds18b20_test_set_edge(0, 510);  /* Reset pulse within range */
    ds18b20_test_set_edge(1, 700);  /* Presence pulse within range */

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
    ds18b20_init();
    ds18b20_test_reset_ctx();

    /* Set state to CONVERT */
    ds18b20_test_set_state(2);

    /* Set presence edges to invalid values (device not present) */
    ds18b20_test_set_edge(0, 100);  /* Too short */
    ds18b20_test_set_edge(1, 100);  /* Too short */

    /* Simulate UIF set */
    mock_tim1.SR |= TIM_SR_UIF;

    /* Poll should report NO_SENSOR error and go to pause */
    ds18b20_poll();

    /* After failure, state goes back to IDLE (0) with pause timer running */
    TEST_ASSERT_EQUAL_UINT8(0, ds18b20_test_get_state());
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
    temp_data[8] = ds18b20_crc8(temp_data, 8);  /* Compute valid CRC */

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
    TEST_ASSERT_TRUE(spy_complete_called);
    TEST_ASSERT_EQUAL_INT(DS18B20_TEMP_ERROR_CRC_FAIL, spy_complete_value);

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
    TEST_ASSERT_TRUE(spy_complete_called);
    TEST_ASSERT_EQUAL_INT(DS18B20_TEMP_ERROR_CRC_FAIL, spy_complete_value);

    /* State should return to IDLE */
    TEST_ASSERT_EQUAL_UINT8(0, ds18b20_test_get_state());
}

/*-------------------------------------------------------------
 *  Test: DECODE with wrong reserved byte 5 reports error
 * -----------------------------------------------------------*/
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
    TEST_ASSERT_TRUE(spy_complete_called);
    TEST_ASSERT_EQUAL_INT(DS18B20_TEMP_ERROR_CRC_FAIL, spy_complete_value);

    /* State should return to IDLE */
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
    TEST_RUN(test_state_machine_decode_wrong_byte5_reports_error);
}
