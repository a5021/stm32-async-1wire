/**
 * @file ow_mcu.h
 * @brief Target-microcontroller facts for the non-blocking 1-Wire engine.
 *
 * Consolidates the family-specific halves of the former port backends
 * (port/stm32f0/ow_port_f0.h ... port/stm32h5/ow_port_h5.h): which clocks to
 * gate, the PA10 pin-mux tokens, the DMA channel/stream assignment and the
 * request-routing macros, plus the ow_pulse_t width. Everything else,
 * including the merged write+read pass, is the shared TIM1+DMA body in
 * inc/ow_port.h, which includes this file first.
 *
 * Include order (linear, acyclic): a TU includes onewire.h (or ow_port.h
 * directly), which reaches this file before any engine code, so the family
 * facts are always in scope where the engine uses them.
 *
 * The CMSIS device header is included per family branch below: on target
 * builds it resolves through the CMSIS device include path, on host-test
 * builds tests/mock/ shadows it with the register mock (keep the mock
 * directory ahead of any real CMSIS directory on the host include path).
 */

#ifndef OW_MCU_H
#define OW_MCU_H

#include <stdint.h>

/* ------------------------------------------------------------------
 *  Family selection: a single OW_PORT_FAMILY_* token resolved from
 *  the CMSIS device macro of the target (e.g. -DSTM32F030x6) or, for
 *  host-test mocks, PlatformIO and STM32CubeMX projects, from a family
 *  macro (STM32F0, STM32F1, STM32F3, STM32F4, STM32G0, STM32G4, STM32H5).
 *  Concrete device spellings are accepted for the parts this project
 *  supports (STM32F030x6 for F0; STM32F103xB for F1; STM32F303xC for F3;
 *  STM32F407xx / STM32F401xC / STM32F401xE / STM32F446xx for F4;
 *  STM32G031xx for G0; STM32G474xx for G4; STM32H503xx for H5).
 *  (Moved here from onewire.h; the single point of family selection.)
 * ------------------------------------------------------------------ */
#if defined(STM32F030x6) || defined(STM32F0)
#define OW_PORT_FAMILY_F0
#elif defined(STM32F103xB) || defined(STM32F1)
#define OW_PORT_FAMILY_F1
#elif defined(STM32F303xC) || defined(STM32F3)
#define OW_PORT_FAMILY_F3
#elif defined(STM32F407xx) || defined(STM32F401xC) || defined(STM32F401xE) || \
    defined(STM32F446xx) || defined(STM32F4)
#define OW_PORT_FAMILY_F4
#elif defined(STM32G031xx) || defined(STM32G0)
#define OW_PORT_FAMILY_G0
#elif defined(STM32G474xx) || defined(STM32G4)
#define OW_PORT_FAMILY_G4
#elif defined(STM32H503xx) || defined(STM32H5)
#define OW_PORT_FAMILY_H5
#else
#error "ow_mcu: no MCU selected. Pass your part's STMicroelectronics device macro (e.g. -DSTM32H503xx) or its family macro (e.g. -DSTM32H5). This is the ONLY define the library requires; OW_PORT_SYSCLK_MHZ is optional (the port core reads CMSIS SystemCoreClock when it is absent). -DSTM32F030x6/STM32F103xB/STM32F303xC/STM32F407xx/STM32F401xC/STM32F401xE/STM32F446xx/STM32G031xx/STM32G474xx/STM32H503xx or -DSTM32F0/F1/F3/F4/G0/G4/H5 are accepted. The Make/CMake builds supply it from OW_MCU/chips/<part>.mk."
#endif

/* === STM32F0 ===
 *  Family-specific half only.  The TIM1/DMA state machine is shared with the
 *  other six families and lives in inc/ow_port.h; what is left here is the
 *  part that is genuinely an F030: which clocks to gate, the PA10 pin-mux
 *  tokens, and the DMA assignment note. */
#if defined(OW_PORT_FAMILY_F0)
#include "stm32f0xx.h"

