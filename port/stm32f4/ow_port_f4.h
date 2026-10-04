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
 * stream assignment, the OW_PORT_DMA_xx and OW_PORT_ROUTE_xx overrides)
 * plus two functions of its own - ow_port_dma_rearm() (stream disable and
 * flag clear) and ow_port_write_then_read() (UG placement carve-out,
 * see its note).
 * Everything else is the shared body in port/common/ow_port_tim_dma.h,
 * included at the end of this file once the macros above are defined.
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

/* EN-bit spelling for the shared ow_port_dma_disable() (compiled but never
 * called on F4 — the DISABLE macros above route to ow_port_dma_rearm). */
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
 * port/common/ow_port_tim_dma.h).  No request routing: CHSEL travels inside
 * the CR word, so the core's OW_PORT_ROUTE_* stay empty no-ops here. */
#define OW_PORT_DMA_DISABLE_CAPTURE() ow_port_dma_rearm(DMA2_Stream4)
#define OW_PORT_DMA_DISABLE_FEED() ow_port_dma_rearm(DMA2_Stream2)
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
#define OW_PORT_ROUTE_CAPTURE() ((void)0)
#define OW_PORT_ROUTE_FEED() ((void)0)

/* Forward declaration: the DISABLE macros above expand inside the shared core
 * below, which is included before the rearm definition that follows it. */
__STATIC_FORCEINLINE void ow_port_dma_rearm(DMA_Stream_TypeDef* stream);

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

/* The shared body.  This shim keeps its own merged pass (UG placement plus
 * the marker hook differ — see OW_PORT_OWN_WRITE_THEN_READ), so the core
 * skips its write_then_read.  After the include: the rearm definition and
 * the F4 write_then_read. */
#define OW_PORT_OWN_WRITE_THEN_READ 1
#include "ow_port_tim_dma.h"

/* @brief Disable a DMA stream and retire all its status flags before re-arm
 * @note Both call sites only ever pass Stream2 (feed) or Stream4 (capture);
 *       the flag register is derived from the pointer. Clears all five flags
 *       (TC/HT/TE/DME/FE) so the next EN starts from a clean state —
 *       auto-recovery in case a stale error flag would otherwise silently
 *       corrupt the next operation.
 *
 *       EN acknowledgement: stm32F4 clears EN by hardware only at the end of a
 *       transfer, so a stream that never drained its counter takes CR=0 as a
 *       disable *request* and keeps EN latched for a while. That happens for
 *       exactly one software path here: the capture stream after a no-presence
 *       reset (2 transfers armed, only the master-release edge arrives → NDTR
 *       2→1, EN never retires). Reprogramming PAR/M0AR/NDTR while EN is still
 *       set is outside the RM0090 §9.3.9 programming model, so the disable is
 *       acknowledged with a bounded wait on CR.EN instead of the bare CR=0 the
 *       historical code used. On every healthy path — the previous transfer
 *       drained, so EN is already 0 when this runs — the wait is zero
 *       iterations (the EN readback check exits immediately). The host model
 *       cannot distinguish this from the bare CR=0 (no disable latency, no
 *       register-access accessors); those tests document the contract and the
 *       real-hardware bench proves the wait.
 */
__STATIC_FORCEINLINE void ow_port_dma_rearm(DMA_Stream_TypeDef* stream) {
    stream->CR = 0; /* request disable */
    if (stream->CR & DMA_SxCR_EN) {
        /* EN still latched (e.g. the capture underrun above): bounded wait for
         * the stream to retire the disable before the reprogram writes below. */
        uint32_t guard = 1000u;
        while ((stream->CR & DMA_SxCR_EN) != 0u && guard-- != 0u) {
        }
    }
    if (stream == DMA2_Stream2) {
        D2.LIFCR = DMA_LIFCR(CFEIF2, CDMEIF2, CTEIF2, CHTIF2, CTCIF2);
    } else {
        D2.HIFCR = DMA_HIFCR(CFEIF4, CDMEIF4, CTEIF4, CHTIF4, CTCIF4);
    }
}

/**
 * @brief Merge the direction-bit write with the id/cmp read pair in one pass
 * @note Deliberate carve-out from the shared core (OW_PORT_OWN_WRITE_THEN_READ):
 *       the core programs DMA after UG, this programs it before (see below).
 * @param[in] bit Direction bit to write in slot 1 (0 or 1)
 * @param[in] pulse3 Buffer for the three captured slots (write-slot capture,
 *                   id pulse, cmp pulse)
 * @param[in] read_pulse CCR3 reloads for read slots 2-3 (+ trailing ONEWIRE_RELEASE_PULSE)
 * @note Single timer pass (RCR=2, three slots) with two DMA2 streams armed
 *       together. The CC2 slot-end marker (Stream2; CCR2 = ONE+ZERO, frozen
 *       output) reloads CCR3 from read_pulse —
 *       ONEWIRE_ONE_PULSE for slots 2-3, then ONEWIRE_RELEASE_PULSE during slot
 *       3 so the one-pulse
 *       timer stops with the line released to idle HIGH — while CC4 (Stream4)
 *       captures the write-slot pulse plus the id/cmp pair into pulse3[0..2].
 *       OC3PE is off so the reload is immediate. DMA requests stay disconnected
 *       (DIER=UIE only) through the re-arm kick so a stale CC2 request can
 *       never fire the reload early; they are re-connected right before CEN
 *       with the direction pulse re-armed last.
 */
