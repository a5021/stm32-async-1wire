/* ============================================================
 *  ow_port_f1.h - STM32F1 backend
 *
 *  Family-specific half only.  The TIM1/DMA1 state machine is shared with
 *  F0 and G0 and lives in port/common/ow_port_tim_dma.h; what is left here is
 *  the part that is genuinely an F103: which clocks to gate, and how PA10 is
 *  put into alternate-function open-drain mode.
 * ============================================================ */

#ifndef OW_PORT_F1_H
#define OW_PORT_F1_H

#include "onewire.h"
#include "ow_port.h"
#include "ow_bits.h"
#include "stm32f1xx.h"

/* @brief Gate GPIOA, TIM1 and DMA1.
 *
 *  F1 puts IOPAEN and TIM1EN in APB2ENR and DMA1EN in AHBENR; F0 uses the same
 *  two registers but names the bits GPIOAEN/DMAEN, and G0 uses IOPENR/APBENR2
 *  with a read-back to settle the APB clock before its first SYSCFG access.
 */
#define OW_PORT_ENABLE_BUS_CLOCKS()      \
    do {                                  \
        RC.APB2ENR |= RCC_APB2ENR(IOPAEN, TIM1EN); \
        RC.AHBENR |= RCC_AHBENR(DMA1EN);  \
    } while (0)

/* @brief PA10: alternate function, open-drain, 2 MHz (TIM1_CH3, default map).
 *
 *  F1 is the odd one out here: it has the legacy GPIO_CRH configuration field
 *  rather than the modern MODER/OTYPER/AFR split that F0 and G0 use, which is
 *  why this is a macro and not shared code.  The whole MODE10/CNF10 field is
 *  cleared first so the pin is configured correctly even if it was previously
 *  set to another mode.
 */
#define OW_PORT_CONFIG_BUS_PIN()                                          \
    do {                                                                  \
        PA.CRH = (PA.CRH & ~GPIO_CRH(MODE10, CNF10)) |                    \
                 GPIO_CRH(MODE10_1, CNF10_0, CNF10_1);                    \
    } while (0)

/* @brief Toggle the bus pin between open-drain and push-pull.
 *
 *  Rewrites the whole CNF10 field: CNF=10 is AF push-pull, CNF=11 is AF
 *  open-drain.  Masking the full field rather than toggling one bit makes the
 *  result independent of the previous CNF state.  Push-pull is only used by the
 *  experimental active-drive write path (OW_DRIVE_ACTIVE); the slave has to be
 *  able to pull the line LOW while the master reads, so every read and reset
 *  phase returns to open-drain.
 */
#define OW_PORT_SET_PIN_MODE(push_pull)   \
    do {                                  \
        PA.CRH = (PA.CRH & ~GPIO_CRH_CNF10) | \
                 (push_pull ? GPIO_CRH_CNF10_1 : GPIO_CRH_CNF10);         \
    } while (0)

/* @brief DMA request routing.
 *
 *  F1 has a fixed request map with no DMAMUX, so TIM1_CC2 -> channel 3 and
 *  CH4 -> channel 4 need no programming.  (Both mappings were confirmed
 *  empirically at board bring-up; USART1_TX shares channel 4's request line,
 *  which is harmless because the driver's UART output runs without DMA.)
 *  G0 does need these - see ow_port_g0.h.
 */
#define OW_PORT_ROUTE_CAPTURE() \
    do {                        \
    } while (0)
#define OW_PORT_ROUTE_FEED() \
    do {                     \
    } while (0)

/* @brief DMA channel assignment (fixed request map, verified on target):
 *       feed rides TIM1_CC2 -> channel 3, capture rides CH4 -> channel 4. */
#define OW_PORT_DMA_FEED D13
#define OW_PORT_DMA_CAPTURE D14

#include "ow_port_tim_dma.h"

#endif /* OW_PORT_F1_H */