/* ------------------------------------------------------------------
 *  Per-family facts owned by this port.  onewire.h delegates them here so the
 *  core headers carry no family knowledge; the dispatch at the top of this
 *  file is the only other place a family appears.
 *  - ow_pulse_t: 8-bit feed entry.  The basic-DMA backends feed CCR3 with a
 *    peripheral read (PSIZE=16) widened to the byte entries, so pulse buffers
 *    are uint8_t here (contrast the F4 backend, a uint16_t direct-mode feed).
 *    Capture-side durations are always uint16_t.
 *  - OW_PORT_SYSCLK_MHZ: system clock in MHz; the timer prescaler and the
 *    IC4F ladder derive from it.  The build systems pass it explicitly
 *    (-DOW_PORT_SYSCLK_MHZ=$(CHIP_SYSCLK_MHZ) from chips/<part>.mk); without
 *    it the shared core reads the CMSIS SystemCoreClock at startup instead.
 *    Validate any change against every supported clock (app.c).
 *  - OW_PORT_TIM1_UPD_IRQn: TIM1 update IRQ for the low-power WFE path. */
typedef uint8_t ow_pulse_t;

#if OW_PORT_LOW_POWER
#define OW_PORT_TIM1_UPD_IRQn TIM1_BRK_UP_TRG_COM_IRQn
#endif

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

/* @brief PA10: alternate function, open-drain, AF2 (TIM1_CH3).
 *
 *  F0 has the modern GPIO register model, as G0 does; F1 configures the same
 *  pin through the legacy CRH field instead, which is why this is a macro
 *  rather than shared code.  The whole MODE field is cleared first so the
 *  pin lands in the right mode even if something set it before us.
 */
#define OW_PORT_CONFIG_BUS_PIN()                                                      \
    do {                                                                              \
        PA.MODER = (PA.MODER & ~GPIO_MODER_MODER10) | GPIO_MODER_MODER10_1;           \
        PA.OTYPER |= GPIO_OTYPER_OT_10;                                               \
        PA.AFR[1] = (PA.AFR[1] & ~GPIO_AFRH_AFSEL10) | (2u << GPIO_AFRH_AFSEL10_Pos); \
        /* Drive strength is configurable via OW_BUS_DRIVE, default MAX. */           \
        PA.OSPEEDR = (PA.OSPEEDR & ~GPIO_OSPEEDR_OSPEEDR10) |                         \
                     ((OW_BUS_DRIVE & 0x3u) << GPIO_OSPEEDR_OSPEEDR10_Pos);           \
    } while (0)

/* @brief Toggle the bus pin between open-drain and push-pull.
 *
 *  Push-pull is only used by the experimental active-drive write path
 *  (OW_DRIVE_ACTIVE); the slave has to be able to pull the line LOW while the
 *  master reads, so every read and reset phase returns to open-drain.
 */
#define OW_PORT_SET_PIN_MODE(push_pull)                                   \
    do {                                                                  \
        if (push_pull) {                                                  \
            PA.OTYPER &= ~GPIO_OTYPER_OT_10; /* OD -> PP (strong HIGH) */ \
        } else {                                                          \
            PA.OTYPER |= GPIO_OTYPER_OT_10; /* PP -> OD (release) */      \
        }                                                                 \
    } while (0)

/* @brief DMA channel assignment (fixed request map, verified at bring-up):
 *       channel 3 carries the CC2 slot-end marker request and feeds CCR3,
 *       channel 4 carries the CC4 capture request and drains CCR4 - the core
 *       D13/D14 defaults.  Request routing needs no programming here;
 *       USART1_TX shares channel 4's request line, which is harmless because
 *       the driver's UART output runs without DMA. */
#endif /* OW_PORT_FAMILY_F0 */

/* === STM32F1 ===
 *  Family-specific half only.  The TIM1/DMA state machine is shared with the
 *  other six families and lives in inc/ow_port.h; what is left here is the
 *  part that is genuinely an F103: which clocks to gate, and how PA10 is put
 *  into alternate-function open-drain mode (legacy CRH). */
#if defined(OW_PORT_FAMILY_F1)
#include "stm32f1xx.h"

