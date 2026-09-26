/* ============================================================
 *  ow_port_f0.h - STM32F0 backend
 *
 *  Family-specific half only.  The TIM1/DMA1 state machine is shared with
 *  F1 and G0 and lives in port/common/ow_port_tim_dma.h; what is left here is
 *  the part that is genuinely an F030: which clocks to gate, and how PA10 is
 *  put into alternate-function open-drain mode.
 * ============================================================ */

#ifndef OW_PORT_F0_H
#define OW_PORT_F0_H

#include "onewire.h"
#include "ow_port.h"
#include "ow_bits.h"
#include "stm32f0xx.h"

/* @brief Gate GPIOA, DMA1 and TIM1.
 *
 *  On F0 these live in AHBENR (GPIOAEN, DMAEN) and APB2ENR (TIM1EN); F1 uses
 *  IOPAEN/DMA1EN in the same pair, and G0 uses IOPENR/APBENR2 with a read-back
 *  to settle the APB clock before its first SYSCFG access.
 */
#define OW_PORT_ENABLE_BUS_CLOCKS()          \
    do {                                      \
        RC.AHBENR |= RCC_AHBENR(GPIOAEN, DMAEN); \
        RC.APB2ENR |= RCC_APB2ENR(TIM1EN);    \
    } while (0)

/* @brief PA10: alternate function, open-drain, AF2 (TIM1_CH3).
 *
 *  F0 has the modern GPIO register model, as G0 does; F1 configures the same
 *  pin through the legacy CRH field instead, which is why this is a macro
 *  rather than shared code.  The whole MODE/CNF field is cleared first so the
 *  pin lands in the right mode even if something set it before us.
 */
#define OW_PORT_CONFIG_BUS_PIN()                                    \
    do {                                                            \
        PA.MODER = (PA.MODER & ~GPIO_MODER_MODER10) | GPIO_MODER_MODER10_1; \
        PA.OTYPER |= GPIO_OTYPER_OT_10;                             \
        PA.AFR[1] = (PA.AFR[1] & ~GPIO_AFRH_AFSEL10) | (2u << GPIO_AFRH_AFSEL10_Pos); \
        /* Drive strength is configurable via OW_BUS_DRIVE, default MAX. */ \
        PA.OSPEEDR = (PA.OSPEEDR & ~GPIO_OSPEEDR_OSPEEDR10) |      \
                     ((OW_BUS_DRIVE & 0x3u) << GPIO_OSPEEDR_OSPEEDR10_Pos); \
    } while (0)

/* @brief Toggle the bus pin between open-drain and push-pull.
 *
 *  Push-pull is only used by the experimental active-drive write path
 *  (OW_DRIVE_ACTIVE); the slave has to be able to pull the line LOW while the
 *  master reads, so every read and reset phase returns to open-drain.
 */
#define OW_PORT_SET_PIN_MODE(push_pull)      \
    do {                                     \
        if (push_pull) {                     \
            PA.OTYPER &= ~GPIO_OTYPER_OT_10; /* OD -> PP (strong HIGH) */ \
        } else {                             \
            PA.OTYPER |= GPIO_OTYPER_OT_10;  /* PP -> OD (release) */ \
        }                                    \
    } while (0)

/* @brief DMA request routing.
 *
 *  F0 has a fixed request map with no DMAMUX, so TIM1_CC2 -> channel 3 and
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

/* @brief DMA channel assignment (fixed request map, verified at bring-up):
 *       channel 3 carries the CC2 slot-end marker request and feeds CCR3,
 *       channel 4 carries the CC4 capture request and drains CCR4. */
#define OW_PORT_DMA_FEED D13
#define OW_PORT_DMA_CAPTURE D14

#include "ow_port_tim_dma.h"

#endif /* OW_PORT_F0_H */
