/**
 * @file ow_port.h
 * @brief Platform port layer for the non-blocking 1-Wire engine
 *
 * The 1-Wire layer (onewire.c) and the DS18B20 driver (ds18b20.c) are written
 * against this thin, target-agnostic interface. Each target provides the
 * ow_port_* implementation as a header of static inline functions, so the
 * port layer compiles away to exactly the same register writes as a direct
 * bare-metal implementation: zero call overhead in any build mode, with or
 * without LTO, and no function pointers or runtime dispatch.
 *
 * Every ow_port_* call schedules exactly one hardware-timed operation on the
 * shared timer/DMA engine and returns immediately; the caller advances by
 * polling ow_port_bus_done(). The CPU is never in the timing-critical path.
 */

#ifndef OW_PORT_H
#define OW_PORT_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* --- 1-Wire reset timeslot geometry (microseconds), shared by all backends.
 *     The '1'/'0' bit-slot durations live in ow_config.h (ONEWIRE_ONE_PULSE,
 *     ONEWIRE_ZERO_PULSE, ONEWIRE_GUARD_BAND). --- */
#define OW_PORT_RESET_PULSE_DURATION 480u
#define OW_PORT_RESET_TIMEOUT 960u
#define OW_PORT_CAPTURE_BUF_SIZE 2u

/* onewire.h supplies OW_PORT_SYSCLK_MHZ (used for the timer prescaler and
 * the IC4F selection below), the bit-slot durations come from ow_config.h, and
 * it defines ow_pulse_t for the backend signatures.  Including it here keeps
 * this header self-contained regardless of TU include order. */
#include "onewire.h"

/* The byte-read capture path (ow_port_read_data, width==8) stores CCR4's
 * least-significant byte via MSIZE=8.  This is lossless only while every
 * slot capture stays below 256 Вµs; the counter runs 0..ARR, so the maximum
 * capture value is ONEWIRE_ONE_PULSE + ONEWIRE_ZERO_PULSE +
 * ONEWIRE_GUARD_BAND.  Enforce this globally across all backends. */
_Static_assert((ONEWIRE_ONE_PULSE + ONEWIRE_ZERO_PULSE + ONEWIRE_GUARD_BAND) < 256u,
               "8-bit read capture (MSIZE=8) would truncate slot durations");

/* --- CH4 input-capture digital filter (IC4F), one standard for every clock.
 *     Keep the filter time T_f = N Г— T_sample as close to ~500ns as the
 *     discrete IC4F table allows for the configured clock: that rejects
 *     sub-Вµs bus glitches while adding well under 1Вµs to read-slot captures
 *     вЂ” negligible against the ONEWIRE_SHORT_PULSE_MAX decode window (an
 *     IC4F sweep on STM32F030@8MHz decoded cleanly from fCK_INT N=2 all the
 *     way to fDTS/4 N=8).
 *
 *     The encoding is a ladder, and it is worth writing down because getting it
 *     wrong is silent - the filter still configures, it is just a different
 *     time. ICxF[3:0], from the RM0090 table (the same one ST's own
 *     stm32f0xx_ll_tim.h spells out, which is where these were read from):
 *       0000 none   0001 fCK_INT N=2  0010 fCK_INT N=4  0011 fCK_INT N=8
 *       0100 fDTS/2 N=6   0101 fDTS/2 N=8
 *       0110 fDTS/4 N=6   0111 fDTS/4 N=8
 *       1000 fDTS/8 N=5   1001 fDTS/8 N=8
 *       1010 fDTS/16 N=5  1011 fDTS/16 N=6  1100 fDTS/16 N=8
 *       1101 fDTS/32 N=5  1110 fDTS/32 N=6  1111 fDTS/32 N=8
 *     fDTS follows CKD, which no port here sets, so fDTS = fCK_INT = the
 *     timer kernel clock.
 *
 *     NOTE, corrected: the table above is a transcription that does not match
 *     the register. ICxF is a 4-bit ladder in which N only ever reaches 5, so
 *     "N=6" and "N=8" do not exist, and the values this header actually writes
 *     do not mean what the old comments claimed. What each tier really programs,
 *     decoded from the ladder, is:
 *
 *       ICxF_1                 = 0b0001 = fCK_INT/16, N=1
 *       ICxF_0|ICxF_1          = 0b0011 = fCK_INT/4,  N=1
 *       ICxF_0|ICxF_1|ICxF_2   = 0b0111 = fCK_INT/4,  N=2
 *       ICxF_3                 = 0b1000 = fCK_INT/32, N=3
 *       ICxF_0|ICxF_1|ICxF_3   = 0b1011 = fCK_INT/4,  N=3
 *
 *     So the >168MHz entry is fCK_INT/4, N=3 - a *shorter* window than the
 *     168MHz entry's fCK_INT/32, N=3, which is the opposite of the "fDTS/16 vs
 *     fDTS/8" the old comment claimed. The reasoning that put it there was
 *     arithmetic on a misread table, and it is recorded here rather than quietly
 *     corrected because the value itself is not harmful: the tier now runs the
 *     full 7-example matrix at 180MHz on a WeAct F446RET6 (see
 *     port/stm32f4/HARDWARE-NOTES.md), which is more than the old comment could
 *     claim for it.
 *
 *     The real consequence is that the "~500ns target" framing above is
 *     fiction: at these kernel clocks the ladder's finest window is fCK_INT/16,
 *     i.e. 5.6ns at 180MHz, and no encoding gets near 500ns. Anyone reasoning
 *     about this filter should start from the decoded values, not the T_f
 *     numbers. Sweeping remains possible with -DOW_PORT_IC4F_ARGS=..., and what
 *     the sweep found so far is that it is not what limits the F446.

 *     Backends feed the macro into TIM_CCMR2(...) unchanged.
 *     The default can be overridden from the build (-DOW_PORT_IC4F_ARGS=...)
 *     to sweep the filter on a bench.
 *     Test: tests/test_timing.c::test_ic4f_matches_the_documented_tier() */
