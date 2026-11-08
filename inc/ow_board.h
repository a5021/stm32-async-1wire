/**
 * @file ow_board.h
 * @brief Public board-level facts for the non-blocking 1-Wire layer.
 *
 * The small part of the port that the public API and the application layer
 * legitimately need, with no CMSIS device header and no register vocabulary:
 *
 *   - the family selection token (OW_FAMILY_*), resolved from the
 *     STMicroelectronics device macro of the target (e.g. -DSTM32F030x6) or,
 *     for host-test mocks, PlatformIO and STM32CubeMX projects, from a family
 *     macro (STM32F0, STM32F1, STM32F3, STM32F4, STM32G0, STM32G4, STM32L4,
 *     STM32H5, STM32H7);
 *   - ow_pulse_t, the DMA feed entry the public write API hands to the
 *     hardware: 8-bit on F0/F1/F3/G0/G4/L4/H5, 16-bit on F4/H7 (direct-mode
 *     feed);
 *   - the board clock defaults read by the application layer (OW_HSE_MHZ and
 *     the OW_*_MAX_SYSCLK_MHZ part ceilings).
 *
 * The CMSIS device header, the per-family register/DMA facts and the
 * TIM1+DMA engine are private to the library and are never reached from
 * the public headers.
 *
 * Include order: onewire.h includes this header for ow_pulse_t, and the
 * engine's family header includes it for the dispatch, so the token is
 * defined exactly once and nothing depends on which is included first.
 * It is the single point of family selection.
 */

#ifndef OW_BOARD_H
#define OW_BOARD_H

/* ------------------------------------------------------------------
 *  Removed public names: fail loudly instead of silently ignoring them.
 *
 *  3.0.0 renamed the public macros when the port layer became private and
 *  dropped every compatibility alias. A build that still passes an old name
 *  would otherwise compile with the renamed knob at its default, which is
 *  exactly the silent change the rename was meant to prevent - a build with
 *  the old low-power flag would quietly lose the WFE path. These guards make
 *  the migration explicit at the first compiler invocation.
 * ------------------------------------------------------------------ */
#if defined(OW_PORT_FAMILY_F0)
#error "OW_PORT_FAMILY_F0 was renamed to OW_FAMILY_F0 in 3.0.0; select the family via the device macro (e.g. -DSTM32F030x6) or -DSTM32F0"
#endif
#if defined(OW_PORT_FAMILY_F1)
#error "OW_PORT_FAMILY_F1 was renamed to OW_FAMILY_F1 in 3.0.0; select the family via the device macro (e.g. -DSTM32F103xB) or -DSTM32F1"
#endif
#if defined(OW_PORT_FAMILY_F3)
#error "OW_PORT_FAMILY_F3 was renamed to OW_FAMILY_F3 in 3.0.0; select the family via the device macro (e.g. -DSTM32F303xC) or -DSTM32F3"
#endif
#if defined(OW_PORT_FAMILY_F4)
#error "OW_PORT_FAMILY_F4 was renamed to OW_FAMILY_F4 in 3.0.0; select the family via the device macro (e.g. -DSTM32F407xx) or -DSTM32F4"
#endif
#if defined(OW_PORT_FAMILY_G0)
#error "OW_PORT_FAMILY_G0 was renamed to OW_FAMILY_G0 in 3.0.0; select the family via the device macro (e.g. -DSTM32G031xx) or -DSTM32G0"
#endif
#if defined(OW_PORT_FAMILY_G4)
#error "OW_PORT_FAMILY_G4 was renamed to OW_FAMILY_G4 in 3.0.0; select the family via the device macro (e.g. -DSTM32G474xx) or -DSTM32G4"
#endif
#if defined(OW_PORT_FAMILY_L4)
#error "OW_PORT_FAMILY_L4 was renamed to OW_FAMILY_L4 in 3.0.0; select the family via the device macro (e.g. -DSTM32L476xx) or -DSTM32L4"
#endif
#if defined(OW_PORT_FAMILY_H5)
#error "OW_PORT_FAMILY_H5 was renamed to OW_FAMILY_H5 in 3.0.0; select the family via the device macro (e.g. -DSTM32H503xx) or -DSTM32H5"
#endif
#if defined(OW_PORT_FAMILY_H7)
#error "OW_PORT_FAMILY_H7 was renamed to OW_FAMILY_H7 in 3.0.0; select the family via the device macro (e.g. -DSTM32H743xx) or -DSTM32H7"
#endif
#if defined(OW_PORT_SYSCLK_MHZ)
#error "OW_PORT_SYSCLK_MHZ was renamed to OW_SYSCLK_MHZ in 3.0.0; pass -DOW_SYSCLK_MHZ=<MHz>"
#endif
#if defined(OW_PORT_F4_MAX_SYSCLK_MHZ)
#error "OW_PORT_F4_MAX_SYSCLK_MHZ was renamed to OW_F4_MAX_SYSCLK_MHZ in 3.0.0; pass -DOW_F4_MAX_SYSCLK_MHZ=<MHz>"
#endif
#if defined(OW_PORT_G4_MAX_SYSCLK_MHZ)
#error "OW_PORT_G4_MAX_SYSCLK_MHZ was renamed to OW_G4_MAX_SYSCLK_MHZ in 3.0.0; pass -DOW_G4_MAX_SYSCLK_MHZ=<MHz>"
#endif
#if defined(OW_PORT_H5_MAX_SYSCLK_MHZ)
#error "OW_PORT_H5_MAX_SYSCLK_MHZ was renamed to OW_H5_MAX_SYSCLK_MHZ in 3.0.0; pass -DOW_H5_MAX_SYSCLK_MHZ=<MHz>"
#endif

