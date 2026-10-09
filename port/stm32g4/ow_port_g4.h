/* ============================================================
 *  ow_port_g4.h - STM32G4 backend
 *
 *  Family-specific half only.  The TIM1/DMA state machine is shared with the
 *  other five families and lives in port/common/ow_port_tim_dma.h.  G4 differs
 *  in four ways, all of them here: the clock gates (AHB1 for DMA+DMAMUX, AHB2
 *  for GPIO, APB2 for TIM1+SYSCFG), the AF number of TIM1_CH3 on PA10, the
 *  DMAMUX request IDs, and the default clock (HSE+PLL to 170MHz).
 * ============================================================ */

#ifndef OW_PORT_G4_H
#define OW_PORT_G4_H

#include "onewire.h"
#include "ow_bits.h"
#include "stm32g4xx.h"

/* ------------------------------------------------------------------
 *  Per-family facts owned by this port.  onewire.h delegates them here so the
 *  core headers carry no family knowledge; the token chain in onewire.h is the
 *  only other place a family appears.
 *  - ow_pulse_t: 8-bit feed entry.  The classic-DMA backends (F0/F1/F3/G0)
 *    feed CCR3 with a 16-bit peripheral read narrowed to byte memory (the
 *    RX8/TX defaults in the shared core), and the G4 DMA is the same classic
 *    IP behind a DMAMUX as G0's, so the same words apply here (contrast the F4
 *    backend, a uint16_t direct-mode feed on DMA2 streams).  Capture-side
 *    durations are always uint16_t.  Pending hardware proof: the first
 *    search-ROM matrix on the WeAct board is the experiment - F4's rejected
 *    byte feed (merged slots, all-zero ROMs, see port/stm32f4/HARDWARE-NOTES.md)
 *    is what a failure looks like.
 *  - OW_PORT_SYSCLK_MHZ: this family's default system clock (HSE+PLL, see
 *    app.c); -DOW_PORT_SYSCLK_MHZ=N overrides it, 16 selects raw HSI16.
 *  - OW_PORT_G4_MAX_SYSCLK_MHZ: the part ceiling; app.c rejects more.
 *  - OW_HSE_MHZ: the board crystal, read by app.c as the PLL's M divider.
 *  - OW_PORT_TIM1_UPD_IRQn: TIM1 update IRQ for the low-power WFE path
 *    (shared with TIM16 on this family). */
typedef uint8_t ow_pulse_t;

#if !defined(OW_PORT_SYSCLK_MHZ)
#define OW_PORT_SYSCLK_MHZ 170 /* STM32G474: HSE 8MHz + PLL (M=2, N=85, R=2) */
#endif

#if !defined(OW_PORT_G4_MAX_SYSCLK_MHZ)
#define OW_PORT_G4_MAX_SYSCLK_MHZ 170 /* G474 ceiling, voltage Range 1 Boost */
#endif

#if !defined(OW_HSE_MHZ)
#define OW_HSE_MHZ 8 /* WeAct STM32G474CBT6 Long: 8MHz crystal */
#endif

#if OW_PORT_LOW_POWER
#define OW_PORT_TIM1_UPD_IRQn TIM1_UP_TIM16_IRQn
#endif

#include "ow_port.h"

/* G4 TIM1 kernel-clock proof (why the shared core's PSC = SYSCLK_MHZ - 1
 * gives a 1us tick here): TIM1 is on APB2 (RM0440 §7), and
 * configure_system_clock() (app.c) leaves every prescaler at /1 on both
 * supported clocks - 170MHz (HSE+PLL) and 16MHz (raw HSI16) - so TIM1 =
 * PCLK2 = SYSCLK directly, with no x2 doubling.  (No APB peripheral this
 * port uses sits on APB1.) */

/* @brief Gate DMA1, DMAMUX1, GPIOA, SYSCFG and TIM1.
 *
 *  G4 splits the gates three ways: DMA1+DMAMUX1 on AHB1, GPIO on AHB2,
 *  TIM1+SYSCFG on APB2 (RM0440 §7).  The APB2 read-back is deliberate: the
 *  APB clock has only just been gated, so the write is flushed before the
 *  first SYSCFG/TIM1 access below.  (No pin remap on this port: PA9/PA10
 *  are bonded out on the LQFP48, unlike the G031 TSSOP20.) */
