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

/* --- Backend selection --- */
#if defined(OW_PORT_TARGET_F1)
#include "ow_port_f1.h"
#elif defined(OW_PORT_TARGET_F0)
#include "ow_port_f0.h"
#else
#error "ow_port: no backend selected (define OW_PORT_TARGET_F1, OW_PORT_TARGET_F0, ...)"
#endif

#endif /* OW_PORT_H */