#include <stdint.h>

/* ------------------------------------------------------------------
 *  Family selection: a single OW_FAMILY_* token resolved from
 *  the CMSIS device macro of the target (e.g. -DSTM32F030x6) or, for
 *  host-test mocks, PlatformIO and STM32CubeMX projects, from a family
 *  macro (STM32F0, STM32F1, STM32F3, STM32F4, STM32G0, STM32G4, STM32L4,
 *  STM32H5, STM32H7).  Concrete device spellings are accepted for the parts this project
 *  supports (STM32F030x6 for F0; STM32F103xB for F1; STM32F303xC for F3;
 *  STM32F407xx / STM32F401xC / STM32F446xx for F4;
 *  STM32G031xx for G0; STM32G474xx for G4; STM32L476xx for L4;
 *  STM32H503xx for H5; STM32H743xx for H7).
 * ------------------------------------------------------------------ */
#if defined(STM32F030x6) || defined(STM32F0)
#define OW_FAMILY_F0
#elif defined(STM32F103xB) || defined(STM32F1)
#define OW_FAMILY_F1
#elif defined(STM32F303xC) || defined(STM32F3)
#define OW_FAMILY_F3
#elif defined(STM32F407xx) || defined(STM32F401xC) || \
    defined(STM32F446xx) || defined(STM32F4)
#define OW_FAMILY_F4
#elif defined(STM32G031xx) || defined(STM32G0)
#define OW_FAMILY_G0
#elif defined(STM32G474xx) || defined(STM32G4)
#define OW_FAMILY_G4
#elif defined(STM32L476xx) || defined(STM32L4)
#define OW_FAMILY_L4
#elif defined(STM32C552xx) || defined(STM32C5)
#define OW_FAMILY_C5
#elif defined(STM32H503xx) || defined(STM32H5)
#define OW_FAMILY_H5
#elif defined(STM32H743xx) || defined(STM32H7)
#define OW_FAMILY_H7
#else
#error "ow_board: no MCU selected. Pass your part's STMicroelectronics device macro (e.g. -DSTM32H743xx) or its family macro (e.g. -DSTM32H7). This is the ONLY define the library requires; OW_SYSCLK_MHZ is optional (the port core reads CMSIS SystemCoreClock when it is absent). -DSTM32F030x6/STM32F103xB/STM32F303xC/STM32F407xx/STM32F401xC/STM32F446xx/STM32G031xx/STM32G474xx/STM32L476xx/STM32C552xx/STM32H503xx/STM32H743xx or -DSTM32F0/F1/F3/F4/G0/G4/L4/C5/H5/H7 are accepted. The Make/CMake builds supply it from OW_MCU/chips/<part>.mk."
#endif

