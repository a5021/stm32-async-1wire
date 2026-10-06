/**
 * @file ow_port_f4.h
 * @brief STM32F4 backend: thin shim over the shared TIM1+DMA core.
 *
 * The 1-Wire bus runs on PA10 (TIM1_CH3 PWM output in open-drain alternate
 * function AF1, or PE13 with -DOW_PORT_BUS_PE13=1); CH4 captures in indirect
 * mode on the same pin (CC4S routes IC4 to TI3) and drains CCR4 via
 * DMA2_Stream4, while the CC2 slot-end marker (DMA2_Stream2) reloads CCR3.
 *
 * This header owns only the F4 facts (ow_pulse_t, clocks, pin mux, DMA
 * stream assignment, the OW_PORT_DMA_xx overrides).  Everything else,
 * including the merged write+read pass, is the shared body in
 * port/common/ow_port_tim_dma.h, included at the end of this file once the
 * macros above are defined.
 *
 * HARDWARE CONTRACT (see the "Required Timer Capabilities" section of README):
 *  - DMA2 streams run in direct mode (no FIFO): the memory transfer width is
 *    forced to PSIZE. CCR3 is a halfword register, so the feed source must be
 *    halfword (ow_pulse_t = uint16_t on this family); the capture moves
 *    matching 16-bit halfwords (or reads CCR4 low - byte serves as an 8-bit
 *    capture path).
 *  - Each stream selects its own request with CHSEL=6 (Stream2 -> TIM1_CH2,
 *    Stream4 -> TIM1_CH4); the two request paths are independent.
 *  - GPIO follows the F0/G0 MODER/OTYPER/AFR style (no F1 CRH rule): PA10 is
 *    alternate-function open-drain with AFR=1 (TIM1_CH3); the parasite
 *    strong-pull-up toggles only the OTYPER bit.
 *
 * Bring-up notes and the alternatives that were tried and rejected:
 * see HARDWARE-NOTES.md in this directory.
 */

#ifndef OW_PORT_F4_H
#define OW_PORT_F4_H

#include "onewire.h"
#include "ow_bits.h"
#include "stm32f4xx.h"

/* ------------------------------------------------------------------
 *  Per-family facts owned by this port.  onewire.h delegates them here so the
 *  core headers carry no family knowledge; the token chain in onewire.h is the
 *  only other place a family appears.
 *  - ow_pulse_t: 16-bit feed entry.  The F4 backend feeds CCR3 in DMA direct
 *    mode, where the memory-transfer width is forced to PSIZE, so a feed entry
 *    is a 16-bit halfword here (zero-copy, CNDTR == slots); every other
 *    backend latches 8-bit entries.  Capture-side durations are always uint16_t.
 *  - OW_PORT_SYSCLK_MHZ: this family's default system clock, one fact per
 *    concrete part; the timer prescaler and the IC4F ladder derive from it.
 *    -DOW_PORT_SYSCLK_MHZ=N overrides it; validate any change against every
 *    supported clock (app.c).
 *  - OW_PORT_F4_MAX_SYSCLK_MHZ: the highest clock this part can take.  app.c
 *    rejects a build that asks for more, so a 180MHz request cannot silently
 *    compile for an F407 (whose cap is 168MHz without over-drive, and whose
 *    flash latency and APB limits differ above it).
 *  - OW_HSE_MHZ: external-crystal frequency, read by app.c as the PLL's M
 *    divider.  Deliberately separate from OW_PORT_SYSCLK_MHZ: the crystal
 *    belongs to the *board*, the system clock to the *application*.  A wrong
 *    value is not a compile error - the PLL simply never locks.
 *  - OW_PORT_TIM1_UPD_IRQn: TIM1 update IRQ for the low-power WFE path. */
typedef uint16_t ow_pulse_t;

#if !defined(OW_PORT_SYSCLK_MHZ)
#if defined(STM32F401xC) || defined(STM32F401xE)
#define OW_PORT_SYSCLK_MHZ 84 /* STM32F401: the part's 84MHz cap, see OW_HSE_MHZ */
#elif defined(STM32F446xx)
#define OW_PORT_SYSCLK_MHZ 180 /* STM32F446: HSE + PLL + over-drive, see OW_HSE_MHZ */
#else
#define OW_PORT_SYSCLK_MHZ 168 /* STM32F407: HSE + PLL, see OW_HSE_MHZ */
#endif
#endif

