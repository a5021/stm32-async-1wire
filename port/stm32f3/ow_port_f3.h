/* ============================================================
 *  ow_port_f3.h - STM32F3 backend
 *
 *  Family-specific half only.  The TIM1/DMA state machine is shared with the
 *  other four families and lives in port/common/ow_port_tim_dma.h; what is
 *  left here is the part that is genuinely an F303: which clocks to gate,
 *  the PA10 pin-mux tokens, and the DMA assignment note.
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

/* @brief PA10 pin-mux tokens for the shared defaults in ow_port_tim_dma.h:
 *       alternate function, open-drain, AF6 (TIM1_CH3).
 *
 *  Same register model as F0/G0, but three of the macro *names* differ from
 *  every other backend here, which is why the values below are tokens rather
 *  than shared code (spellings checked against CMSIS/device/stm32f303xc.h by
 *  tests/check_mock_headers.sh):
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
#define OW_PORT_MODER_MASK GPIO_MODER_MODER10
#define OW_PORT_MODER_AF GPIO_MODER_MODER10_1
#define OW_PORT_OT_BIT GPIO_OTYPER_OT_10
#define OW_PORT_AF_MASK GPIO_AFRH_AFRH2
#define OW_PORT_AF_POS GPIO_AFRH_AFRH2_Pos
#define OW_PORT_BUS_AF 6u
#define OW_PORT_OSPEED_MASK GPIO_OSPEEDER_OSPEEDR10
#define OW_PORT_OSPEED_POS GPIO_OSPEEDER_OSPEEDR10_Pos

/* @brief DMA channel assignment, from RM0316 Table 78 (STM32F303xB/C/D/E,
 *        STM32F358xC and STM32F398xE summary of DMA1 requests for each
 *        channel). The TIM1 row carries CC2 on channel 3 and CH4 on
 *        channel 4 - the same arrangement as F0/F1 (the core D13/D14
 *        defaults), verified on the bench: all seven DS18B20 enumerate with
 *        valid CRCs.
 *
 *        Channel 4 also carries USART1_TX, and channel 3 carries USART3_TX.
 *        Neither collides here: this driver moves no UART bytes by DMA, it
 *        polls USART1->ISR and writes USART1->TDR, so the shared request line
 *        is never enabled. Same situation as F1, where USART1_TX shares
 *        channel 4 with nothing the port uses but the capture.
 */

#include "ow_port_tim_dma.h"

#endif /* OW_PORT_F3_H */
