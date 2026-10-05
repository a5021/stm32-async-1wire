/* ============================================================
 *  ow_port_g0.h - STM32G0 backend
 *
 *  Family-specific half only.  The TIM1/DMA state machine is shared with the
 *  other four families and lives in port/common/ow_port_tim_dma.h.  G0 differs
 *  in three ways, all of them here: the bus pins are remapped, the
 *  GPIO register macros have a different spelling, and DMA requests go through
 *  DMAMUX instead of a fixed map.
 * ============================================================ */

#ifndef OW_PORT_G0_H
#define OW_PORT_G0_H

#include "onewire.h"
#include "ow_bits.h"
#include "stm32g0xx.h"

/* ------------------------------------------------------------------
 *  Per-family facts owned by this port.  onewire.h delegates them here so the
 *  core headers carry no family knowledge; the token chain in onewire.h is the
 *  only other place a family appears.
 *  - ow_pulse_t: 8-bit feed entry.  The basic-DMA backends feed CCR3 with a
 *    peripheral read (PSIZE=16) widened to the byte entries, so pulse buffers
 *    are uint8_t here (contrast the F4 backend, a uint16_t direct-mode feed).
 *    Capture-side durations are always uint16_t.
 *  - OW_PORT_SYSCLK_MHZ: this family's default system clock; the timer
 *    prescaler and the IC4F ladder derive from it.  -DOW_PORT_SYSCLK_MHZ=N
 *    overrides it; validate any change against every supported clock (app.c).
 *  - OW_PORT_TIM1_UPD_IRQn: TIM1 update IRQ for the low-power WFE path. */
typedef uint8_t ow_pulse_t;

#if !defined(OW_PORT_SYSCLK_MHZ)
#define OW_PORT_SYSCLK_MHZ 64 /* STM32G031: HSI16 + PLL */
#endif

#if OW_PORT_LOW_POWER
#define OW_PORT_TIM1_UPD_IRQn TIM1_BRK_UP_TRG_COM_IRQn
#endif

#include "ow_port.h"

/* @brief Gate DMA1, GPIOA, SYSCFG and TIM1, then remap the bus pads.
 *
 *  G0 splits the clock gates across IOPENR / AHBENR / APBENR2 rather than the
 *  APB2ENR/AHBENR pair F0 and F1 use.  The read-back of APBENR2 is deliberate:
 *  the APB clock has only just been gated, and the SYSCFG access below needs it
 *  settled, so the write is flushed before the first SYSCFG register read.
 *
 *  PA9/PA10 are not bonded out on the G031 TSSOP20.  SYSCFG_CFGR1.PA11_RMP and
 *  PA12_RMP make the PA11/PA12 pads operate as PA9/PA10 (RM0444 section 8.1.1),
 *  so everything below that says "logical PA10" is physical PA12.
 */
#define OW_PORT_ENABLE_BUS_CLOCKS()                                                 \
    do {                                                                            \
        RC.AHBENR |= RCC_AHBENR_DMA1EN;                                             \
        RC.IOPENR |= RCC_IOPENR_GPIOAEN;                                            \
        RC.APBENR2 |= RCC_APBENR2_SYSCFGEN | RCC_APBENR2_TIM1EN;                    \
        (void)RC.APBENR2; /* settle the APB clock before the SYSCFG access below */ \
        SYSCFG->CFGR1 |= SYSCFG_CFGR1_PA11_RMP | SYSCFG_CFGR1_PA12_RMP;             \
    } while (0)

/* @brief Logical PA10 pin-mux tokens for the shared defaults in
 *       ow_port_tim_dma.h: alternate function, open-drain, AF2 (TIM1_CH3).
 *
 *  Same register model as F0; the macro names differ because the G0 CMSIS
 *  spells the MODER fields without the R (MODE10, not MODER10) and drops the
 *  _R infix in OTYPER/OSPEEDR.  The values are the same pins.
 */
#define OW_PORT_MODER_MASK GPIO_MODER_MODE10
#define OW_PORT_MODER_AF GPIO_MODER_MODE10_1
#define OW_PORT_OT_BIT GPIO_OTYPER_OT10
#define OW_PORT_AF_MASK GPIO_AFRH_AFSEL10
#define OW_PORT_AF_POS GPIO_AFRH_AFSEL10_Pos
#define OW_PORT_BUS_AF 2u
#define OW_PORT_OSPEED_MASK GPIO_OSPEEDR_OSPEED10
#define OW_PORT_OSPEED_POS GPIO_OSPEEDR_OSPEED10_Pos

/* @brief DMAMUX request selectors (RM0444 Table 42).
 *
 *  G0 has no fixed DMA request map: every DMA channel picks its peripheral
 *  source through DMAMUX.  These two pair channel 2 with the CC2 slot-end
 *  marker (the feed) and channel 3 with CH4 (the capture); both were confirmed
 *  on a G031F6P6 at bring-up.  They are written before each operation rather
 *  than once at init so the routing cannot be left pointing at a previous
 *  request. */
#define OW_PORT_DMAMUX_REQ_TIM1_CC2 21u
#define OW_PORT_DMAMUX_REQ_TIM1_CH4 23u

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
 *       DMAMUX routing above is G0-specific. */

#include "ow_port_tim_dma.h"

#endif /* OW_PORT_G0_H */