/* ------------------------------------------------------------------
 *  Per-family facts owned by this port.  onewire.h delegates them here so the
 *  core headers carry no family knowledge; the dispatch at the top of this
 *  file is the only other place a family appears.
 *  - ow_pulse_t: 8-bit feed entry.  The basic-DMA backends feed CCR3 with a
 *    peripheral read (PSIZE=16) widened to the byte entries, so pulse buffers
 *    are uint8_t here (contrast the F4 backend, a uint16_t direct-mode feed).
 *    Capture-side durations are always uint16_t.
 *  - OW_PORT_SYSCLK_MHZ: system clock in MHz; the timer prescaler and the
 *    IC4F ladder derive from it.  The build systems pass it explicitly
 *    (-DOW_PORT_SYSCLK_MHZ=$(CHIP_SYSCLK_MHZ) from chips/<part>.mk); without
 *    it the shared core reads the CMSIS SystemCoreClock at startup instead.
 *    Validate any change against every supported clock (app.c).
 *  - OW_PORT_TIM1_UPD_IRQn: TIM1 update IRQ for the low-power WFE path. */
typedef uint8_t ow_pulse_t;

#if OW_PORT_LOW_POWER
#define OW_PORT_TIM1_UPD_IRQn TIM1_UP_IRQn
#endif

/* @brief Gate GPIOA, TIM1 and DMA1.
 *
 *  F0/F1 put the port and timer gates in APB2ENR and the DMA gate in AHBENR
 *  (F1 names them IOPAEN/TIM1EN/DMA1EN, F0 GPIOAEN/DMAEN); G0 uses
 *  IOPENR/APBENR2 with a read-back to settle the APB clock before its first
 *  SYSCFG access.
 */
#define OW_PORT_ENABLE_BUS_CLOCKS()                \
    do {                                           \
        RC.APB2ENR |= RCC_APB2ENR(IOPAEN, TIM1EN); \
        RC.AHBENR |= RCC_AHBENR(DMA1EN);           \
    } while (0)

/* @brief PA10: alternate function, open-drain, 2 MHz (TIM1_CH3, default map).
 *
 *  F1 is the odd one out here: it has the legacy GPIO_CRH configuration field
 *  rather than the modern MODER/OTYPER/AFR split that F0 and G0 use, which is
 *  why this is a macro and not shared code.  The whole MODE10/CNF10 field is
 *  cleared first so the pin is configured correctly even if it was previously
 *  set to another mode.
 */
