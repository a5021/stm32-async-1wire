/* ============================================================
 *  test_sysclk_fallback.c - Family-macro backend selection check
 *
 *  Compile-only (no test harness): verifies that selecting a family
 *  through the raw family macro (STM32F1/F0/G0) — the path
 *  PlatformIO / STM32CubeMX use, without the OW_PORT_TARGET_* knob —
 *  resolves both the OW_PORT_FAMILY_* token and the matching default
 *  OW_PORT_SYSCLK_MHZ. Guards against a family gaining a backend today
 *  and a wrong clock default tomorrow (see ow_port.h backend selection).
 *  Built per OW_TARGET by the test-clocks Makefile target.
 * ============================================================ */

#include "ow_port.h"

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
#else
#error "test_sysclk_fallback: no OW_PORT_FAMILY_* token resolved (family macro not defined)"
#endif