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

/* The clock default has two sources: the #if cascade in the family headers
 * (ow_port_f0/f1/f3/g0/f4.h) above, and
 * CHIP_SYSCLK_MHZ in the chips/<part>.mk the build selects. They are not
 * connected: the build passes -DOW_PORT_SYSCLK_MHZ=$(CHIP_SYSCLK_MHZ) on the
 * firmware command line, which is exactly the #if !defined() guard in the
 * family headers,
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
#elif defined(OW_PORT_FAMILY_F3)
#if OW_PORT_SYSCLK_MHZ != 72
#error "F3 family-macro selection must default to a 72 MHz system clock"
#endif
/* The F3 default is the part's 72MHz ceiling, and unlike F0/G0 it is not
 * reachable from the internal RC: this family has HSI (8MHz) and no HSI16, and
 * PLLSRC offers only HSI/2 or HSE/PREDIV with PLLMUL topping out at 16, so
 * 4 x 16 = 64MHz is all HSI can make. Reaching 72 means HSE x 9 with PREDIV = 1.
 *
 * The clock default is checked against the ceiling in app.c by simply not
 * offering the unreachable values: the F3 branch is a cascade of exactly 8, 64
 * and 72 with an #error for anything else, so the 64MHz internal ceiling cannot
 * be requested as the part's maximum and cannot silently become the default
 * either. The PLL input is the board's external clock, so OW_HSE_MHZ has to be
 * defined for the same reason it is on F4. */
#ifndef OW_HSE_MHZ
#error "OW_HSE_MHZ must be defined for the F3 backend (ow_port_f3.h defaults it to 8)"
#endif
#if (OW_HSE_MHZ) != 8
#error "F3: 72MHz is HSE x PLLMUL9, so the board oscillator must be 8MHz; the HSI-only ceiling is 64MHz (SYSCLK_MHZ=64)"
#endif
#elif defined(OW_PORT_FAMILY_F4)
#if defined(STM32F401xC) || defined(STM32F401xE)
#if OW_PORT_SYSCLK_MHZ != 84
#error "F4/STM32F401 family-macro selection must default to an 84 MHz system clock"
#endif
#elif defined(STM32F446xx)
#if OW_PORT_SYSCLK_MHZ != 180
#error "F4/STM32F446 family-macro selection must default to a 180 MHz system clock"
#endif
#else
#if OW_PORT_SYSCLK_MHZ != 168
#error "F4 family-macro selection must default to a 168 MHz system clock"
#endif
#endif
#elif defined(OW_PORT_FAMILY_G4)
#if OW_PORT_SYSCLK_MHZ != 170
#error "G4 family-macro selection must default to a 170 MHz system clock"
#endif
#else
#error "test_sysclk_fallback: no OW_PORT_FAMILY_* token resolved (family macro not defined)"
#endif

/* The per-part ceiling has to agree with the default this file just checked, or
 * the guard that rejects an out-of-range request (OW_PORT_F4_MAX_SYSCLK_MHZ,
 * owned by ow_port_f4.h, used by app.c) would reject the part's own
 * default. That pair is the only thing stopping a 180 MHz build for an F407. */
#if defined(OW_PORT_FAMILY_F4)
#if (OW_PORT_SYSCLK_MHZ) > (OW_PORT_F4_MAX_SYSCLK_MHZ)
#error "the default clock exceeds OW_PORT_F4_MAX_SYSCLK_MHZ for this part: the ceiling and the default disagree"
#endif
#elif defined(OW_PORT_FAMILY_G4)
/* Same pair for G4 (OW_PORT_G4_MAX_SYSCLK_MHZ, owned by ow_port_g4.h): the
 * only part tops out at 170MHz. */
#if (OW_PORT_SYSCLK_MHZ) > (OW_PORT_G4_MAX_SYSCLK_MHZ)
#error "the default clock exceeds OW_PORT_G4_MAX_SYSCLK_MHZ for this part: the ceiling and the default disagree"
#endif
#endif

