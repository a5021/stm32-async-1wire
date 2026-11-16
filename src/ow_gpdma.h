/**
 * @file ow_gpdma.h
 * @brief Shared transfer-register vocabulary for the GPDMA/LPDMA backends
 *
 * Included from the STM32H5, STM32C5 and STM32U5 sections of ow_mcu.h. These
 * families share the same CTR1/CTR2/CBR1/CSAR/CDAR transfer-register IP. A
 * family defines only its own facts before including this header:
 *
 *  - OW_PORT_DMA_FEED / OW_PORT_DMA_CAPTURE: channel lvalues (*channel).
 *  - OW_PORT_GPDMA_FEED_CH / OW_PORT_GPDMA_CAPTURE_CH: channel pointers, used
 *    for CFCR flag retirement.
 *  - OW_PORT_GPDMA_REQ_FEED / OW_PORT_GPDMA_REQ_CAPTURE: CTR2.REQSEL values.
 *  - OW_PORT_GPDMA_FEED_CTR2_EXTRA: extra feed-side CTR2 bits (DREQ on GPDMA,
 *    zero on LPDMA).
 *
 * Everything else here is common: width templates, disable/program macros and
 * the BNDT-to-transfer conversion. Public macro names stay the same, so the
 * per-family contract tests keep pinning the resulting register words.
 *
 * Library-internal: not part of the public API.
 */

#ifndef OW_GPDMA_H
#define OW_GPDMA_H

#define OW_PORT_DMA_EN_BIT (1u << DMA_CCR_EN_Pos)

/* CTR1 templates. SDW/DDW_LOG2: 0 = byte, 1 = halfword. Capture reads the
 * halfword CCR4 into incrementing memory; feed reads incrementing byte memory
 * into the fixed halfword CCR3. Pinned by
 * tests/test/test_dma_contract.c::test_dma_cr_value_macros. */
#define OW_PORT_DMA_CR_RX16 \
    ((1u << DMA_CTR1_SDW_LOG2_Pos) | (1u << DMA_CTR1_DDW_LOG2_Pos) | (1u << DMA_CTR1_DINC_Pos))
#define OW_PORT_DMA_CR_RX8 \
    ((1u << DMA_CTR1_SDW_LOG2_Pos) | (1u << DMA_CTR1_DINC_Pos))
#define OW_PORT_DMA_CR_TX \
    ((1u << DMA_CTR1_SINC_Pos) | (1u << DMA_CTR1_DDW_LOG2_Pos))

/* Disable = disable + bounded EN wait (shared ow_port_dma_disable()) plus flag
 * retirement: TCF (transfer done) and SUSPF (our own mid-transfer suspend on
 * underrun re-arms). Error flags are deliberately NOT retired so a
 * misprogrammed channel stays visible instead of self-clearing. */
#define OW_PORT_DMA_DISABLE_FEED()                                \
    do {                                                          \
        ow_port_dma_disable(&OW_PORT_DMA_FEED.CCR);               \
        OW_PORT_GPDMA_FEED_CH->CFCR = (1u << DMA_CFCR_TCF_Pos) |  \
                                      (1u << DMA_CFCR_SUSPF_Pos); \
    } while (0)
#define OW_PORT_DMA_DISABLE_CAPTURE()                                \
    do {                                                             \
        ow_port_dma_disable(&OW_PORT_DMA_CAPTURE.CCR);               \
        OW_PORT_GPDMA_CAPTURE_CH->CFCR = (1u << DMA_CFCR_TCF_Pos) |  \
                                         (1u << DMA_CFCR_SUSPF_Pos); \
    } while (0)

/* CBR1.BNDT is a byte count, unlike classic CNDTR which counts transfers: a
 * halfword source (SDW_LOG2 = 1) consumes two bytes per element, so BNDT =
 * count * 2. RX8 keeps the halfword CCR4 source (reading the low byte) and
 * needs the same x2; the feed is byte-sourced and stays at BNDT = count. */
