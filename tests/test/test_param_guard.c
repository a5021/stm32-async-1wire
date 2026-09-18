/* ============================================================
 *  test_param_guard.c - Rejected-parameter schedules
 *
 *  onewire_write_pulses/read_data guard out-of-range sizes with
 *  `assert(0 && ...); return 0;`, and onewire_write_command()
 *  rejects empty/oversized commands with `assert(0 && ...); return;`.
 *  With NDEBUG the assert is a no-op, so this TU is built only by
 *  the release-semantics test variant (make test-ndebug*): TEST_FLAG
 *  + -DNDEBUG -DOW_TEST_PARAM_GUARD.  In the default debug build
 *  these calls would abort the process, which is exactly what the OS
 *  is compiled to do, so the file is compiled empty there (all body
 *  code sits under OW_TEST_PARAM_GUARD).
 *
 *  Each reject must be observable outside the library: the return
 *  value is 0 AND no timer/DMA operation may be scheduled (the
 *  shared engine stays idle, so a subsequent onewire_bus_done()
 *  poll is not fooled into waiting on a phantom transfer).
 * ============================================================ */

#ifdef OW_TEST_PARAM_GUARD

#include "hw_model.h"
#include "mock_target.h"
#include "onewire_internal.h"
#include "ow_port.h"
#include "unity.h"

/* ---- helpers ---- */

/* Assert that the shared TIM1/DMA engine is fully idle. */
static void assert_engine_idle(void) {
    TEST_ASSERT_BITS_LOW(TIM_CR1_CEN, mock_tim1.CR1);
    TEST_ASSERT_EQUAL_UINT32(0u, mock_feed_ch.CNDTR);
    TEST_ASSERT_BITS_LOW(DMA_CCR_EN, mock_feed_ch.CCR);
    TEST_ASSERT_EQUAL_UINT32(0u, mock_dma1_ch4.CNDTR);
    TEST_ASSERT_BITS_LOW(DMA_CCR_EN, mock_dma1_ch4.CCR);
}

/* ---- rejected schedules ---- */

void test_guard_write_pulses_zero_rejected(void) {
    uint8_t pulse = ONEWIRE_ONE_PULSE;
    uint8_t st = onewire_write_pulses(&pulse, 0);
    TEST_ASSERT_EQUAL_UINT8(0u, st);
    assert_engine_idle();
}

void test_guard_write_pulses_over_max_rejected(void) {
    uint8_t pulse = ONEWIRE_ONE_PULSE;
    uint8_t st = onewire_write_pulses(&pulse, (uint16_t)(ONEWIRE_MAX_SLOTS + 1u));
    TEST_ASSERT_EQUAL_UINT8(0u, st);
    assert_engine_idle();
}

void test_guard_write_command_zero_rejected(void) {
    uint8_t byte = 0xCC;
    onewire_write_command(&byte, 0);
    assert_engine_idle();
}

void test_guard_write_command_over_max_rejected(void) {
    uint8_t bytes[ONEWIRE_CMD_MAX_BYTES + 1];
    memset(bytes, 0x00, sizeof(bytes));
    onewire_write_command(bytes, (uint8_t)(ONEWIRE_CMD_MAX_BYTES + 1u));
    assert_engine_idle();
}

void test_guard_read_data_zero_rejected(void) {
    uint8_t rx[ONEWIRE_MAX_READ_BYTES * ONEWIRE_BITS_PER_BYTE];
    uint8_t st = onewire_read_data(rx, 0);
    TEST_ASSERT_EQUAL_UINT8(0u, st);
    assert_engine_idle();
}

void test_guard_read_data_over_max_rejected(void) {
    uint8_t rx[ONEWIRE_MAX_READ_BYTES * ONEWIRE_BITS_PER_BYTE];
    uint8_t st = onewire_read_data(rx, (uint8_t)(ONEWIRE_MAX_READ_BYTES + 1u));
    TEST_ASSERT_EQUAL_UINT8(0u, st);
    assert_engine_idle();
}

/* ---- accepted schedules still work ---- */

void test_guard_write_bit_schedules(void) {
    uint8_t st = onewire_write_bit(1);
    TEST_ASSERT_EQUAL_UINT8(1u, st);
    TEST_ASSERT_TRUE(mock_tim1.CR1 & TIM_CR1_CEN);
    TEST_ASSERT_EQUAL_UINT32(0u, mock_tim1.RCR);

    uint32_t slots = (uint32_t)(mock_tim1.RCR & 0xFFu) + 1u;
    TEST_ASSERT_TRUE(hw_run_until_uif(slots));
}

void test_guard_write_pulses_schedules(void) {
    uint8_t pulse = ONEWIRE_ONE_PULSE;
    uint8_t st = onewire_write_pulses(&pulse, 1);
    TEST_ASSERT_EQUAL_UINT8(1u, st);
    TEST_ASSERT_TRUE(mock_tim1.CR1 & TIM_CR1_CEN);

    uint32_t slots = (uint32_t)(mock_tim1.RCR & 0xFFu) + 1u;
    TEST_ASSERT_TRUE(hw_run_until_uif(slots));
}

void test_guard_write_command_schedules(void) {
    const uint8_t cmd[2] = {0xCC, 0x44};
    hw_register_buf((const void*)(uintptr_t)onewire_test_cmd_feed_addr());
    onewire_write_command(cmd, 2);
    TEST_ASSERT_TRUE(mock_tim1.CR1 & TIM_CR1_CEN);
    TEST_ASSERT_EQUAL_UINT32((uint32_t)(ONEWIRE_BITS_PER_BYTE * 2u - 1u),
                             mock_tim1.RCR); /* 16 slots for 2 bytes */
    TEST_ASSERT_EQUAL_UINT32(16u, mock_feed_ch.CNDTR);
    TEST_ASSERT_EQUAL_UINT32((uint32_t)(uintptr_t)onewire_test_cmd_feed_addr(),
                             mock_feed_ch.CMAR);

    uint32_t slots = (uint32_t)(mock_tim1.RCR & 0xFFu) + 1u;
    TEST_ASSERT_TRUE(hw_run_until_uif(slots));
}

void test_guard_read_data_schedules(void) {
    uint8_t rx[ONEWIRE_BITS_PER_BYTE];
    memset(rx, 0xEE, sizeof(rx));
    hw_register_buf(rx);

    uint8_t st = onewire_read_data(rx, 1);
    TEST_ASSERT_EQUAL_UINT8(1u, st);
    TEST_ASSERT_TRUE(mock_tim1.CR1 & TIM_CR1_CEN);
    TEST_ASSERT_EQUAL_UINT32(8u, mock_dma1_ch4.CNDTR);

    uint32_t slots = (uint32_t)(mock_tim1.RCR & 0xFFu) + 1u;
    TEST_ASSERT_TRUE(hw_run_until_uif(slots));
}

/* ---- runner ---- */

void run_test_param_guard(void) {
    TEST_RUN(test_guard_write_pulses_zero_rejected);
    TEST_RUN(test_guard_write_pulses_over_max_rejected);
    TEST_RUN(test_guard_write_command_zero_rejected);
    TEST_RUN(test_guard_write_command_over_max_rejected);
    TEST_RUN(test_guard_read_data_zero_rejected);
    TEST_RUN(test_guard_read_data_over_max_rejected);
    TEST_RUN(test_guard_write_bit_schedules);
    TEST_RUN(test_guard_write_pulses_schedules);
    TEST_RUN(test_guard_write_command_schedules);
    TEST_RUN(test_guard_read_data_schedules);
}

#endif /* OW_TEST_PARAM_GUARD */