/* F4 TIM1 kernel-clock proof (why the shared core's PSC = SYSCLK_MHZ - 1
 * gives a 1us tick here): TIM1 is on APB2 (RM0090 §7), and when the APB
 * prescaler feeding it is != 1 the timer clock doubles to 2 x PCLK, which
 * configure_system_clock() relies on:
 *  - 168MHz (8MHz HSE+PLL, default): PPRE1=/4 (APB1=42MHz), PPRE2=/2
 *    (APB2=84MHz) — both at their datasheet limits — timer doubles to
 *    2 x 84 = 168MHz = SYSCLK.
 *  - 180MHz (STM32F446 only, HSE+PLL + over-drive): PPRE1=/4 and PPRE2=/2
 *    again, F446 limits 45/90MHz, so 2 x 90 = 180MHz = SYSCLK.
 *  - 16MHz (raw HSI) / 8MHz (raw HSE): both APB prescalers stay /1, so
 *    TIM1 = PCLK2 = SYSCLK directly.
 * (PPRE1=/4 only affects APB1 peripherals — TIM2..5, USART2/3/6, I2C —
 * none of which this port uses.) */

#if !defined(OW_PORT_F4_MAX_SYSCLK_MHZ)
#if defined(STM32F446xx)
#define OW_PORT_F4_MAX_SYSCLK_MHZ 180 /* F446: the only part here that needs the over-drive sequence */
#else
#define OW_PORT_F4_MAX_SYSCLK_MHZ 168 /* F405/F407 cap; the F401 parts are lower still, see below */
#endif
#endif

#if !defined(OW_HSE_MHZ)
#define OW_HSE_MHZ 8
#endif

#if OW_PORT_LOW_POWER
#define OW_PORT_TIM1_UPD_IRQn TIM1_UP_TIM10_IRQn
#endif

/* IC4F ladder and OC3PE knob: owned by the shared core (identical tiers),
 * which sees OW_PORT_SYSCLK_MHZ above before its own definitions. */

#include "ow_port.h"

/* ------------------------------------------------------------------
 *  1-Wire bus pin selection (TIM1_CH3 output + IC4 capture via TI3).
 *
 *  Default: PA10 (AF1). With -DOW_PORT_BUS_PE13=1 the bus moves to PE13
 *  (also TIM1_CH3, AF1) for boards where PA10 is loaded/unavailable.
 *  PA11 is left untouched for the application; logic-analyzer sync, if
 *  needed, goes through the opt-in OW_PORT_MARKER_TOGGLE() hook.
 * ------------------------------------------------------------------ */
#if defined(OW_PORT_BUS_PE13)
#define OW_BUS_GPIO (*GPIOE)
#define OW_BUS_GPIO_CLK RCC_BITS(AHB1ENR, GPIOEEN)
#define OW_BUS_MODER GPIO_MODER_MODER13
#define OW_BUS_MODER_1 GPIO_MODER_MODER13_1
#define OW_BUS_OT GPIO_OTYPER_OT_13
#define OW_BUS_OSPEEDR GPIO_OSPEEDR_OSPEED13
#define OW_BUS_OSPEEDR_Pos GPIO_OSPEEDR_OSPEED13_Pos
#define OW_BUS_AFSEL GPIO_AFRH_AFSEL13
#define OW_BUS_AFSEL_Pos GPIO_AFRH_AFSEL13_Pos
#else
#define OW_BUS_GPIO (*GPIOA)
#define OW_BUS_GPIO_CLK RCC_BITS(AHB1ENR, GPIOAEN)
#define OW_BUS_MODER GPIO_MODER_MODER10
#define OW_BUS_MODER_1 GPIO_MODER_MODER10_1
#define OW_BUS_OT GPIO_OTYPER_OT_10
#define OW_BUS_OSPEEDR GPIO_OSPEEDR_OSPEED10
#define OW_BUS_OSPEEDR_Pos GPIO_OSPEEDR_OSPEED10_Pos
#define OW_BUS_AFSEL GPIO_AFRH_AFSEL10
#define OW_BUS_AFSEL_Pos GPIO_AFRH_AFSEL10_Pos
#endif