#define OW_PORT_CONFIG_BUS_PIN()                       \
    do {                                               \
        PA.CRH = (PA.CRH & ~GPIO_CRH(MODE10, CNF10)) | \
                 GPIO_CRH(MODE10_1, CNF10_0, CNF10_1); \
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
#define OW_PORT_SET_PIN_MODE(push_pull)                           \
    do {                                                          \
        PA.CRH = (PA.CRH & ~GPIO_CRH_CNF10) |                     \
                 (push_pull ? GPIO_CRH_CNF10_1 : GPIO_CRH_CNF10); \
    } while (0)

/* @brief DMA channel assignment (fixed request map, verified on target):
 *       feed rides TIM1_CC2 -> channel 3, capture rides CH4 -> channel 4 -
 *       the core D13/D14 defaults.  Request routing needs no programming
 *       (USART1_TX shares channel 4's request line, which is harmless because
 *       the driver's UART output runs without DMA). */
#endif /* OW_PORT_FAMILY_F1 */

/* === STM32F3 ===
 *  Family-specific half only.  The TIM1/DMA state machine is shared with the
 *  other six families and lives in inc/ow_port.h; what is left here is the
 *  part that is genuinely an F303: which clocks to gate, the PA10 pin-mux
 *  tokens, and the DMA assignment note.
 *
 *  Verified against the STM32F3-DISCOVERY (MB1035B, STM32F303VC), 7 DS18B20
 *  in parasite power on one bus, at 8, 64 and 72 MHz. */
#if defined(OW_PORT_FAMILY_F3)
#include "stm32f3xx.h"

/* ------------------------------------------------------------------
 *  Per-family facts owned by this port.  onewire.h delegates them here so the
 *  core headers carry no family knowledge; the dispatch at the top of this
 *  file is the only other place a family appears.
 *  - ow_pulse_t: 8-bit feed entry.  The basic-DMA backends feed CCR3 with a
 *    peripheral read (PSIZE=16) widened to the byte entries, so pulse buffers
 *    are uint8_t here (contrast the F4 backend, a uint16_t direct-mode feed).
 *    Capture-side durations are always uint16_t.
 *  - OW_PORT_SYSCLK_MHZ: system clock in MHz; the timer prescaler and the
 *    IC4F ladder derive from it.  The build systems pass it explicitly
 *    (-DOW_PORT_SYSCLK_MHZ=$(CHIP_SYSCLK_MHZ) from chips/<part>.mk); without
 *    it the shared core reads the CMSIS SystemCoreClock at startup instead.
 *    Validate any change against every supported clock (app.c).
 *  - OW_HSE_MHZ: external-crystal frequency, read by app.c as the PLL input
 *    (this family has no HSI16, so the 72MHz PLL multiplies the crystal by 9).
 *    Deliberately separate from OW_PORT_SYSCLK_MHZ: the crystal belongs to the
 *    *board*, the system clock to the *application*.
 *  - OW_PORT_TIM1_UPD_IRQn: TIM1 update IRQ for the low-power WFE path. */
typedef uint8_t ow_pulse_t;

#if !defined(OW_HSE_MHZ)
#define OW_HSE_MHZ 8
#endif

#if OW_PORT_LOW_POWER
#define OW_PORT_TIM1_UPD_IRQn TIM1_UP_TIM16_IRQn
#endif

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
 *  Same register model as F0/F4/G0, but three of the macro *names* differ from
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
#endif /* OW_PORT_FAMILY_F3 */

/* === STM32F4 ===
 *  Thin shim over the shared TIM1+DMA core.
 *
 *  The 1-Wire bus runs on PA10 (TIM1_CH3 PWM output in open-drain alternate
 *  function AF1); CH4 captures in indirect mode on the same pin (CC4S routes IC4 to TI3) and drains CCR4 via
 *  DMA2_Stream4, while the CC2 slot-end marker (DMA2_Stream2) reloads CCR3.
 *
 *  This section owns only the F4 facts (ow_pulse_t, clocks, pin mux, DMA
 *  stream assignment, the OW_PORT_DMA_xx overrides).  Everything else,
 *  including the merged write+read pass, is the shared body in
 *  inc/ow_port.h, included after this file once the macros above are defined.
 *
 *  HARDWARE CONTRACT (see the "Required Timer Capabilities" section of README):
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
 *  Bring-up notes and the alternatives that were tried and rejected:
 *  see port/stm32f4/HARDWARE-NOTES.md.
 */
#if defined(OW_PORT_FAMILY_F4)
#include "stm32f4xx.h"

/* ------------------------------------------------------------------
 *  Per-family facts owned by this port.  onewire.h delegates them here so the
 *  core headers carry no family knowledge; the dispatch at the top of this
 *  file is the only other place a family appears.
 *  - ow_pulse_t: 16-bit feed entry.  The F4 backend feeds CCR3 in DMA direct
 *    mode, where the memory-transfer width is forced to PSIZE, so a feed entry
 *    is a 16-bit halfword here (zero-copy, CNDTR == slots); every other
 *    backend latches 8-bit entries.  Capture-side durations are always uint16_t.
 *  - OW_PORT_SYSCLK_MHZ: system clock in MHz, one fact per concrete part;
 *    the timer prescaler and the IC4F ladder derive from it.  The build
 *    systems pass it explicitly (-DOW_PORT_SYSCLK_MHZ=$(CHIP_SYSCLK_MHZ)
 *    from chips/f4<part>.mk); without it the shared core reads the CMSIS
 *    SystemCoreClock at startup instead.  Validate any change against every
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
#elif defined(STM32F401xC) || defined(STM32F401xE)
#define OW_PORT_F4_MAX_SYSCLK_MHZ 84 /* F401 parts top out at 84MHz */
#else
#define OW_PORT_F4_MAX_SYSCLK_MHZ 168 /* F405/F407 cap */
#endif
#endif

#if !defined(OW_HSE_MHZ)
#define OW_HSE_MHZ 8
#endif

#if OW_PORT_LOW_POWER
#define OW_PORT_TIM1_UPD_IRQn TIM1_UP_TIM10_IRQn
#endif

/* IC4F ladder and OC3PE knob: owned by the shared core (identical tiers),
 * which resolves OW_PORT_SYSCLK_MHZ (or SystemCoreClock) before its own
 * definitions. */

/* ------------------------------------------------------------------
 *  1-Wire bus pin: PA10 (TIM1_CH3 output + IC4 capture via TI3, AF1).
 *
 *  PA11 is left untouched for the application; logic-analyzer sync, if
 *  needed, goes through the opt-in OW_PORT_MARKER_TOGGLE() hook.
 *  (The former PE13 alternate-pin option was removed: no board used it,
 *  no bench ever validated it.  Passing -DOW_PORT_BUS_PE13=1 is a hard
 *  error below instead of a silent PA10 build.)
 * ------------------------------------------------------------------ */
#ifdef OW_PORT_BUS_PE13
#error "OW_PORT_BUS_PE13 was removed (PA10-only bus); see CHANGELOG"
#endif

/* Statement macro spelling of the pin-mode switch (same shape as the other
 * backends' OW_PORT_SET_PIN_MODE): the shared core calls this to toggle the
 * bus pin between open-drain and push-pull. */
#define OW_PORT_SET_PIN_MODE(push_pull)                                   \
    do {                                                                  \
        if (push_pull) {                                                  \
            PA.OTYPER &= ~GPIO_OTYPER_OT_10; /* OD -> PP (strong HIGH) */ \
        } else {                                                          \
            PA.OTYPER |= GPIO_OTYPER_OT_10; /* PP -> OD (release) */      \
        }                                                                 \
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
 * DISABLE macros below (bounded wait identical to the old rearm request
 * path). */
#define OW_PORT_DMA_EN_BIT DMA_SxCR_EN

/* @brief Capture-stream control bits: MINC = memory-increment, PSIZE_0 =
 *       16-bit peripheral read (CCR4).  Direct mode forces the memory width
 *       to PSIZE, so the caller also sets MSIZE_0 (matching halfwords).
 *       Priority stays at the reset default (like every other backend: ties
 *       break by stream number, feed wins).  Note the F4 DIR field is two
 *       bits wide: the generic single-bit DMA_SxCR_DIR macro would write the
 *       reserved 0b11. */
#define OW_PORT_DMA_CR_CAPTURE (DMA_SxCR_MINC | DMA_SxCR_PSIZE_0)

/* @brief Feed-stream control bits: DIR_0 = memory-to-peripheral, MINC =
 *       memory-increment, PSIZE_0 = 16-bit peripheral write (CCR3 is a
 *       halfword register).  Priority stays at the reset default (see
 *       above).  The caller also sets MSIZE_0: direct mode forces the memory
 *       width to PSIZE, so the source must be a matching halfword
 *       (ow_pulse_t) buffer. */
#define OW_PORT_DMA_CR_FEED (DMA_SxCR_DIR_0 | DMA_SxCR_MINC | DMA_SxCR_PSIZE_0)

/* Port-unification overrides for the OW_PORT_DMA_CR_* vocabulary (see the
 * defaults in inc/ow_port.h): same three transfer classes in stream
 * spelling.  Each equals the per-site expression used by the migrated
 * bodies (capture, read pair and merged capture share RX16; feed and merged
 * feed share TX) - pinned by
 * tests/test/test_dma_contract.c::test_dma_cr_value_macros.  Every
 * OW_PORT_DMA_CR_* definition (here and in ow_port.h) spells explicit CMSIS
 * masks instead of the DMA_CCR/DMA_SxCR sugar: the contract tests expand
 * these macros in their own TU, after ow_port.h has withdrawn the BITS()
 * helpers. */
#define OW_PORT_DMA_CR_RX16 (OW_PORT_DMA_CR_CAPTURE | OW_PORT_DMA_CHSEL | DMA_SxCR_MSIZE_0 | DMA_SxCR_EN)
#define OW_PORT_DMA_CR_RX8 ((OW_PORT_DMA_CR_CAPTURE & ~(DMA_SxCR_PSIZE_0)) | OW_PORT_DMA_CHSEL | DMA_SxCR_EN)
#define OW_PORT_DMA_CR_TX (OW_PORT_DMA_CR_FEED | OW_PORT_DMA_CHSEL | DMA_SxCR_MSIZE_0 | DMA_SxCR_EN)

/* Stream-spelling overrides for the DISABLE/PROG vocabulary (defaults in
 * inc/ow_port.h).  DISABLE is disable + bounded EN wait
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
#define OW_PORT_ENABLE_BUS_CLOCKS()                                                          \
    do {                                                                                     \
        RC.AHB1ENR |= RCC_BITS(AHB1ENR, DMA2EN, GPIOAEN); /* TIM1 requests route via DMA2 */ \
        RC.APB2ENR |= RCC_APB2ENR(TIM1EN);                                                   \
    } while (0)
#define OW_PORT_CONFIG_BUS_PIN()                                                      \
    do {                                                                              \
        PA.MODER = (PA.MODER & ~GPIO_MODER_MODER10) | GPIO_MODER_MODER10_1;           \
        PA.OTYPER |= GPIO_OTYPER_OT_10;                                               \
        PA.AFR[1] = (PA.AFR[1] & ~GPIO_AFRH_AFSEL10) | (1u << GPIO_AFRH_AFSEL10_Pos); \
        PA.OSPEEDR = (PA.OSPEEDR & ~GPIO_OSPEEDR_OSPEED10) |                          \
                     ((OW_BUS_DRIVE & 0x3u) << GPIO_OSPEEDR_OSPEED10_Pos);            \
    } while (0)
#endif /* OW_PORT_FAMILY_F4 */

/* === STM32G0 ===
 *  Family-specific half only.  The TIM1/DMA state machine is shared with the
 *  other six families and lives in inc/ow_port.h.  G0 differs in three ways,
 *  all of them here: the bus pins are remapped, the GPIO register macros
 *  have a different spelling, and DMA requests go through DMAMUX instead of
 *  a fixed map. */
#if defined(OW_PORT_FAMILY_G0)
#include "stm32g0xx.h"

/* ------------------------------------------------------------------
 *  Per-family facts owned by this port.  onewire.h delegates them here so the
 *  core headers carry no family knowledge; the dispatch at the top of this
 *  file is the only other place a family appears.
 *  - ow_pulse_t: 8-bit feed entry.  The basic-DMA backends feed CCR3 with a
 *    peripheral read (PSIZE=16) widened to the byte entries, so pulse buffers
 *    are uint8_t here (contrast the F4 backend, a uint16_t direct-mode feed).
 *    Capture-side durations are always uint16_t.
 *  - OW_PORT_SYSCLK_MHZ: system clock in MHz; the timer prescaler and the
 *    IC4F ladder derive from it.  The build systems pass it explicitly
 *    (-DOW_PORT_SYSCLK_MHZ=$(CHIP_SYSCLK_MHZ) from chips/<part>.mk); without
 *    it the shared core reads the CMSIS SystemCoreClock at startup instead.
 *    Validate any change against every supported clock (app.c).
 *  - OW_PORT_TIM1_UPD_IRQn: TIM1 update IRQ for the low-power WFE path. */
typedef uint8_t ow_pulse_t;

#if OW_PORT_LOW_POWER
#define OW_PORT_TIM1_UPD_IRQn TIM1_BRK_UP_TRG_COM_IRQn
#endif

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

/* @brief Logical PA10: alternate function, open-drain, AF2 (TIM1_CH3).
 *
 *  Same register model as F0; the macro names differ because the G0 CMSIS
 *  spells the MODER fields without the R (MODE10, not MODER10) and drops the
 *  _R infix in OTYPER/OSPEEDR.  The values are the same pins.
 */
#define OW_PORT_CONFIG_BUS_PIN()                                                      \
    do {                                                                              \
        PA.MODER = (PA.MODER & ~GPIO_MODER_MODE10) | GPIO_MODER_MODE10_1;             \
        PA.OTYPER |= GPIO_OTYPER_OT10;                                                \
        PA.AFR[1] = (PA.AFR[1] & ~GPIO_AFRH_AFSEL10) | (2u << GPIO_AFRH_AFSEL10_Pos); \
        /* Drive strength is configurable via OW_BUS_DRIVE, default MAX. */           \
        PA.OSPEEDR = (PA.OSPEEDR & ~GPIO_OSPEEDR_OSPEED10) |                          \
                     ((OW_BUS_DRIVE & 0x3u) << GPIO_OSPEEDR_OSPEED10_Pos);            \
    } while (0)

/* @brief Toggle the bus pin between open-drain and push-pull.
 *
 *  Push-pull is only used by the experimental active-drive write path
 *  (OW_DRIVE_ACTIVE); the slave has to be able to pull the line LOW while the
 *  master reads, so every read and reset phase returns to open-drain.
 */
#define OW_PORT_SET_PIN_MODE(push_pull)                                  \
    do {                                                                 \
        if (push_pull) {                                                 \
            PA.OTYPER &= ~GPIO_OTYPER_OT10; /* OD -> PP (strong HIGH) */ \
        } else {                                                         \
            PA.OTYPER |= GPIO_OTYPER_OT10; /* PP -> OD (release) */      \
        }                                                                \
    } while (0)

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
#endif /* OW_PORT_FAMILY_G0 */

/* === STM32G4 ===
 *  Family-specific half only.  The TIM1/DMA state machine is shared with the
 *  other six families and lives in inc/ow_port.h.  G4 differs in four ways,
 *  all of them here: the clock gates (AHB1 for DMA+DMAMUX, AHB2 for GPIO,
 *  APB2 for TIM1+SYSCFG), the AF number of TIM1_CH3 on PA10, the DMAMUX
 *  request IDs, and the default clock (HSE+PLL to 170MHz). */
#if defined(OW_PORT_FAMILY_G4)
#include "stm32g4xx.h"

/* ------------------------------------------------------------------
 *  Per-family facts owned by this port.  onewire.h delegates them here so the
 *  core headers carry no family knowledge; the dispatch at the top of this
 *  file is the only other place a family appears.
 *  - ow_pulse_t: 8-bit feed entry.  The classic-DMA backends (F0/F1/F3/G0)
 *    feed CCR3 with a 16-bit peripheral read narrowed to byte memory (the
 *    RX8/TX defaults in the shared core), and the G4 DMA is the same classic
 *    IP behind a DMAMUX as G0's, so the same words apply here (contrast the F4
 *    backend, a uint16_t direct-mode feed on DMA2 streams).  Capture-side
 *    durations are always uint16_t.  Pending hardware proof: the first
 *    search-ROM matrix on the WeAct board is the experiment - F4's rejected
 *    byte feed (merged slots, all-zero ROMs, see port/stm32f4/HARDWARE-NOTES.md)
 *    is what a failure looks like.
 *  - OW_PORT_SYSCLK_MHZ: system clock in MHz (HSE+PLL, see app.c; 16
 *    selects raw HSI16).  The build systems pass it explicitly
 *    (-DOW_PORT_SYSCLK_MHZ=$(CHIP_SYSCLK_MHZ) from chips/<part>.mk); without
 *    it the shared core reads the CMSIS SystemCoreClock at startup instead.
 *  - OW_PORT_G4_MAX_SYSCLK_MHZ: the part ceiling; app.c rejects more.
 *  - OW_HSE_MHZ: the board crystal, read by app.c as the PLL's M divider.
 *  - OW_PORT_TIM1_UPD_IRQn: TIM1 update IRQ for the low-power WFE path
 *    (shared with TIM16 on this family). */
typedef uint8_t ow_pulse_t;

#if !defined(OW_PORT_G4_MAX_SYSCLK_MHZ)
#define OW_PORT_G4_MAX_SYSCLK_MHZ 170 /* G474 ceiling, voltage Range 1 Boost */
#endif

#if !defined(OW_HSE_MHZ)
#define OW_HSE_MHZ 8 /* WeAct STM32G474CBT6 Long: 8MHz crystal */
#endif

#if OW_PORT_LOW_POWER
#define OW_PORT_TIM1_UPD_IRQn TIM1_UP_TIM16_IRQn
#endif

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
 *  master reads, so every read and reset phase returns to open-drain.
 */
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
#endif /* OW_PORT_FAMILY_G4 */

/* === STM32H5 ===
 *  Vocabulary overrides over the shared TIM1+GPDMA core.
 *
 *  The 1-Wire bus runs on PA10 (TIM1_CH3 PWM output in open-drain alternate
 *  function AF1); CH4 captures in indirect mode on the same pin (CC4S routes IC4
 *  to TI3) and drains CCR4 via GPDMA1_Channel3, while the CC2 slot-end marker
 *  (GPDMA1_Channel2) reloads CCR3.
 *
 *  This section owns only the H5 facts (ow_pulse_t, clocks, pin mux, GPDMA
 *  channel assignment, the CTR1 templates below).  Everything else, including
 *  the merged write+read pass, is the shared body in
 *  inc/ow_port.h, included after this file once the macros above are defined.
 *
 *  HARDWARE CONTRACT (see the "Required Timer Capabilities" section of README):
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
 *  Bench-proven on the WeAct STM32H503CBT6 (ST-LINK, UART on COM5 at 115200):
 *  all seven examples run correctly at the HSI-64 default (correct 22.8-22.9C
 *  temperatures, 0 errors in 6_statistics, WFE sleep in 7_low_power), and the
 *  8MHz raw-HSE and 250MHz HSE+PLL1 clocks are proven by 2_device_search
 *  (correct temperatures) and 1_basic (clean UART at the selected baud).  The
 *  reset geometry, slot widths and feed reload timing are exercised by the same
 *  suite at all three clocks.
 */
#if defined(OW_PORT_FAMILY_H5)
#include "stm32h5xx.h"

/* ------------------------------------------------------------------
 *  Per-family facts owned by this port.  onewire.h delegates them here so the
 *  core headers carry no family knowledge; the dispatch at the top of this
 *  file is the only other place a family appears.
 *  - ow_pulse_t: 8-bit feed entry.  GPDMA converts source/destination widths
 *    in hardware, so a byte memory cell feeding the halfword CCR3 needs no
 *    packing games (contrast the F4 backend, a uint16_t direct-mode feed on
 *    DMA2 streams).  Capture-side durations are always uint16_t.
 *  - OW_PORT_SYSCLK_MHZ: system clock in MHz (HSI 64MHz direct, see
 *    app.c).  The build systems pass it explicitly
 *    (-DOW_PORT_SYSCLK_MHZ=$(CHIP_SYSCLK_MHZ) from chips/<part>.mk); without
 *    it the shared core reads the CMSIS SystemCoreClock at startup instead.
 *    The two other clocks app.c implements for this family are the raw 8MHz
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
 *    of F0/F1/G0). */
typedef uint8_t ow_pulse_t;

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
 * inc/ow_port.h).  DISABLE is disable + bounded EN wait
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
         * (SDW_LOG2 = 0) and stays BNDT = count bytes. */          \
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
        /* CC4 captures indirect from TI3 (bus); TISEL reset = direct TI4 pad */      \
        T1.TISEL = (T1.TISEL & ~TIM_TISEL_TI4SEL_Msk) | (0u << TIM_TISEL_TI4SEL_Pos); \
    } while (0)
