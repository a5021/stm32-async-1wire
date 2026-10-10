/* ============================================================
 *  test_dmamux.c - G0/G4 DMAMUX and H5 GPDMA request-routing tests
 *
 *  STM32G031 and STM32G474 have no fixed DMA request map: peripheral requests
 *  reach DMA channels through DMAMUX. The backend must program
 *  the mux before enabling each channel:
 *    - G0:  TIM1_CC2 (marker feed -> CCR3) = request 21 on channel 2
 *    - G0:  TIM1_CH4 (capture drain <- CCR4) = request 23 on channel 3
 *    - G4:  TIM1_CC2 (marker feed -> CCR3) = request 43 on channel 2
 *    - G4:  TIM1_CH4 (capture drain <- CCR4) = request 45 on channel 3
 *  H5 has no DMAMUX either: each GPDMA channel selects its request in
 *  CTR2.REQSEL, programmed by the PROG macros before each operation:
 *    - H5:  TIM1_CH2 (marker feed -> CCR3) = request 59 on channel 2
 *    - H5:  TIM1_CH4 (capture drain <- CCR4) = request 61 on channel 3
 *  (double-sourced from the H5 RM GPDMA mapping table and CubeH5
 *  stm32h5xx_ll_dma.h).
 *  Other backends have a fixed request map, so this suite
 *  compiles empty there.
 * ============================================================ */

#include "ds18b20.h"
#include "hw_model.h"
#include "mock_target.h"
#include "onewire.h"
#include "unity.h"

#if defined(OW_PORT_TARGET_G0) || defined(OW_PORT_TARGET_G4) || defined(OW_PORT_TARGET_H5)

#if defined(OW_PORT_TARGET_G4)
#define DMAMUX_EXPECT_FEED 43u
#define DMAMUX_EXPECT_CAP 45u
#elif defined(OW_PORT_TARGET_H5)
#define GPDMA_EXPECT_FEED_REQ 59u /* TIM1_CH2, in CTR2.REQSEL */
#define GPDMA_EXPECT_CAP_REQ 61u /* TIM1_CH4, in CTR2.REQSEL */
#else
#define DMAMUX_EXPECT_FEED 21u
#define DMAMUX_EXPECT_CAP 23u
#endif

static uint16_t rx_src(uint32_t idx) {
    return idx == 0 ? 510u : 700u; /* reset presence pulse durations */
}

void test_dmamux_capture_request_routed_on_reset(void) {
    static uint16_t capture[2];
    hw_reset_all();
    ds18b20_init();
    hw_register_buf(capture);
    hw_set_capture_source(rx_src);

    onewire_reset(capture); /* schedules the CC4 capture drain */
#if defined(OW_PORT_TARGET_H5)
    TEST_ASSERT_EQUAL_UINT32((GPDMA_EXPECT_CAP_REQ << DMA_CTR2_REQSEL_Pos),
                             mock_dma1_ch4.CTR2);
    TEST_ASSERT_BITS_HIGH(DMA_CCR_EN, mock_dma1_ch4.CCR);
#else
    TEST_ASSERT_EQUAL_UINT32(DMAMUX_EXPECT_CAP, mock_dmamux_ch3.CCR);
    TEST_ASSERT_BITS_HIGH(DMA_CCR_EN | DMA_CCR_MINC, mock_dma1_ch4.CCR);
#endif

    /* run-through: the mux-enable actually let the capture data flow */
    TEST_ASSERT_TRUE(hw_run_until_uif(mock_tim1.RCR + 1u));
    TEST_ASSERT_EQUAL_UINT32(0u, mock_dma1_ch4.CNDTR);
    TEST_ASSERT_EQUAL_UINT32(2u, hw_capture_count());
    TEST_ASSERT_EQUAL_UINT16(510u, capture[0]);
    TEST_ASSERT_EQUAL_UINT16(700u, capture[1]);
}

void test_dmamux_feed_request_routed_on_write(void) {
    ow_pulse_t pulses[ONEWIRE_BITS_PER_BYTE + 1];
    hw_reset_all();
    ds18b20_init();
    hw_register_buf(&pulses[1]);

    onewire_encode_byte(pulses, 0xCC);
    pulses[ONEWIRE_BITS_PER_BYTE] = 0; /* trailing hardware bus release */
    onewire_write_slots(pulses, ONEWIRE_BITS_PER_BYTE);
#if defined(OW_PORT_TARGET_H5)
    TEST_ASSERT_EQUAL_UINT32((GPDMA_EXPECT_FEED_REQ << DMA_CTR2_REQSEL_Pos) |
                                 (1u << DMA_CTR2_DREQ_Pos),
                             mock_feed_ch.CTR2);
    TEST_ASSERT_BITS_HIGH(DMA_CCR_EN, mock_feed_ch.CCR);
#else
    TEST_ASSERT_EQUAL_UINT32(DMAMUX_EXPECT_FEED, mock_dmamux_ch2.CCR);
    TEST_ASSERT_BITS_HIGH(DMA_CCR_EN | DMA_CCR_DIR, mock_feed_ch.CCR);
#endif

    /* run-through: the mux-enable actually let the feed data flow */
    TEST_ASSERT_TRUE(hw_run_until_uif(mock_tim1.RCR + 1u));
    TEST_ASSERT_EQUAL_UINT32(0u, mock_feed_ch.CNDTR);
    TEST_ASSERT_EQUAL_UINT8(ONEWIRE_BITS_PER_BYTE, hw_ccr3_feed_log()->count);
    TEST_ASSERT_EQUAL_UINT16(0u, hw_ccr3_feed_log()->values[ONEWIRE_BITS_PER_BYTE - 1]);
    TEST_ASSERT_BITS_LOW(DMA_CCR_EN, mock_feed_ch.CCR);
}

#endif /* OW_PORT_TARGET_G0 || OW_PORT_TARGET_G4 || OW_PORT_TARGET_H5 */

void run_test_dmamux(void) {
#if defined(OW_PORT_TARGET_G0) || defined(OW_PORT_TARGET_G4) || defined(OW_PORT_TARGET_H5)
    TEST_RUN(test_dmamux_capture_request_routed_on_reset);
    TEST_RUN(test_dmamux_feed_request_routed_on_write);
#endif
}
