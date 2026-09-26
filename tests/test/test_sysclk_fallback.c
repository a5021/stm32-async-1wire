/* ============================================================
 *  test_sysclk_fallback.c - Family-macro backend selection check
 *
 *  Compile-only (no test harness): verifies that selecting a family
 *  through the raw family macro (STM32F1/F0/G0/F4 — for F4 also the
 *  concrete device spellings) — the path PlatformIO / STM32CubeMX use,
 *  without the OW_PORT_TARGET_* knob — resolves both the OW_PORT_FAMILY_*
 *  token and the matching default OW_PORT_SYSCLK_MHZ. Guards against a
 *  family gaining a backend today and a wrong clock default tomorrow
 *  (see ow_port.h backend selection).
 *  Built per OW_TARGET by the test-clocks Makefile target.
 * ============================================================ */

#include "ow_port.h"

/* The clock default has two sources: the #if cascade in onewire.h above, and
 * CHIP_SYSCLK_MHZ in the chips/<part>.mk the build selects. They are not
 * connected: the build passes -DOW_PORT_SYSCLK_MHZ=$(CHIP_SYSCLK_MHZ) on the
 * firmware command line, which is exactly the #if !defined() guard in onewire.h,
 * so on a Make/CMake firmware the header cascade is bypassed and the part file
 * alone decides. Meanwhile this file deliberately omits that define so it can
 * check the cascade the PlatformIO / CubeMX path gets.
 *
 * The consequence was a silent one: with CHIP_SYSCLK_MHZ = 168 in
 * chips/f401xe.mk, the firmware compiled against a 168 MHz prescaler while the
 * part actually runs at 84 MHz - every 1-Wire slot half as long as intended -
 * and clock-ref-check, test-chips and all sixteen host configurations still
 * passed. So compare the two explicitly. The build passes the part file's value
 * as OW_CHIP_SYSCLK_MHZ precisely so it can be compared here.
 */
#ifndef OW_CHIP_SYSCLK_MHZ
#error "OW_CHIP_SYSCLK_MHZ missing: compile via `make test-clocks`, which passes the part file's CHIP_SYSCLK_MHZ"
#endif
#if OW_CHIP_SYSCLK_MHZ != OW_PORT_SYSCLK_MHZ
#error "chips/<part>.mk CHIP_SYSCLK_MHZ disagrees with the ow_port.h family default for this part"
#endif

#if defined(OW_PORT_FAMILY_F1)
#if OW_PORT_SYSCLK_MHZ != 72
#error "F1 family-macro selection must default to a 72 MHz system clock"
#endif
#elif defined(OW_PORT_FAMILY_F0)
#if OW_PORT_SYSCLK_MHZ != 48
#error "F0 family-macro selection must default to a 48 MHz system clock"
#endif
#elif defined(OW_PORT_FAMILY_G0)
#if OW_PORT_SYSCLK_MHZ != 64
#error "G0 family-macro selection must default to a 64 MHz system clock"
#endif
#elif defined(OW_PORT_FAMILY_F4)
#if defined(STM32F401xC) || defined(STM32F401xE)
#if OW_PORT_SYSCLK_MHZ != 84
#error "F4/STM32F401 family-macro selection must default to an 84 MHz system clock"
#endif
#else
#if OW_PORT_SYSCLK_MHZ != 168
#error "F4 family-macro selection must default to a 168 MHz system clock"
#endif
#endif
#else
#error "test_sysclk_fallback: no OW_PORT_FAMILY_* token resolved (family macro not defined)"
#endif