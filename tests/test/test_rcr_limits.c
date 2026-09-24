/* ============================================================
 *  test_rcr_limits.c - TIM1 RCR 8-bit hardware limit tests
 *
 *  Verifies the public API correctly handles the TIM1 8-bit RCR
 *  boundary: slots=256 works, slots=0/257+ are rejected by the
 *  onewire.c layer before reaching the hardware driver.
 *
 *  The test build does NOT define NDEBUG, so assert() in the
 *  guards would abort the process. These tests therefore exercise
 *  only the safe side of the boundary (valid range and the
 *  internal-only limits that the ds18b20 driver never exceeds).
 *  The out-of-range rejection itself is tested by
 *  tests/test/test_param_guard.c, which is compiled only into the
 *  release-semantics build (make test-ndebug*, -DNDEBUG) where the
 *  guard asserts compile out and the return status is observable.
 * ============================================================ */

#include "ds18b20.h"
#include "ds18b20_test_access.h"
#include "hw_model.h"
#include "mock_target.h"
#include "onewire.h"
#include "ow_port.h"
#include "unity.h"

/* ---- helpers ---- */

/* Multi-slot pulse buffer: 256 entries + trailing bus release 0. */
static ow_pulse_t max_pulses[ONEWIRE_MAX_SLOTS + 1];

/* Capture source: return slot index as byte value. */
static uint16_t idx_capture_src(uint32_t idx) { return (uint16_t)(idx & 0xFFu); }

/* ---- tests ---- */

void test_rcr_constants_values(void) {
    /* Public constants must match the hardware constraint. */
    TEST_ASSERT_EQUAL_UINT32(256u, ONEWIRE_MAX_SLOTS);
    TEST_ASSERT_EQUAL_UINT32(32u, ONEWIRE_MAX_READ_BYTES);
    TEST_ASSERT_EQUAL_UINT32(256u / ONEWIRE_BITS_PER_BYTE, ONEWIRE_MAX_READ_BYTES);
}

void test_rcr_write_slots_256_ok(void) {
    /* slots == 256 -> RCR = 255 (fits 8-bit). The full boundary must work. */
    for (uint16_t i = 0; i < ONEWIRE_MAX_SLOTS; i++) {
        max_pulses[i] = ONEWIRE_ONE_PULSE;
    }
    max_pulses[ONEWIRE_MAX_SLOTS] = 0; /* trailing bus release */
    hw_register_buf(&max_pulses[1]);

    onewire_write_slots(max_pulses, ONEWIRE_MAX_SLOTS);
    TEST_ASSERT_TRUE(mock_tim1.CR1 & TIM_CR1_CEN);
    TEST_ASSERT_EQUAL_UINT32(ONEWIRE_MAX_SLOTS - 1u, mock_tim1.RCR);
    TEST_ASSERT_EQUAL_UINT32(ONEWIRE_MAX_SLOTS, mock_feed_ch.CNDTR);

    /* Run to completion: the DMA must fully drain. */
    uint32_t slots = (uint32_t)(mock_tim1.RCR & 0xFFu) + 1u;
    TEST_ASSERT_TRUE(hw_run_until_uif(slots));
    TEST_ASSERT_EQUAL_UINT32(0u, mock_feed_ch.CNDTR);
    TEST_ASSERT_BITS_LOW(DMA_CCR_EN, mock_feed_ch.CCR);
}

void test_rcr_write_slots_1_ok(void) {
    /* slots == 1: single-slot path, no DMA. Must not touch the feed channel. */
    ow_pulse_t pulse = ONEWIRE_ONE_PULSE;
    onewire_write_slots(&pulse, 1);
    TEST_ASSERT_TRUE(mock_tim1.CR1 & TIM_CR1_CEN);
    TEST_ASSERT_EQUAL_UINT32(0u, mock_tim1.RCR);
    TEST_ASSERT_EQUAL_UINT32(0u, mock_feed_ch.CNDTR);
    TEST_ASSERT_BITS_LOW(DMA_CCR_EN, mock_feed_ch.CCR);

    uint32_t slots = (uint32_t)(mock_tim1.RCR & 0xFFu) + 1u;
    TEST_ASSERT_TRUE(hw_run_until_uif(slots));
}

void test_rcr_read_data_32_ok(void) {
    /* bytes == 32 -> bits = 256 -> RCR = 255 (fits 8-bit). Full boundary. */
    uint8_t rx[ONEWIRE_MAX_READ_BYTES * ONEWIRE_BITS_PER_BYTE];
    memset(rx, 0xEE, sizeof(rx));
    hw_register_buf(rx);

    onewire_read_data(rx, ONEWIRE_MAX_READ_BYTES);
    TEST_ASSERT_TRUE(mock_tim1.CR1 & TIM_CR1_CEN);
    TEST_ASSERT_EQUAL_UINT32(ONEWIRE_MAX_SLOTS - 1u, mock_tim1.RCR);
    TEST_ASSERT_EQUAL_UINT32(ONEWIRE_MAX_SLOTS, mock_dma1_ch4.CNDTR);

    uint32_t slots = (uint32_t)(mock_tim1.RCR & 0xFFu) + 1u;
    TEST_ASSERT_TRUE(hw_run_until_uif(slots));
    TEST_ASSERT_EQUAL_UINT32(0u, mock_dma1_ch4.CNDTR);
    TEST_ASSERT_BITS_LOW(DMA_CCR_EN, mock_dma1_ch4.CCR);
}

