/**
 * @file ow_port.h
 * @brief Platform port layer for the non-blocking 1-Wire engine
 *
 * The 1-Wire layer (onewire.c) is written against this thin, target-agnostic
 * interface; the DS18B20 driver (ds18b20.c) uses only the onewire_* API on top
 * of it. Each target provides the
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
 *     ONEWIRE_ZERO_PULSE, ONEWIRE_GUARD_BAND). These are the port's own
 *     schedule parameters, published as port contract: onewire.c and the
 *     tests consume them through this interface, so this is not protocol
 *     leaking down — it is the interface. --- */
#define OW_PORT_RESET_PULSE_DURATION 480u
#define OW_PORT_RESET_TIMEOUT 960u
#define OW_PORT_CAPTURE_BUF_SIZE 2u

/* Long-stage threshold shared by every schedule path: a running, unfinished
 * operation whose ARR*(RCR+1) exceeds this is "long" and may be slept through
 * with ow_port_sleep_until_done() instead of busy-polled. One rule for
 * capture, feed and timer waits — 1 ms for every stage. */
#define OW_PORT_LONG_STAGE_US 1000u

/* Optional logic-analyzer marker hook, empty by default.  Backends that have
 * one may define OW_PORT_MARKER_TOGGLE() to pulse a spare GPIO so a decoder
 * can find the start of a merged operation pass.  The core never calls it, so
 * this is a contract for the backend's own use, not a required callback.
 * Lives above the onewire.h include on purpose: onewire.h pulls in the
 * backend, which is included while this header's guard is already active, so
 * anything below that include is invisible to backends (circular inclusion).
 * A backend calling this hook needs the default defined before that point. */
#ifndef OW_PORT_MARKER_TOGGLE
#define OW_PORT_MARKER_TOGGLE() ((void)0)
#endif

/* The selected backend (onewire.h includes it) supplies OW_PORT_SYSCLK_MHZ
 * (used for the timer prescaler and the IC4F selection below) and ow_pulse_t
 * for the backend signatures; the bit-slot durations come from ow_config.h.
 * Including onewire.h here keeps this header self-contained regardless of the
 * TU include order. */
#include "onewire.h"

/* The byte-read capture path (ow_port_read_data, width==8) stores CCR4's
 * least-significant byte via the DMA memory-store width.  How the offsets are
 * expressed differs per backend: the basic-DMA backends keep PSIZE=16 (CCR4 is
 * a 16-bit halfword) and let MSIZE=8 store only the low byte, while the F4
 * backend runs direct mode with PSIZE=MSIZE=8, so the DMA fetches just the low
 * byte of CCR4.  Both are lossless only while every slot capture stays below
 * 256 microseconds: the counter runs 0..ARR, so the maximum capture value is
 * ONEWIRE_ONE_PULSE + ONEWIRE_ZERO_PULSE + ONEWIRE_GUARD_BAND.  Enforce this
 * globally across all backends. */
_Static_assert((ONEWIRE_ONE_PULSE + ONEWIRE_ZERO_PULSE + ONEWIRE_GUARD_BAND) < 256u,
               "8-bit read capture (MSIZE=8) would truncate slot durations");

/* --- OC3 output-compare preload (OC3PE) knob ---
 * The preload bit in TIM_CCMR2 keeps the active CCR3 stable through a slot:
 * with OC3PE the value written before CEN only enters the output at the
 * terminal update event, which is the "line released to idle HIGH in
 * hardware" guarantee (see README, "Bus Idle Behaviour"). The write/capture
 * paths that rely on it (capture, read_pair, single-slot write) gate the bit
 * behind this flag so a bench can sweep OC3PE on/off with
 * -DOW_PORT_OC3PE=0/-DOW_PORT_OC3PE=1 without editing code. The DMA-fed paths
 * (feed, write_then_read) leave it off unconditionally: their reload must act
 * immediately, which preload would break.
 *
 * Default 1 (preload on) identical to the historical hard-coded behaviour.
 *
 * OW_PORT_OC3PE_ARGS (the CCMR2 bit value OR-ed into the mask at the affected
 * call sites) derives from it, defined where it is used: ow_port_tim_dma.h
 * for F0/F1/F3/G0 and ow_port_f4.h for F4, by the same #ifndef pattern as
 * OW_PORT_IC4F_ARGS. */

/* --- CH4 input-capture digital filter (IC4F) ladder moved out ---
 * The clock -> ICxF selection now lives where it is used, so it runs before
 * any backend body that feeds TIM_CCMR2(..., OW_PORT_IC4F_ARGS) is reached no
 * matter which header the TU includes first: the shared core owns it for
 * every family (port/common/ow_port_tim_dma.h), no backend overrides it.
 * See there for the decoded tier table and the bench-sweep override. */

/* --- Backend selection happened in onewire.h ---
 * The family chain and the per-family \#include branches live there; this
 * header is reached through it, so the family token, the backend and its
 * facts (ow_pulse_t, the clock defaults and the low-power IRQ mapping) are
 * already in scope here.  Nothing to add.  (That chain is the single place
 * for the header-level selection; the build systems name families too -
 * Makefile, CMakeLists.txt, examples/app/app.c, chips/ and the host tests.) */

#ifdef __cplusplus
}
#endif

#endif /* OW_PORT_H */