/* Statement macro spelling of the pin-mode switch (same shape as the other
 * backends' OW_PORT_SET_PIN_MODE): the shared core will call this, the
 * function below delegates to it until the body moves to the core. */
#define OW_PORT_SET_PIN_MODE(push_pull)                                    \
    do {                                                                   \
        if (push_pull) {                                                   \
            OW_BUS_GPIO.OTYPER &= ~OW_BUS_OT; /* OD -> PP (strong HIGH) */ \
        } else {                                                           \
            OW_BUS_GPIO.OTYPER |= OW_BUS_OT; /* PP -> OD (release) */      \
        }                                                                  \
    } while (0)

/* TIM prescaler: owned by the shared core (same PSC = SYSCLK_MHZ - 1 plus
 * the width assert); the F4 APB2 clock-tree proof lives with the SYSCLK
 * defaults above. */

/* @brief DMA stream assignment (RM0368 §9.3.3, Table 29): each stream selects
 *       its request source with its own CHSEL field.  DMA2_Stream2 CHSEL=6
 *       carries TIM1_CH2 (slot-end CC2 event); DMA2_Stream4 CHSEL=6 carries
 *       TIM1_CH4 (input-capture CC4 event).  CHSEL=6 is the per-stream mux
 *       index, not a shared physical request line. */
#define OW_PORT_DMA_FEED (*DMA2_Stream2)
#define OW_PORT_DMA_CAPTURE (*DMA2_Stream4)
#define OW_PORT_DMA_CHSEL (6u << DMA_SxCR_CHSEL_Pos)

/* EN-bit spelling for the shared ow_port_dma_disable(), called by the
 * DISABLE macros above (bounded wait identical to the old rearm request
 * path). */
#define OW_PORT_DMA_EN_BIT DMA_SxCR_EN

/* @brief Capture-stream control bits: MINC = memory-increment, PSIZE_0 =
 *       16-bit peripheral read (CCR4), PL_1 = high priority.  Direct mode
 *       forces the memory width to PSIZE, so the caller also sets MSIZE_0
 *       (matching halfwords).  Note the F4 DIR field is two bits wide: the
 *       generic single-bit DMA_SxCR_DIR macro would write the reserved 0b11. */
#define OW_PORT_DMA_CR_CAPTURE DMA_SxCR(MINC, PSIZE_0, PL_1)

/* @brief Feed-stream control bits: DIR_0 = memory-to-peripheral, MINC =
 *       memory-increment, PSIZE_0 = 16-bit peripheral write (CCR3 is a
 *       halfword register), PL_1 = high priority.  The caller also sets
 *       MSIZE_0: direct mode forces the memory width to PSIZE, so the source
 *       must be a matching halfword (ow_pulse_t) buffer. */
#define OW_PORT_DMA_CR_FEED DMA_SxCR(DIR_0, MINC, PL_1, PSIZE_0)

/* Port-unification overrides for the OW_PORT_DMA_CR_* vocabulary (see the
 * defaults in port/common/ow_port_tim_dma.h): same three transfer classes
 * in stream spelling.  Each equals the per-site expression it will replace
 * (capture, read pair and merged capture share RX16; feed and merged feed
 * share TX) — pinned by
 * tests/test_dma_contract.c::test_dma_cr_value_macros. */
#define OW_PORT_DMA_CR_RX16 (OW_PORT_DMA_CR_CAPTURE | OW_PORT_DMA_CHSEL | DMA_SxCR(MSIZE_0, EN))
#define OW_PORT_DMA_CR_RX8 ((OW_PORT_DMA_CR_CAPTURE & ~DMA_SxCR(PSIZE_0)) | OW_PORT_DMA_CHSEL | DMA_SxCR(EN))
#define OW_PORT_DMA_CR_TX (OW_PORT_DMA_CR_FEED | OW_PORT_DMA_CHSEL | DMA_SxCR(MSIZE_0, EN))

