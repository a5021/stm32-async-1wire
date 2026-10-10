/**
 * @file ow_port_h5.h
 * @brief STM32H5 backend: vocabulary overrides over the shared TIM1+GPDMA core.
 *
 * The 1-Wire bus runs on PA10 (TIM1_CH3 PWM output in open-drain alternate
 * function AF1); CH4 captures in indirect mode on the same pin (CC4S routes IC4
 * to TI3) and drains CCR4 via GPDMA1_Channel3, while the CC2 slot-end marker
 * (GPDMA1_Channel2) reloads CCR3.
 *
 * This header owns only the H5 facts (ow_pulse_t, clocks, pin mux, GPDMA
 * channel assignment, the CTR1 templates below).  Everything else, including
 * the merged write+read pass, is the shared body in
 * port/common/ow_port_tim_dma.h, included at the end of this file once the
 * macros above are defined.
 *
 * HARDWARE CONTRACT (see the "Required Timer Capabilities" section of README):
 *  - GPDMA1 channels run in direct register mode (no linked list): each
 *    channel is programmed through CTR1/CTR2/CBR1/CSAR/CDAR and started with
 *    CCR.EN.  Request routing is per-channel REQSEL in CTR2 (no DMAMUX on
 *    this family): Channel2 <- TIM1_CH2 (slot-end CC2 event, REQSEL 59),
 *    Channel3 <- TIM1_CH4 (input-capture CC4 event, REQSEL 61).  Double-
 *    sourced from the H5 reference manual GPDMA request-mapping table and
 *    STM32CubeH5 stm32h5xx_ll_dma.h (LL_GPDMA1_REQUEST_TIM1_CH2 = 59,
 *    LL_GPDMA1_REQUEST_TIM1_CH4 = 61): the CC2 compare-match event raises
 *    the TIM1_CH2 request line, which is why the feed selector is the CH2
 *    number.
 *  - GPDMA converts widths in hardware, so unlike the F4 direct-mode feed
 *    there is no width forcing here: feed entries stay 8-bit
 *    (ow_pulse_t = uint8_t, memory byte -> CCR3 halfword) and capture
 *    durations stay uint16_t.  The 8-bit capture path reads the CCR4 low
 *    byte, same as every other backend.
 *  - Feed is memory-to-peripheral (request driven by the destination, so
 *    CTR2.DREQ = 1); capture is peripheral-to-memory (request driven by the
 *    source, CTR2.DREQ = 0).  Single burst per request (BREQ = 0), no
 *    trigger (TRIGSEL = 0), no block repeat (BRC = 0).
 *  - GPIO follows the F0/G0 MODER/OTYPER/AFR style: PA10 is
 *    alternate-function open-drain with AFR=1 (TIM1_CH3, Arduino-variant
 *    PinMap confirmed); the parasite strong-pull-up toggles only the
 *    OTYPER bit.
 *
 * Bench-proven on the WeAct STM32H503CBT6 (ST-LINK, UART on COM5 at 115200):
 * all seven examples run correctly at the HSI-64 default (correct 22.8-22.9C
 * temperatures, 0 errors in 6_statistics, WFE sleep in 7_low_power), and the
 * 8MHz raw-HSE and 250MHz HSE+PLL1 clocks are proven by 2_device_search
 * (correct temperatures) and 1_basic (clean UART at the selected baud).  The
 * reset geometry, slot widths and feed reload timing are exercised by the same
 * suite at all three clocks.
 */

#ifndef OW_PORT_H5_H
#define OW_PORT_H5_H

#include "onewire.h"
#include "ow_bits.h"
#include "stm32h5xx.h"