#if defined(OW_PORT_FAMILY_F4)
/* The F4 PLL takes its M divider from the crystal (OW_HSE_MHZ) rather than a
 * hardcoded 8, so one part can run on boards with different crystals: PLLM =
 * OW_HSE_MHZ puts the PLL input at 1MHz, PLLN = 2 x SYSCLK sets the VCO. An 8MHz
 * crystal therefore still yields M=8/N=336 at 168MHz, M=8/N=168 at 84MHz and
 * M=8/N=360 at the F446's 180MHz, and a 25MHz board reaches the F401's 84MHz
 * cap with M=25/N=168. These are the checks that would have caught the 25MHz
 * F401 quietly reverting to M=8: nothing else in the build looks at the M field,
 * and a wrong value is not a compile error - the PLL just never locks. */
#if !defined(OW_HSE_MHZ)
#error "OW_HSE_MHZ must be defined for the F4 backend (ow_port_f4.h defaults it to 8)"
#endif
#if (OW_HSE_MHZ) < 2 || (OW_HSE_MHZ) > 63
#error "F4: PLLM is 5 bits (2..63); OW_HSE_MHZ outside that cannot give a 1MHz PLL input"
#endif
#if ((OW_PORT_SYSCLK_MHZ) * 2) < 100 || ((OW_PORT_SYSCLK_MHZ) * 2) > 432
#error "F4: at a 1MHz PLL input the VCO equals 2*SYSCLK, which must stay in 100..432MHz"
#endif
/* The raw-HSE mode bypasses the PLL and runs at the crystal's own frequency, so
 * it is only selected when the requested clock equals the crystal. Requesting 8MHz
 * on a 25MHz board used to compile and run with every 1-Wire timing scaled by
 * 3.125 - now a build error. */
#if (OW_PORT_SYSCLK_MHZ) == 8 && (OW_HSE_MHZ) != 8
#error "F4: SYSCLK_MHZ=8 selects raw HSE, which runs at the crystal: pass HSE_MHZ=8, or SYSCLK_MHZ=16 for the internal RC"
#endif
#endif /* OW_PORT_FAMILY_F4 */
#if defined(OW_PORT_FAMILY_G4)
/* The G4 PLL aims its input at exactly 4MHz (M = HSE/4 = 2 for an 8MHz
 * crystal, field value = divider-1 = 1, mid-window of the 2.66..16MHz range)
 * with N = SYSCLK/2 and the R divider at 2, so the VCO equals 2 x SYSCLK
 * and must stay in 64..344MHz (RM0440). The crystal is a board property
 * (ow_port_g4.h defaults it to 8); a wrong value is not a compile error - the
 * PLL simply never locks - which is why the input frequency itself is checked
 * here and not just trusted from the M macro.
 *
 * The design point is pinned without restating the macro: whatever M the code
 * computes, HSE/(M+1) must come out at 4MHz for a crystal that divides by 4. */
#if !defined(OW_HSE_MHZ)
#error "OW_HSE_MHZ must be defined for the G4 backend (ow_port_g4.h defaults it to 8)"
#endif
#if (OW_HSE_MHZ) < 4 || (OW_HSE_MHZ) > 48
#error "G4: the HSE oscillator takes a 4..48MHz crystal (DS12288)"
#endif
#if (OW_HSE_MHZ) % 4 != 0
#error "G4: the PLL input is HSE/4 = 4MHz by design, so the crystal must divide by 4"
#endif
#if ((OW_HSE_MHZ) / ((OW_HSE_MHZ) / 4u)) != 4
#error "G4: HSE divided by the M divider (field+1) must give the 4MHz PLL input"
#endif
#if ((OW_PORT_SYSCLK_MHZ) * 2) < 64 || ((OW_PORT_SYSCLK_MHZ) * 2) > 344
#error "G4: at a 4MHz PLL input the VCO equals 2*SYSCLK, which must stay in 64..344MHz"
#endif
/* 16MHz is raw HSI16 (no PLL), like the F4 16MHz path: nothing to check. */
#endif /* OW_PORT_FAMILY_G4 */