/* Stream-spelling overrides for the DISABLE/PROG vocabulary (defaults in
 * port/common/ow_port_tim_dma.h).  DISABLE is disable + bounded EN wait
 * (shared ow_port_dma_disable(), same request path as the old rearm) plus
 * the per-stream flag retirement: Stream2 retires LIFCR, Stream4 HIFCR, all
 * five flags each so the next EN starts from a clean state.  No request
 * routing: CHSEL travels inside the CR word, so the core's OW_PORT_ROUTE_*
 * stay empty no-ops here. */
#define OW_PORT_DMA_DISABLE_FEED()                                     \
    do {                                                               \
        ow_port_dma_disable(&OW_PORT_DMA_FEED.CR);                     \
        D2.LIFCR = DMA_LIFCR(CFEIF2, CDMEIF2, CTEIF2, CHTIF2, CTCIF2); \
    } while (0)
#define OW_PORT_DMA_DISABLE_CAPTURE()                                  \
    do {                                                               \
        ow_port_dma_disable(&OW_PORT_DMA_CAPTURE.CR);                  \
        D2.HIFCR = DMA_HIFCR(CFEIF4, CDMEIF4, CTEIF4, CHTIF4, CTCIF4); \
    } while (0)
#define OW_PORT_DMA_PROG_CAPTURE(dst, count, cr)      \
    do {                                              \
        OW_PORT_DMA_CAPTURE.PAR = (uint32_t)&T1.CCR4; \
        OW_PORT_DMA_CAPTURE.M0AR = (uint32_t)(dst);   \
        OW_PORT_DMA_CAPTURE.NDTR = (count);           \
        OW_PORT_DMA_CAPTURE.CR = (cr);                \
    } while (0)
#define OW_PORT_DMA_PROG_FEED(src, count, cr)      \
    do {                                           \
        OW_PORT_DMA_FEED.PAR = (uint32_t)&T1.CCR3; \
        OW_PORT_DMA_FEED.M0AR = (uint32_t)(src);   \
        OW_PORT_DMA_FEED.NDTR = (count);           \
        OW_PORT_DMA_FEED.CR = (cr);                \
    } while (0)

/* Core-required init hooks: the shared ow_port_init() calls these.
 * Statements moved verbatim from the former F4 ow_port_init().
 * Bus pin: alternate function, open-drain, AF1 (TIM1_CH3).  Drive strength:
 * the strong pull-up is this pin in AF push-pull (TIM1_CH3 driven HIGH
 * while the timer is stopped), so its drive strength is the parasite supply
 * for the whole fleet: at the reset-default low speed a simultaneous
 * (broadcast) conversion of several devices droops the line into brown-out
 * (POR 85 C / garbage with valid CRC), while one device at a time still
 * converts fine.  Configurable via OW_BUS_DRIVE (default MAX = very-high). */
#define OW_PORT_ENABLE_BUS_CLOCKS()                                                                            \
    do {                                                                                                       \
        RC.AHB1ENR |= RCC_BITS(AHB1ENR, DMA2EN, GPIOAEN) | OW_BUS_GPIO_CLK; /* TIM1 requests route via DMA2 */ \
        RC.APB2ENR |= RCC_APB2ENR(TIM1EN);                                                                     \
    } while (0)
#define OW_PORT_CONFIG_BUS_PIN()                                                              \
    do {                                                                                      \
        OW_BUS_GPIO.MODER = (OW_BUS_GPIO.MODER & ~OW_BUS_MODER) | OW_BUS_MODER_1;             \
        OW_BUS_GPIO.OTYPER |= OW_BUS_OT;                                                      \
        OW_BUS_GPIO.AFR[1] = (OW_BUS_GPIO.AFR[1] & ~OW_BUS_AFSEL) | (1u << OW_BUS_AFSEL_Pos); \
        OW_BUS_GPIO.OSPEEDR = (OW_BUS_GPIO.OSPEEDR & ~OW_BUS_OSPEEDR) |                       \
                              ((OW_BUS_DRIVE & 0x3u) << OW_BUS_OSPEEDR_Pos);                  \
    } while (0)

/* The shared body. */
#include "ow_port_tim_dma.h"

#endif /* OW_PORT_F4_H */
