/* ============================================================
 *  ow_port_f0.h - STM32F0 backend
 *
 *  Family-specific half only.  The TIM1/DMA state machine is shared with the
 *  other four families and lives in port/common/ow_port_tim_dma.h; what is
 *  left here is the part that is genuinely an F030: which clocks to gate,
 *  the PA10 pin-mux tokens, and the DMA assignment note.
 * ============================================================ */

#ifndef OW_PORT_F0_H
#define OW_PORT_F0_H

#include "onewire.h"
#include "ow_bits.h"
#include "stm32f0xx.h"

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
#define OW_PORT_SYSCLK_MHZ 48 /* STM32F030: HSI/2 + PLL x12 */
#endif

#if OW_PORT_LOW_POWER
#define OW_PORT_TIM1_UPD_IRQn TIM1_BRK_UP_TRG_COM_IRQn
#endif

#include "ow_port.h"

/* @brief Gate GPIOA, DMA1 and TIM1.
 *
 *  On F0 these live in AHBENR (GPIOAEN, DMAEN) and APB2ENR (TIM1EN); F1 uses
 *  IOPAEN/DMA1EN in the same pair, and G0 uses IOPENR/APBENR2 with a read-back
 *  to settle the APB clock before its first SYSCFG access.
 */
#define OW_PORT_ENABLE_BUS_CLOCKS()              \
    do {                                         \
        RC.AHBENR |= RCC_AHBENR(GPIOAEN, DMAEN); \
        RC.APB2ENR |= RCC_APB2ENR(TIM1EN);       \
    } while (0)

/* @brief PA10 pin-mux tokens for the shared defaults in ow_port_tim_dma.h:
 *       alternate function, open-drain, AF2 (TIM1_CH3).
 *
 *  F0 has the modern GPIO register model, as G0 does; F1 configures the same
 *  pin through the legacy CRH field instead (and keeps its own macros), which
 *  is why the shared defaults assemble tokens rather than spelling registers.
 *  The whole MODE field is cleared first so the pin lands in the right mode
 *  even if something set it before us.
 */
#define OW_PORT_MODER_MASK GPIO_MODER_MODER10
#define OW_PORT_MODER_AF GPIO_MODER_MODER10_1
#define OW_PORT_OT_BIT GPIO_OTYPER_OT_10
#define OW_PORT_AF_MASK GPIO_AFRH_AFSEL10
#define OW_PORT_AF_POS GPIO_AFRH_AFSEL10_Pos
#define OW_PORT_BUS_AF 2u
#define OW_PORT_OSPEED_MASK GPIO_OSPEEDR_OSPEEDR10
#define OW_PORT_OSPEED_POS GPIO_OSPEEDR_OSPEEDR10_Pos

/* @brief DMA channel assignment (fixed request map, verified at bring-up):
 *       channel 3 carries the CC2 slot-end marker request and feeds CCR3,
 *       channel 4 carries the CC4 capture request and drains CCR4 - the core
 *       D13/D14 defaults.  Request routing needs no programming here;
 *       USART1_TX shares channel 4's request line, which is harmless because
 *       the driver's UART output runs without DMA. */

#include "ow_port_tim_dma.h"

#endif /* OW_PORT_F0_H */