#endif /* OW_PORT_FAMILY_H5 */

/* Default DMA channel assignment: feed rides TIM1_CC2 -> channel 3, capture
 * rides TIM1_CH4 -> channel 4 (D13/D14: register aliases from ow_port.h).
 * These are not
 * arbitrary channel numbers: each backend's pair was read out of its own
 * reference manual and bench-verified (F0/F1/F3/G0/G4 14/14 matrices, F4 LA,
 * H5 GPDMA channels 2/3 with REQSEL 59/61 pending LA proof),
 * and a wrong pair is silent (feed never fires, captures read back empty).
 * A backend on different DMA IP (F4: DMA2 streams) overrides both.  There is
 * deliberately no #error here: D13/D14 expand through ow_port.h's register
 * vocabulary, whose engine is where this default gets used, so a TU that
 * forgot its device macros still fails loudly on the missing device
 * macros. */
#ifndef OW_PORT_DMA_FEED
#define OW_PORT_DMA_FEED D13
#endif
#ifndef OW_PORT_DMA_CAPTURE
#define OW_PORT_DMA_CAPTURE D14
#endif
/* Default request routing: no-op (fixed request maps need no programming).
 * G0/G4 program their DMAMUX through these. */
#ifndef OW_PORT_ROUTE_CAPTURE
#define OW_PORT_ROUTE_CAPTURE() \
    do {                        \
    } while (0)
#endif
#ifndef OW_PORT_ROUTE_FEED
#define OW_PORT_ROUTE_FEED() \
    do {                     \
    } while (0)
#endif

#endif /* OW_MCU_H */