/**
 * @brief DMA feed entry width of the active family.
 *
 * The public write API (onewire_write_slots/us, onewire_encode_byte) takes a
 * buffer of these and the hardware feeds CCR3 from it without a copy.  F4
 * and H7 feed in DMA direct mode, where the memory-transfer width is forced
 * to the peripheral size, so an entry is a 16-bit halfword there; every
 * other family latches 8-bit entries.  Keeping it family-sized avoids
 * doubling the pulse buffers on 4 KB-RAM parts.
 */
#if defined(OW_FAMILY_F4) || defined(OW_FAMILY_H7)
typedef uint16_t ow_pulse_t;
#else
typedef uint8_t ow_pulse_t;
#endif
/* ------------------------------------------------------------------
 *  Board clock defaults.  The external crystal belongs to the board and
 *  the system clock to the application, so they are separate knobs; the
 *  build passes OW_SYSCLK_MHZ for the part, and app.c reads these.
 *  A wrong OW_HSE_MHZ is not a compile error - the PLL simply never
 *  locks.  The *_MAX_SYSCLK_MHZ ceilings let app.c reject a request
 *  above what the concrete part can take.
 * ------------------------------------------------------------------ */
#if defined(OW_FAMILY_F3)
/* No HSI16 on this family: the 72MHz PLL multiplies the crystal by 9. */
#if !defined(OW_HSE_MHZ)
#define OW_HSE_MHZ 8
#endif
#elif defined(OW_FAMILY_F4)
#if !defined(OW_F4_MAX_SYSCLK_MHZ)
#if defined(STM32F446xx)
#define OW_F4_MAX_SYSCLK_MHZ 180 /* F446: the only part here that needs the over-drive sequence */
#elif defined(STM32F401xC)
#define OW_F4_MAX_SYSCLK_MHZ 84 /* F401 parts top out at 84MHz */
#else
#define OW_F4_MAX_SYSCLK_MHZ 168 /* F405/F407 cap */
#endif
#endif
#if !defined(OW_HSE_MHZ)
#define OW_HSE_MHZ 8
#endif
#elif defined(OW_FAMILY_G4)
#if !defined(OW_G4_MAX_SYSCLK_MHZ)
#define OW_G4_MAX_SYSCLK_MHZ 170 /* G474 ceiling, voltage Range 1 Boost */
#endif
#if !defined(OW_HSE_MHZ)
#define OW_HSE_MHZ 8 /* WeAct STM32G474CBT6 Long: 8MHz crystal */
#endif
#elif defined(OW_FAMILY_L4)
#if !defined(OW_L4_MAX_SYSCLK_MHZ)
#define OW_L4_MAX_SYSCLK_MHZ 80 /* L476 ceiling, voltage Range 1 */
#endif
/* No OW_HSE_MHZ default: the NUCLEO-L476RG has no crystal populated and this
 * backend is HSI16-only by design, so there is no external-oscillator knob to
 * get wrong. */
#elif defined(OW_FAMILY_H5)
#if !defined(OW_H5_MAX_SYSCLK_MHZ)
#define OW_H5_MAX_SYSCLK_MHZ 250 /* H503 ceiling */
#endif
#if !defined(OW_HSE_MHZ)
#define OW_HSE_MHZ 8 /* WeAct STM32H503Cx Core Board: 8MHz crystal (Y2) */
#endif
#elif defined(OW_FAMILY_C5)
#if !defined(OW_C5_MAX_SYSCLK_MHZ)
#define OW_C5_MAX_SYSCLK_MHZ 144 /* C552 ceiling */
#endif
#if !defined(OW_HSE_MHZ)
#define OW_HSE_MHZ 8 /* WeAct STM32C5 48-pin Core Board: 8MHz crystal (Y2) */
#endif
#elif defined(OW_FAMILY_H7)
#if !defined(OW_H7_MAX_SYSCLK_MHZ)
#define OW_H7_MAX_SYSCLK_MHZ 480 /* Rev V H743 core ceiling; 480MHz requires VOS0 */
#endif
#if !defined(OW_H7_MAX_TIM1_MHZ)
#define OW_H7_MAX_TIM1_MHZ 240 /* APB2 <=120MHz, timer x2; this is the 1-Wire timing ceiling */
#endif
#if !defined(OW_HSE_MHZ)
#define OW_HSE_MHZ 25 /* WeAct Studio STM32H7XX board: 25MHz crystal */
#endif
#endif

#endif /* OW_BOARD_H */