#define OW_PORT_DMA_PROG_CAPTURE(dst, count, cr)                             \
    do {                                                                     \
        OW_PORT_DMA_CAPTURE.CSAR = (uint32_t)&T1.CCR4;                       \
        OW_PORT_DMA_CAPTURE.CDAR = (uint32_t)(dst);                          \
        OW_PORT_DMA_CAPTURE.CBR1 =                                           \
            ((uint32_t)(count) << (((cr) >> DMA_CTR1_SDW_LOG2_Pos) & 0x3u)); \
        OW_PORT_DMA_CAPTURE.CTR2 =                                           \
            (OW_PORT_GPDMA_REQ_CAPTURE << DMA_CTR2_REQSEL_Pos);              \
        OW_PORT_DMA_CAPTURE.CTR1 = (uint32_t)(cr);                           \
        OW_PORT_DMA_CAPTURE.CCR = (1u << DMA_CCR_EN_Pos) |                   \
                                  OW_PORT_GPDMA_PRIO_BITS;                   \
    } while (0)
#define OW_PORT_DMA_PROG_FEED(src, count, cr)                                \
    do {                                                                     \
        OW_PORT_DMA_FEED.CSAR = (uint32_t)(src);                             \
        OW_PORT_DMA_FEED.CDAR = (uint32_t)&T1.CCR3;                          \
        OW_PORT_DMA_FEED.CBR1 =                                              \
            ((uint32_t)(count) << (((cr) >> DMA_CTR1_SDW_LOG2_Pos) & 0x3u)); \
        OW_PORT_DMA_FEED.CTR2 =                                              \
            (OW_PORT_GPDMA_REQ_FEED << DMA_CTR2_REQSEL_Pos) |                \
            OW_PORT_GPDMA_FEED_CTR2_EXTRA;                                   \
        OW_PORT_DMA_FEED.CTR1 = (uint32_t)(cr);                              \
        OW_PORT_DMA_FEED.CCR = (1u << DMA_CCR_EN_Pos) |                      \
                               OW_PORT_GPDMA_PRIO_BITS;                      \
    } while (0)

/* GPDMA/LPDMA count source bytes in CBR1.BNDT; the reset capture source is the
 * halfword CCR4 (SDW_LOG2 = 1), so one element consumes two BNDT bytes. */
#define OW_PORT_CAPTURE_REMAINING()                               \
    ((uint16_t)((OW_PORT_DMA_CAPTURE.CBR1 & DMA_CBR1_BNDT_Msk) >> \
                (DMA_CBR1_BNDT_Pos + 1u)))

/* Bus pin (identical on all three families): PA10 is TIM1_CH3, AF1,
 * open-drain. CC4 captures indirectly from TI3; TISEL reset selects the
 * direct TI4 pad, so force TI4SEL = 0. Drive strength is OW_BUS_DRIVE. */
#define OW_PORT_CONFIG_BUS_PIN()                                                      \
    do {                                                                              \
        PA.MODER = (PA.MODER & ~GPIO_MODER_MODE10) | GPIO_MODER_MODE10_1;             \
        PA.OTYPER |= GPIO_OTYPER_OT10;                                                \
        PA.AFR[1] = (PA.AFR[1] & ~GPIO_AFRH_AFSEL10) | (1u << GPIO_AFRH_AFSEL10_Pos); \
        PA.OSPEEDR = (PA.OSPEEDR & ~GPIO_OSPEEDR_OSPEED10) |                          \
                     ((OW_BUS_DRIVE & 0x3u) << GPIO_OSPEEDR_OSPEED10_Pos);            \
        T1.TISEL = (T1.TISEL & ~TIM_TISEL_TI4SEL_Msk) | (0u << TIM_TISEL_TI4SEL_Pos); \
    } while (0)

#endif /* OW_GPDMA_H */
