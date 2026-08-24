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

/* --- Compiler helpers the driver expects (portable stand-ins; real CMSIS
 *     headers define them too, so the #ifndef guards keep both paths
 *     identical) --- */
#ifndef __STATIC_FORCEINLINE
#define __STATIC_FORCEINLINE static __attribute__((always_inline)) inline
#endif
#ifndef __WEAK
#define __WEAK __attribute__((weak))
#endif

/* --- 1-Wire reset timeslot geometry (microseconds), shared by all backends.
 *     The '1'/'0' bit-slot durations live in onewire.h (ONEWIRE_ONE_PULSE,
 *     ONEWIRE_ZERO_PULSE, ONEWIRE_GUARD_BAND). --- */
#define OW_PORT_RESET_PULSE_DURATION 480u
#define OW_PORT_RESET_TIMEOUT 960u
#define OW_PORT_CAPTURE_BUF_SIZE 2u

/* onewire.h supplies the clock-derived timing constants referenced below
 * (OW_PORT_SYSCLK_MHZ and the bit-slot durations); including it here keeps
 * this header self-contained regardless of TU include order. */
#include "onewire.h"

/* --- CH4 input-capture digital filter (IC4F), one standard for every clock.
 *     Keep the filter time T_f = N × T_sample as close to ~500ns as the
 *     discrete IC4F table allows for the configured clock: that rejects
 *     sub-µs bus glitches while adding well under 1µs to read-slot captures
 *     — negligible against the ONEWIRE_SHORT_PULSE_MAX decode window (an
 *     IC4F sweep on STM32F030@8MHz decoded cleanly from fCK_INT N=2 all the
 *     way to fDTS/4 N=8):
 *       ≤ 8MHz   fCK_INT, N=4    T_f ≈ 500ns @ 8MHz
 *       ≤16MHz   fCK_INT, N=8    T_f ≈ 500ns @ 16MHz
 *       >16MHz   fDTS/4,  N=8    T_f ≈ 444..667ns @ 48..72MHz
 *     Backends feed the macro into TIM_CCMR2(...) unchanged. --- */
#if (OW_PORT_SYSCLK_MHZ) <= 8
#define OW_PORT_IC4F_ARGS IC4F_1 /* fCK_INT, N=4 */
#elif (OW_PORT_SYSCLK_MHZ) <= 16
#define OW_PORT_IC4F_ARGS IC4F_0, IC4F_1 /* fCK_INT, N=8 */
#else
#define OW_PORT_IC4F_ARGS IC4F_0, IC4F_1, IC4F_2 /* fDTS/4, N=8 */
#endif

/* --- Backend selection --- */
#if defined(OW_PORT_TARGET_F1)
#include "ow_port_f1.h"
#elif defined(OW_PORT_TARGET_F0)
#include "ow_port_f0.h"
#elif defined(OW_PORT_TARGET_G0)
#include "ow_port_g0.h"
#else
#error "ow_port: no backend selected (define OW_PORT_TARGET_F1, OW_PORT_TARGET_F0, ...)"
#endif

#endif /* OW_PORT_H */