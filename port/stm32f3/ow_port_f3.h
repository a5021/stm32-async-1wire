/* ============================================================
 *  ow_port_f3.h - STM32F3 backend
 *
 *  Family-specific half only.  The TIM1/DMA1 state machine is shared with
 *  F0, F1 and G0 and lives in port/common/ow_port_tim_dma.h; what is left
 *  here is the part that is genuinely an F303: which clocks to gate, and how
 *  PA10 is put into alternate-function open-drain mode.
 *
 *  Verified against the STM32F3-DISCOVERY (MB1035B, STM32F303VC), 7 DS18B20
 *  in parasite power on one bus, at 8, 64 and 72 MHz.
 * ============================================================ */

#ifndef OW_PORT_F3_H
#define OW_PORT_F3_H

#include "onewire.h"
#include "ow_bits.h"
#include "stm32f3xx.h"

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
 *  - OW_HSE_MHZ: external-crystal frequency, read by app.c as the PLL input
 *    (this family has no HSI16, so the 72MHz PLL multiplies the crystal by 9).
 *    Deliberately separate from OW_PORT_SYSCLK_MHZ: the crystal belongs to the
 *    *board*, the system clock to the *application*.
 *  - OW_PORT_TIM1_UPD_IRQn: TIM1 update IRQ for the low-power WFE path. */
typedef uint8_t ow_pulse_t;

#if !defined(OW_PORT_SYSCLK_MHZ)
#define OW_PORT_SYSCLK_MHZ 72 /* STM32F303: the part's 72MHz ceiling, HSE/PREDIV + PLL x9 */
#endif

#if !defined(OW_HSE_MHZ)
#define OW_HSE_MHZ 8
#endif

#if OW_PORT_LOW_POWER
#define OW_PORT_TIM1_UPD_IRQn TIM1_UP_TIM16_IRQn
#endif

#include "ow_port.h"

/* @brief Gate GPIOA, GPIOE, DMA1 and TIM1.
 *
 *  F3 puts every GPIO port *and* DMA1 in AHBENR - there is no AHB2ENR on this
 *  family, so the bus-pin port and the DMA that moves its captures are gated
 *  from the same register. F0 uses the same pair (GPIOAEN, DMAEN under a
 *  different DMA bit name); F1 uses APB2ENR/AHBENR with IOPAEN; G0 splits the
 *  gates across IOPENR / AHBENR / APBENR2.
 *
 *  GPIOE is gated here rather than in app.c because it carries the board's
 *  LED, and the port layer is the only place that already names this register
 *  pair for this family.
 */
#define OW_PORT_ENABLE_BUS_CLOCKS()                        \
    do {                                                   \
        RC.AHBENR |= RCC_AHBENR(GPIOAEN, GPIOEEN, DMA1EN); \
        RC.APB2ENR |= RCC_APB2ENR(TIM1EN);                 \
    } while (0)

/* @brief PA10: alternate function, open-drain, AF6 (TIM1_CH3).
 *
 *  Same register model as F0/G0/F4, but three of the macro *names* differ from
 *  every other backend here, which is why this is a macro rather than shared
 *  code and why the spellings below are the F3 CMSIS ones (checked against
 *  CMSIS/device/stm32f303xc.h by tests/check_mock_headers.sh):
 *
 *    - the drive-strength field is GPIO_OSPEEDER_..., with an E. There is no
 *      GPIO_OSPEEDR_* in this header at all.
 *    - the alternate-function field is named by its *position* in AFR[1] rather
 *      than by pin number: only AFRH_AFRH0..7 and AFRL_AFRL0..7 exist, so PA10
 *      is AFRH_AFRH2 (bits [11:8]) and there is no GPIO_AFRH_AFSEL10 to write.
 *    - MODER/OTYPER are spelled the long way, as on F0/F4: MODER10, OT_10.
 *
 *  The whole MODE field is cleared first so the pin lands in the right mode
 *  even if something set it before us.
 */
#define OW_PORT_CONFIG_BUS_PIN()                                                  \
    do {                                                                          \
        PA.MODER = (PA.MODER & ~GPIO_MODER_MODER10) | GPIO_MODER_MODER10_1;       \
        PA.OTYPER |= GPIO_OTYPER_OT_10;                                           \
        PA.AFR[1] = (PA.AFR[1] & ~GPIO_AFRH_AFRH2) | (6u << GPIO_AFRH_AFRH2_Pos); \
        /* Drive strength is configurable via OW_BUS_DRIVE, default MAX. */       \
        PA.OSPEEDR = (PA.OSPEEDR & ~GPIO_OSPEEDER_OSPEEDR10) |                    \
                     ((OW_BUS_DRIVE & 0x3u) << GPIO_OSPEEDER_OSPEEDR10_Pos);      \
    } while (0)

/* @brief Toggle the bus pin between open-drain and push-pull.
 *
 *  Push-pull is only used by the parasite strong pull-up and, when
 *  OW_DRIVE_ACTIVE is defined, by the active-drive write path; the slave has
 *  to be able to pull the line LOW while the master reads, so every read and
 *  reset phase returns to open-drain.
 */
#define OW_PORT_SET_PIN_MODE(push_pull)                                   \
    do {                                                                  \
        if (push_pull) {                                                  \
            PA.OTYPER &= ~GPIO_OTYPER_OT_10; /* OD -> PP (strong HIGH) */ \
        } else {                                                          \
            PA.OTYPER |= GPIO_OTYPER_OT_10; /* PP -> OD (release) */      \
        }                                                                 \
    } while (0)

/* @brief DMA request routing.
 *
 *  F3 has a fixed request map with no DMAMUX, so like F0 and F1 there is
 *  nothing to program. The channel numbers below are not the F0/F1 ones - see
 *  the assignment comment.
 */
#define OW_PORT_ROUTE_CAPTURE() \
    do {                        \
    } while (0)
#define OW_PORT_ROUTE_FEED() \
    do {                     \
    } while (0)

/* @brief DMA channel assignment, from RM0316 Table 78 (STM32F303xB/C/D/E,
 *        STM32F358xC and STM32F398xE summary of DMA1 requests for each
 *        channel). The TIM1 row carries CC2 on channel 3 and CH4 on
 *        channel 4 - the same arrangement as F0/F1 (D13/D14), verified on
 *        the bench: all seven DS18B20 enumerate with valid CRCs.
 *
 *        Channel 4 also carries USART1_TX, and channel 3 carries USART3_TX.
 *        Neither collides here: this driver moves no UART bytes by DMA, it
 *        polls USART1->ISR and writes USART1->TDR, so the shared request line
 *        is never enabled. Same situation as F1, where USART1_TX shares
 *        channel 4 with nothing the port uses but the capture.
 */
#define OW_PORT_DMA_FEED D13 /* DMA1_Channel3: TIM1_CC2 slot-end marker -> feeds CCR3 */
#define OW_PORT_DMA_CAPTURE D14 /* DMA1_Channel4: TIM1_CH4 capture -> drains CCR4 */

#include "ow_port_tim_dma.h"

#endif /* OW_PORT_F3_H */