#ifndef OW_PORT_IC4F_ARGS
#if (OW_PORT_SYSCLK_MHZ) <= 8
#define OW_PORT_IC4F_ARGS IC4F_1 /* 0b0001 = fCK_INT/16, N=1 */
#elif (OW_PORT_SYSCLK_MHZ) <= 16
#define OW_PORT_IC4F_ARGS IC4F_0, IC4F_1 /* 0b0011 = fCK_INT/4, N=1 */
#elif (OW_PORT_SYSCLK_MHZ) <= 72
#define OW_PORT_IC4F_ARGS IC4F_0, IC4F_1, IC4F_2 /* 0b0111 = fCK_INT/4, N=2 */
#elif (OW_PORT_SYSCLK_MHZ) <= 168
#define OW_PORT_IC4F_ARGS IC4F_3 /* 0b1000 = fCK_INT/32, N=3 */
#else
#define OW_PORT_IC4F_ARGS IC4F_0, IC4F_1, IC4F_3 /* 0b1011 = fCK_INT/4, N=3 */
#endif
#endif

/* --- Backend selection: onewire.h resolves OW_PORT_FAMILY_* from either the
 *     OW_PORT_TARGET_* knob or the PlatformIO / STM32CubeMX family macro
 *     (STM32F1/F0/G0/F4, plus the concrete F4 device spellings
 *     STM32F407xx / STM32F401xC / STM32F401xE); a single chain keeps the
 *     default clock (onewire.h) and the backend in sync. Add new families in
 *     both places, never here alone. */
#if defined(OW_PORT_FAMILY_F1)
#include "ow_port_f1.h"
#elif defined(OW_PORT_FAMILY_F0)
#include "ow_port_f0.h"
#elif defined(OW_PORT_FAMILY_G0)
#include "ow_port_g0.h"
#elif defined(OW_PORT_FAMILY_F4)
#include "ow_port_f4.h"
#else
#error "ow_port: no family selected (define OW_PORT_TARGET_F1, OW_PORT_TARGET_F0, OW_PORT_TARGET_G0 or OW_PORT_TARGET_F4, or a family macro such as STM32F1/STM32F0/STM32G0/STM32F4/STM32F407xx/STM32F401xC/STM32F401xE)"
#endif

#ifdef __cplusplus
}
#endif

#endif /* OW_PORT_H */