/* ------------------------------------------------------------------
 *  Per-family facts owned by this port.  onewire.h delegates them here so the
 *  core headers carry no family knowledge; the token chain in onewire.h is the
 *  only other place a family appears.
 *  - ow_pulse_t: 8-bit feed entry.  GPDMA converts source/destination widths
 *    in hardware, so a byte memory cell feeding the halfword CCR3 needs no
 *    packing games (contrast the F4 backend, a uint16_t direct-mode feed on
 *    DMA2 streams).  Capture-side durations are always uint16_t.
 *  - OW_PORT_SYSCLK_MHZ: this family's default system clock (HSI 64MHz
 *    direct, see app.c); -DOW_PORT_SYSCLK_MHZ=N overrides it.  The two other
 *    clocks app.c implements for this family are the raw 8MHz HSE (N =
 *    OW_HSE_MHZ) and the HSE+PLL1 250MHz step (N = 250): both switch CFGR1.SW
 *    away from HSI, the 250 path raising VOS to SCALE0 and taking 5 flash
 *    wait states first.  All three are bench-validated and built by the host
 *    suite (the 64MHz default) and its 250/8MHz sibling executables.
 *  - OW_PORT_H5_MAX_SYSCLK_MHZ: the part ceiling (H503: 250MHz).
 *  - OW_HSE_MHZ: external-crystal frequency, read by app.c as the PLL's
 *    divider input.  Deliberately separate from OW_PORT_SYSCLK_MHZ: the
 *    crystal belongs to the *board*, the system clock to the *application*.
 *  - OW_PORT_TIM1_UPD_IRQn: TIM1 update IRQ for the low-power WFE path
 *    (split vector on this family: TIM1_UP, not the combined BRK/UP/TRG/COM
 *    of F1/F0/G0). */
typedef uint8_t ow_pulse_t;

#if !defined(OW_PORT_SYSCLK_MHZ)
#define OW_PORT_SYSCLK_MHZ 64 /* STM32H503: HSI 64MHz direct, no PLL */
#endif

#if !defined(OW_PORT_H5_MAX_SYSCLK_MHZ)
#define OW_PORT_H5_MAX_SYSCLK_MHZ 250 /* H503 ceiling */
#endif

#if !defined(OW_HSE_MHZ)
#define OW_HSE_MHZ 8 /* WeAct STM32H503Cx Core Board: 8MHz crystal (Y2) */
#endif

#if OW_PORT_LOW_POWER
#define OW_PORT_TIM1_UPD_IRQn TIM1_UP_IRQn
#endif

/* H5 TIM1 kernel-clock proof (why the shared core's PSC = SYSCLK_MHZ - 1
 * gives a 1us tick here): TIM1 is on APB2, and configure_system_clock()
 * (app.c) leaves every prescaler at /1 on all three supported clocks (64MHz
 * HSI, 8MHz raw HSE, 250MHz HSE+PLL1) — so TIM1 = PCLK2 = SYSCLK directly, with
 * no x2 doubling.  Bench proof for the tick is the LA slot-width matrix
 * (5/60/70us), same as every other family. */

#include "ow_port.h"

/* ------------------------------------------------------------------
 *  1-Wire bus pin: PA10 (TIM1_CH3 output + IC4 capture via TI3, AF1).
 *
 *  PA11 is left untouched for the application; logic-analyzer sync, if
 *  needed, goes through the opt-in OW_PORT_MARKER_TOGGLE() hook.
 * ------------------------------------------------------------------ */

/* Statement macro spelling of the pin-mode switch (same shape as the other
 * backends' OW_PORT_SET_PIN_MODE): the shared core calls this to toggle the
 * bus pin between open-drain and push-pull. */
#define OW_PORT_SET_PIN_MODE(push_pull)                                  \
    do {                                                                 \
        if (push_pull) {                                                 \
            PA.OTYPER &= ~GPIO_OTYPER_OT10; /* OD -> PP (strong HIGH) */ \
        } else {                                                         \
            PA.OTYPER |= GPIO_OTYPER_OT10; /* PP -> OD (release) */      \
        }                                                                \
    } while (0)

/* TIM prescaler: owned by the shared core (same PSC = SYSCLK_MHZ - 1 plus
 * the width assert); the H5 APB2 clock-tree proof lives with the SYSCLK
 * default above. */