void test_rcr_read_data_1_ok(void) {
    /* bytes == 1 -> bits = 8 -> RCR = 7. Must work. */
    uint8_t rx[8];
    memset(rx, 0xEE, sizeof(rx));
    hw_register_buf(rx);

    onewire_read_data(rx, 1);
    TEST_ASSERT_TRUE(mock_tim1.CR1 & TIM_CR1_CEN);
    TEST_ASSERT_EQUAL_UINT32(7u, mock_tim1.RCR);
    TEST_ASSERT_EQUAL_UINT32(8u, mock_dma1_ch4.CNDTR);

    uint32_t slots = (uint32_t)(mock_tim1.RCR & 0xFFu) + 1u;
    TEST_ASSERT_TRUE(hw_run_until_uif(slots));
}

void test_rcr_write_bit_single_slot_uses_no_dma(void) {
    /* onewire_write_bit (slots == 1) must not touch the DMA feed channel.
     * Regression: confirm the single-slot path is still correct. */
    onewire_write_bit(1);
    TEST_ASSERT_TRUE(mock_tim1.CR1 & TIM_CR1_CEN);
    TEST_ASSERT_EQUAL_UINT32(0u, mock_tim1.RCR);
    TEST_ASSERT_EQUAL_UINT32(0u, mock_feed_ch.CNDTR);
    TEST_ASSERT_BITS_LOW(DMA_CCR_EN, mock_feed_ch.CCR);

    uint32_t slots = (uint32_t)(mock_tim1.RCR & 0xFFu) + 1u;
    TEST_ASSERT_TRUE(hw_run_until_uif(slots));
}

void test_rcr_write_slots_256_buffer_integrity(void) {
    /* After a full 256-slot write the source buffer must be untouched
     * (DMA reads, does not write) and the feed DMA must be fully drained. */
    for (uint16_t i = 0; i < ONEWIRE_MAX_SLOTS; i++) {
        max_pulses[i] = (uint8_t)(i & 0xFFu);
    }
    max_pulses[ONEWIRE_MAX_SLOTS] = 0;
    hw_register_buf(&max_pulses[1]);

    onewire_write_slots(max_pulses, ONEWIRE_MAX_SLOTS);
    uint32_t slots = (uint32_t)(mock_tim1.RCR & 0xFFu) + 1u;
    TEST_ASSERT_TRUE(hw_run_until_uif(slots));

    /* Buffer intact: DMA read-only, nothing overwritten. */
    for (uint16_t i = 0; i < ONEWIRE_MAX_SLOTS; i++) {
        TEST_ASSERT_EQUAL_UINT8((uint8_t)(i & 0xFFu), max_pulses[i]);
    }
    TEST_ASSERT_EQUAL_UINT8(0u, max_pulses[ONEWIRE_MAX_SLOTS]);
    TEST_ASSERT_EQUAL_UINT32(0u, mock_feed_ch.CNDTR);
    TEST_ASSERT_BITS_LOW(DMA_CCR_EN, mock_feed_ch.CCR);
}

void test_rcr_read_data_32_full_capture(void) {
    /* 32-byte read = 256 capture transfers. Verify the capture DMA fills
     * every byte of the destination and fully drains. */
    uint8_t rx[ONEWIRE_MAX_READ_BYTES * ONEWIRE_BITS_PER_BYTE];
    memset(rx, 0xEE, sizeof(rx));
    hw_register_buf(rx);
    hw_set_capture_source(idx_capture_src);

    onewire_read_data(rx, ONEWIRE_MAX_READ_BYTES);
    TEST_ASSERT_EQUAL_UINT32(ONEWIRE_MAX_SLOTS, mock_dma1_ch4.CNDTR);

    uint32_t slots = (uint32_t)(mock_tim1.RCR & 0xFFu) + 1u;
    TEST_ASSERT_TRUE(hw_run_until_uif(slots));

    TEST_ASSERT_EQUAL_UINT32(0u, mock_dma1_ch4.CNDTR);
    TEST_ASSERT_BITS_LOW(DMA_CCR_EN, mock_dma1_ch4.CCR);
    TEST_ASSERT_EQUAL_UINT32(ONEWIRE_MAX_SLOTS, hw_capture_count());

    /* Each byte of the destination must hold the low byte of its capture value. */
    for (uint16_t i = 0; i < ONEWIRE_MAX_SLOTS; i++) {
        TEST_ASSERT_EQUAL_UINT8((uint8_t)(i & 0xFFu), rx[i]);
    }
}

/* ---- runner ---- */

void run_test_rcr_limits(void) {
    TEST_RUN(test_rcr_constants_values);
    TEST_RUN(test_rcr_write_slots_256_ok);
    TEST_RUN(test_rcr_write_slots_1_ok);
    TEST_RUN(test_rcr_read_data_32_ok);
    TEST_RUN(test_rcr_read_data_1_ok);
    TEST_RUN(test_rcr_write_bit_single_slot_uses_no_dma);
    TEST_RUN(test_rcr_write_slots_256_buffer_integrity);
    TEST_RUN(test_rcr_read_data_32_full_capture);
}