__STATIC_FORCEINLINE void ow_port_write_then_read(uint8_t bit, volatile uint16_t* pulse3,
                                                  const ow_pulse_t* read_pulse) {
#if OW_DRIVE_ACTIVE
    ow_port_set_pin_mode(0); /* merged write+read stays open-drain so the read half is safe */
#endif
    const ow_pulse_t write_pulse = bit ? ONEWIRE_ONE_PULSE : ONEWIRE_ZERO_PULSE;
    OW_PORT_MARKER_TOGGLE(); /* opt-in LA hook (no-op by default): merged pass starts here */
    /* Clear TCIF status on both streams before reprogram. */
    ow_port_dma_rearm(DMA2_Stream2);
    ow_port_dma_rearm(DMA2_Stream4);
    /* Arrange the timer pass: three slots, then a single update event. CC4 is
     * armed for the whole pass, so the write-slot falling edge is captured
     * into pulse3[0] as well as the id/cmp reads into [1] and [2]. */
    T1.RCR = 2;
    T1.ARR = ONEWIRE_ONE_PULSE + ONEWIRE_ZERO_PULSE + ONEWIRE_GUARD_BAND; /* Total bit slot time */
    T1.CCR2 = ONEWIRE_ONE_PULSE + ONEWIRE_ZERO_PULSE; /* End-of-slot reload trigger */
    /* OC3 in PWM mode (no preload so the DMA reload is immediate), CC4 capture armed */
    T1.CCMR2 = TIM_CCMR2(OC3M_0, OC3M_1, OC3M_2, CC4S_1, OW_PORT_IC4F_ARGS);
    T1.CCER = TIM_CCER(CC3E, CC4E);
    /* Keep the DMA requests disconnected while the channels re-arm. */
#if OW_PORT_LOW_POWER
    T1.DIER = TIM_DIER(UIE); /* keep UIE for WFE while the DMA requests are apart */
#else
    T1.DIER = 0;
#endif
    /* Capture stream: write-slot capture plus the id/cmp pulse pair. */
    OW_PORT_DMA_CAPTURE.PAR = (uint32_t)&T1.CCR4;
    OW_PORT_DMA_CAPTURE.M0AR = (uint32_t)pulse3;
    OW_PORT_DMA_CAPTURE.NDTR = 3;
    OW_PORT_DMA_CAPTURE.CR = OW_PORT_DMA_CR_CAPTURE | OW_PORT_DMA_CHSEL |
                             DMA_SxCR(MSIZE_0, EN);
    /* Feed stream: halfword-width reloads of CCR3 — read pulse for slots 2-3,
     * then the trailing ONEWIRE_RELEASE_PULSE during slot 3 so the OPM stop
     * hands the line back
     * idle HIGH (hardware bus release); fed zero-copy from read_pulse. */
    OW_PORT_DMA_FEED.PAR = (uint32_t)&T1.CCR3;
    OW_PORT_DMA_FEED.M0AR = (uint32_t)read_pulse;
    OW_PORT_DMA_FEED.NDTR = 3;
    OW_PORT_DMA_FEED.CR = OW_PORT_DMA_CR_FEED | OW_PORT_DMA_CHSEL |
                          DMA_SxCR(MSIZE_0, EN);
    /* UG kick: reload ARR/RCR preloads and hand the fresh operation a clean
     * UIF, then re-connect the DMA requests and start. The direction pulse is
     * re-armed after the DIER write so a stale CC2 request can never clobber
     * it before the timer runs. */
    ow_port_update_event();
#if OW_PORT_LOW_POWER
    T1.DIER = TIM_DIER(CC4DE, CC2DE, UIE); /* Capture + CCR3 reload via DMA (UIE for WFE) */
#else
    T1.DIER = TIM_DIER(CC4DE, CC2DE); /* Capture + CCR3 reload via DMA */
#endif
    T1.CCR3 = write_pulse; /* Re-arm the direction pulse (safe against a stale CC2 DMA reload) */
    T1.CR1 = TIM_CR1(OPM, CEN);
}

#endif /* OW_PORT_F4_H */