/* @brief GPDMA channel assignment: feed rides GPDMA1_Channel2 (TIM1_CH2),
 *       capture rides GPDMA1_Channel3 (TIM1_CH4) — the core D13/D14 roles
 *       in GPDMA spelling. */
#define OW_PORT_DMA_FEED (*GPDMA1_Channel2)
#define OW_PORT_DMA_CAPTURE (*GPDMA1_Channel3)

/* EN-bit spelling for the shared ow_port_dma_disable(), called by the
 * DISABLE macros below (bounded wait identical to the classic-DMA path). */
#define OW_PORT_DMA_EN_BIT (1u << DMA_CCR_EN_Pos)

/* @brief CTR1 templates: the GPDMA transfer-register-1 word for each of the
 *       three transfer classes.  SDW/DDW_LOG2: 0 = byte, 1 = halfword.
 *       Capture reads the halfword CCR4 into incrementing memory (16- or
 *       8-bit cells); feed reads incrementing byte memory into the fixed
 *       halfword CCR3.  Pinned by
 *       tests/test/test_dma_contract.c::test_dma_cr_value_macros. */
#define OW_PORT_DMA_CR_RX16 \
    ((1u << DMA_CTR1_SDW_LOG2_Pos) | (1u << DMA_CTR1_DDW_LOG2_Pos) | (1u << DMA_CTR1_DINC_Pos))
#define OW_PORT_DMA_CR_RX8 \
    ((1u << DMA_CTR1_SDW_LOG2_Pos) | (1u << DMA_CTR1_DINC_Pos))
#define OW_PORT_DMA_CR_TX \
    ((1u << DMA_CTR1_SINC_Pos) | (1u << DMA_CTR1_DDW_LOG2_Pos))

/* @brief GPDMA request selectors (H5 RM GPDMA request-mapping table,
 *       double-sourced from STM32CubeH5 stm32h5xx_ll_dma.h:
 *       TIM1_CH2 = 59, TIM1_CH4 = 61).  Written as part of the CTR2 word in
 *       the PROG macros below, so the routing cannot be left pointing at a
 *       previous request; the core's OW_PORT_ROUTE_* stay empty no-ops here
 *       (same split as the F4 backend, where CHSEL travels in the CR word). */
#define OW_PORT_GPDMA_REQ_TIM1_CC2 59u
#define OW_PORT_GPDMA_REQ_TIM1_CH4 61u

/* GPDMA-spelling overrides for the DISABLE/PROG vocabulary (defaults in
 * port/common/ow_port_tim_dma.h).  DISABLE is disable + bounded EN wait
 * (shared ow_port_dma_disable(), same contract as the classic path) plus
 * flag retirement: TCF (transfer done) and SUSPF (our own mid-transfer
 * suspend on underrun re-arms).  Error flags are deliberately NOT retired
 * here so a misprogrammed channel stays visible instead of self-clearing. */
#define OW_PORT_DMA_DISABLE_FEED()                          \
    do {                                                    \
        ow_port_dma_disable(&OW_PORT_DMA_FEED.CCR);         \
        GPDMA1_Channel2->CFCR = (1u << DMA_CFCR_TCF_Pos) |  \
                                (1u << DMA_CFCR_SUSPF_Pos); \
    } while (0)
#define OW_PORT_DMA_DISABLE_CAPTURE()                       \
    do {                                                    \
        ow_port_dma_disable(&OW_PORT_DMA_CAPTURE.CCR);      \
        GPDMA1_Channel3->CFCR = (1u << DMA_CFCR_TCF_Pos) |  \
                                (1u << DMA_CFCR_SUSPF_Pos); \
    } while (0)