#define OW_PORT_ENABLE_BUS_CLOCKS()                                               \
    do {                                                                          \
        RC.AHB1ENR |= RCC_AHB1ENR_DMA1EN | RCC_AHB1ENR_DMAMUX1EN;                 \
        RC.AHB2ENR |= RCC_AHB2ENR_GPIOAEN;                                        \
        RC.APB2ENR |= RCC_APB2ENR_SYSCFGEN | RCC_APB2ENR_TIM1EN;                  \
        (void)RC.APB2ENR; /* settle the APB clock before the TIM1 access below */ \
    } while (0)

/* @brief PA10: alternate function, open-drain, AF6 (TIM1_CH3).
 *
 *  AF6 carries the advanced timers on this family (DS12288 Table 17:
 *  TIM1_CH3 on PA10); USART1_TX on PA9 is AF7, so the bus and the console
 *  share the port without touching each other. */
#define OW_PORT_CONFIG_BUS_PIN()                                                      \
    do {                                                                              \
        PA.MODER = (PA.MODER & ~GPIO_MODER_MODER10) | GPIO_MODER_MODER10_1;           \
        PA.OTYPER |= GPIO_OTYPER_OT_10;                                               \
        PA.AFR[1] = (PA.AFR[1] & ~GPIO_AFRH_AFSEL10) | (6u << GPIO_AFRH_AFSEL10_Pos); \
        /* Drive strength is configurable via OW_BUS_DRIVE, default MAX. */           \
        PA.OSPEEDR = (PA.OSPEEDR & ~GPIO_OSPEEDR_OSPEED10) |                          \
                     ((OW_BUS_DRIVE & 0x3u) << GPIO_OSPEEDR_OSPEED10_Pos);            \
    } while (0)

/* @brief Toggle the bus pin between open-drain and push-pull.
 *
 *  Push-pull is only used by the experimental active-drive write path
 *  (OW_DRIVE_ACTIVE); the slave has to be able to pull the line LOW while the
 *  master reads, so every read and reset phase returns to open-drain. */
#define OW_PORT_SET_PIN_MODE(push_pull)                                   \
    do {                                                                  \
        if (push_pull) {                                                  \
            PA.OTYPER &= ~GPIO_OTYPER_OT_10; /* OD -> PP (strong HIGH) */ \
        } else {                                                          \
            PA.OTYPER |= GPIO_OTYPER_OT_10; /* PP -> OD (release) */      \
        }                                                                 \
    } while (0)

/* @brief DMAMUX request selectors (RM0440 §12, DMA request mapping table;
 *  double-sourced from STM32CubeG4 stm32g4xx_hal_dma.h / stm32g4xx_ll_dmamux.h:
 *  TIM1_CH2 = 43, TIM1_CH4 = 45).
 *
 *  Like G0, this family has no fixed DMA request map: every DMA channel picks
 *  its peripheral source through DMAMUX.  These two pair channel 2 with the
 *  CC2 slot-end marker (the feed) and channel 3 with CH4 (the capture); the
 *  CC2 compare-match event raises the TIM1_CH2 request line, which is why the
 *  feed selector is the CH2 number (G0 reads 21 = CH2 the same way).  They are
 *  written before each operation rather than once at init so the routing
 *  cannot be left pointing at a previous request. */
#define OW_PORT_DMAMUX_REQ_TIM1_CC2 43u
#define OW_PORT_DMAMUX_REQ_TIM1_CH4 45u

#define OW_PORT_ROUTE_CAPTURE()                              \
    do {                                                     \
        DMAMUX1_Channel3->CCR = OW_PORT_DMAMUX_REQ_TIM1_CH4; \
    } while (0)
#define OW_PORT_ROUTE_FEED()                                 \
    do {                                                     \
        DMAMUX1_Channel2->CCR = OW_PORT_DMAMUX_REQ_TIM1_CC2; \
    } while (0)

/* @brief DMA channel assignment: feed rides DMAMUX channel 2 paired with
 *       DMA1_Channel3 (TIM1_CC2), capture rides DMAMUX channel 3 paired with
 *       DMA1_Channel4 (TIM1_CH4) - the core D13/D14 defaults; only the
 *       DMAMUX routing above is G4-specific. */

#include "ow_port_tim_dma.h"

#endif /* OW_PORT_G4_H */
