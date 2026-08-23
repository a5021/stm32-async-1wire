/* ============================================================
 *  test_dmamux.c - G0 DMAMUX request-routing tests
 *
 *  STM32G031 has no fixed DMA request map: peripheral requests
 *  reach DMA channels through DMAMUX. The backend must program
 *  the mux before enabling each channel:
 *    - TIM1_CC2 (marker feed -> CCR3) = request 21 on channel 2
 *    - TIM1_CH4 (capture drain <- CCR4) = request 23 on channel 3
 *  Other backends have a fixed request map, so this suite
 *  compiles empty there.
 * ============================================================ */

#include "ds18b20.h"
#include "hw_model.h"
#include "mock_target.h"
#include "onewire.h"
#include "unity.h"

#if defined(OW_PORT_TARGET_G0)

void test_dmamux_capture_request_routed_on_reset(void) {
    static uint16_t edges[2];
    hw_reset_all();
    ds18b20_init();

    onewire_reset(edges); /* schedules the CC4 capture drain */
    TEST_ASSERT_EQUAL_UINT32(23u, mock_dmamux_ch3.CCR);
    TEST_ASSERT_BITS_HIGH(DMA_CCR_EN | DMA_CCR_MINC, mock_dma1_ch3.CCR);
}

void test_dmamux_feed_request_routed_on_write(void) {
    uint8_t pulses[ONEWIRE_BITS_PER_BYTE + 1];
    hw_reset_all();
    ds18b20_init();

    onewire_encode_byte(pulses, 0xCC);
    pulses[ONEWIRE_BITS_PER_BYTE] = 0; /* trailing hardware bus release */
    onewire_write_slots(pulses, ONEWIRE_BITS_PER_BYTE);
    TEST_ASSERT_EQUAL_UINT32(21u, mock_dmamux_ch2.CCR);
    TEST_ASSERT_BITS_HIGH(DMA_CCR_EN | DMA_CCR_DIR, mock_feed_ch.CCR);
}

#endif /* OW_PORT_TARGET_G0 */

void run_test_dmamux(void) {
#if defined(OW_PORT_TARGET_G0)
    TEST_RUN(test_dmamux_capture_request_routed_on_reset);
    TEST_RUN(test_dmamux_feed_request_routed_on_write);
#endif
}