#define OW_PORT_DMA_PROG_CAPTURE(dst, count, cr)                             \
    do {                                                                     \
        OW_PORT_DMA_CAPTURE.CSAR = (uint32_t)&T1.CCR4;                       \
        OW_PORT_DMA_CAPTURE.CDAR = (uint32_t)(dst);                          \
        /* GPDMA CBR1.BNDT is a byte count (see stm32h503xx.h    \
         * DMA_CBR1_BNDT_Msk), unlike classic CNDTR which counts \
         * transfers: a halfword source (SDW_LOG2 = 1) consumes  \
         * 2 bytes per element, so BNDT = count * 2. RX8 keeps   \
         * the halfword CCR4 source (reading the low byte) and   \
         * needs the same x2; the feed below is byte-sourced     \
         * (SDW_LOG2 = 0) and stays BNDT = count bytes. */           \
        OW_PORT_DMA_CAPTURE.CBR1 =                                           \
            ((uint32_t)(count) << (((cr) >> DMA_CTR1_SDW_LOG2_Pos) & 0x3u)); \
        OW_PORT_DMA_CAPTURE.CTR2 =                                           \
            (OW_PORT_GPDMA_REQ_TIM1_CH4 << DMA_CTR2_REQSEL_Pos);             \
        OW_PORT_DMA_CAPTURE.CTR1 = (uint32_t)(cr);                           \
        OW_PORT_DMA_CAPTURE.CCR = (1u << DMA_CCR_EN_Pos);                    \
    } while (0)
#define OW_PORT_DMA_PROG_FEED(src, count, cr)                                \
    do {                                                                     \
        OW_PORT_DMA_FEED.CSAR = (uint32_t)(src);                             \
        OW_PORT_DMA_FEED.CDAR = (uint32_t)&T1.CCR3;                          \
        OW_PORT_DMA_FEED.CBR1 =                                              \
            ((uint32_t)(count) << (((cr) >> DMA_CTR1_SDW_LOG2_Pos) & 0x3u)); \
        OW_PORT_DMA_FEED.CTR2 =                                              \
            (OW_PORT_GPDMA_REQ_TIM1_CC2 << DMA_CTR2_REQSEL_Pos) |            \
            (1u << DMA_CTR2_DREQ_Pos);                                       \
        OW_PORT_DMA_FEED.CTR1 = (uint32_t)(cr);                              \
        OW_PORT_DMA_FEED.CCR = (1u << DMA_CCR_EN_Pos);                       \
    } while (0)

/* Core-required init hooks: bus clocks and the bus pin.
 * Bus pin: alternate function, open-drain, AF1 (TIM1_CH3, Arduino-variant
 * PinMap confirmed).  Console USART1_TX on PA9 is AF7, so the bus and the
 * console share the port without touching each other.  Drive strength:
 * configurable via OW_BUS_DRIVE (default MAX). */
#define OW_PORT_ENABLE_BUS_CLOCKS()                                                 \
    do {                                                                            \
        RC.AHB1ENR |= RCC_AHB1ENR_GPDMA1EN; /* GPDMA1 request routing + channels */ \
        RC.AHB2ENR |= RCC_AHB2ENR_GPIOAEN;                                          \
        RC.APB2ENR |= RCC_APB2ENR_TIM1EN;                                           \
        (void)RC.APB2ENR; /* settle the APB clock before the TIM1 access below */   \
    } while (0)
#define OW_PORT_CONFIG_BUS_PIN()                                                      \
    do {                                                                              \
        PA.MODER = (PA.MODER & ~GPIO_MODER_MODE10) | GPIO_MODER_MODE10_1;             \
        PA.OTYPER |= GPIO_OTYPER_OT10;                                                \
        PA.AFR[1] = (PA.AFR[1] & ~GPIO_AFRH_AFSEL10) | (1u << GPIO_AFRH_AFSEL10_Pos); \
        PA.OSPEEDR = (PA.OSPEEDR & ~GPIO_OSPEEDR_OSPEED10) |                          \
                     ((OW_BUS_DRIVE & 0x3u) << GPIO_OSPEEDR_OSPEED10_Pos);            \
        /* Route TI4 to TIM1_CH4 (direct pin) so that CC4 capture sees the bus */     \
        T1.TISEL = (T1.TISEL & ~TIM_TISEL_TI4SEL_Msk) | (0u << TIM_TISEL_TI4SEL_Pos); \
    } while (0)

/* The shared body. */
#include "ow_port_tim_dma.h"

#endif /* OW_PORT_H5_H */
