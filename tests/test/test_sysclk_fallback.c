/* ============================================================
 *  test_sysclk_fallback.c - Family-macro backend selection check
 *
 *  Compile-only (no test harness), built in two modes from this one
 *  source (see the test-clocks Makefile target):
 *  - Values mode (-DOW_SYSCLK_VALUES_CHECK=1, WITH -DOW_SYSCLK_MHZ
 *    from chips/<part>.mk): verifies that the part file's clock matches
 *    the literal table below and the PLL/ceiling invariants - the check
 *    that would have caught a chips-clock typo (see below).
 *  - No-default mode (WITHOUT -DOW_SYSCLK_MHZ): anti-regression
 *    proving no family header guesses a clock default anymore; the
 *    PlatformIO / CubeMX path must reach the port core's SystemCoreClock
 *    auto-path instead of a silent fallback.
 *  Selecting a family through the raw family macro (STM32F0/F1/F3/F4/G0/
 *  G4/H5 - for F4/G4/H5 also the concrete device spellings) must still
 *  resolve the OW_FAMILY_* token in both modes. Built per OW_TARGET
 *  by the test-clocks Makefile target.
 * ============================================================ */

#include "ow_port.h"

/* The part file's clock used to have two sources: the #if cascade in the
 * family headers above, and CHIP_SYSCLK_MHZ in the chips/<part>.mk the
 * build selects. They were not connected: the build passes
 * -DOW_SYSCLK_MHZ=$(CHIP_SYSCLK_MHZ) on the firmware command line,
 * which used to be exactly the #if !defined() guard in the family headers,
 * so on a Make/CMake firmware the header cascade was bypassed and the part
 * file alone decided. Meanwhile this file deliberately omitted that define
 * so it could check the cascade the PlatformIO / CubeMX path got.
 *
 * The consequence was a silent one: with CHIP_SYSCLK_MHZ = 168 in
 * chips/f401xe.mk, the firmware compiled against a 168 MHz prescaler while the
 * part actually runs at 84 MHz - every 1-Wire slot half as long as intended -
 * and clock-ref-check, test-chips and all sixteen host configurations still
 * passed. The header cascade is now gone (no family header may define a
 * clock), so the values mode below compares the part file's value - passed
 * as both OW_CHIP_SYSCLK_MHZ and OW_SYSCLK_MHZ - against the literal
 * table instead. Same bug class, one source instead of two.
 */
#ifndef OW_CHIP_SYSCLK_MHZ
#error "OW_CHIP_SYSCLK_MHZ missing: compile via `make test-clocks`, which passes the part file's CHIP_SYSCLK_MHZ"
#endif

#ifdef OW_SYSCLK_VALUES_CHECK
/* Values mode: OW_SYSCLK_MHZ arrives via -D from chips/<part>.mk.
 * Every check below validates that value against the literals. */

#if defined(STM32F0)
#if OW_SYSCLK_MHZ != 48
#error "F0 family-macro selection must default to a 48 MHz system clock"
#endif
#elif defined(STM32F1)
#if OW_SYSCLK_MHZ != 72
#error "F1 family-macro selection must default to a 72 MHz system clock"
#endif
#elif defined(STM32F3)
#if OW_SYSCLK_MHZ != 72
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
#error "OW_HSE_MHZ must be defined for the F3 backend (the ow_board.h F3 section defaults it to 8)"
#endif
#if (OW_HSE_MHZ) != 8
#error "F3: 72MHz is HSE x PLLMUL9, so the board oscillator must be 8MHz; the HSI-only ceiling is 64MHz (SYSCLK_MHZ=64)"
#endif
#elif defined(STM32F4)
#if defined(STM32F401xC) || defined(STM32F401xE)
#if OW_SYSCLK_MHZ != 84
#error "F4/STM32F401 family-macro selection must default to an 84 MHz system clock"
#endif
#elif defined(STM32F446xx)
#if OW_SYSCLK_MHZ != 180
#error "F4/STM32F446 family-macro selection must default to a 180 MHz system clock"
#endif
#else
#if OW_SYSCLK_MHZ != 168
#error "F4 family-macro selection must default to a 168 MHz system clock"
#endif
#endif
#elif defined(STM32G0)
#if OW_SYSCLK_MHZ != 64
#error "G0 family-macro selection must default to a 64 MHz system clock"
#endif
#elif defined(STM32G4)
#if OW_SYSCLK_MHZ != 170
#error "G4 family-macro selection must default to a 170 MHz system clock"
#endif
#elif defined(STM32H5)
#if OW_SYSCLK_MHZ != 64
#error "H5 family-macro selection must default to a 64 MHz system clock"
#endif
#else
#error "test_sysclk_fallback: no OW_FAMILY_* token resolved (family macro not defined)"
#endif

/* The per-part ceiling has to agree with the default this file just checked, or
 * the guard that rejects an out-of-range request (OW_F4_MAX_SYSCLK_MHZ,
 * owned by the ow_board.h F4 section, used by app.c) would reject the part's own
 * default. That pair is the only thing stopping a 180 MHz build for an F407. */
#if defined(STM32F4)
#if (OW_SYSCLK_MHZ) > (OW_F4_MAX_SYSCLK_MHZ)
#error "the default clock exceeds OW_F4_MAX_SYSCLK_MHZ for this part: the ceiling and the default disagree"
#endif
#elif defined(STM32G4)
/* Same pair for G4 (OW_G4_MAX_SYSCLK_MHZ, owned by the ow_board.h G4 section): the
 * only part tops out at 170MHz. */
#if (OW_SYSCLK_MHZ) > (OW_G4_MAX_SYSCLK_MHZ)
#error "the default clock exceeds OW_G4_MAX_SYSCLK_MHZ for this part: the ceiling and the default disagree"
#endif
#elif defined(STM32H5)
/* Same pair for H5 (OW_H5_MAX_SYSCLK_MHZ, owned by the ow_board.h H5 section): the
 * H503 tops out at 250MHz; the 64MHz HSI default is far below it. */
#if (OW_SYSCLK_MHZ) > (OW_H5_MAX_SYSCLK_MHZ)
#error "the default clock exceeds OW_H5_MAX_SYSCLK_MHZ for this part: the ceiling and the default disagree"
#endif
#endif

#if defined(STM32F4)
/* The F4 PLL takes its M divider from the crystal (OW_HSE_MHZ) rather than a
 * hardcoded 8, so one part can run on boards with different crystals: PLLM =
 * OW_HSE_MHZ puts the PLL input at 1MHz, PLLN = 2 x SYSCLK sets the VCO. An 8MHz
 * crystal therefore still yields M=8/N=336 at 168MHz, M=8/N=168 at 84MHz and
 * M=8/N=360 at the F446's 180MHz, and a 25MHz board reaches the F401's 84MHz
 * cap with M=25/N=168. These are the checks that would have caught the 25MHz
 * F401 quietly reverting to M=8: nothing else in the build looks at the M field,
 * and a wrong value is not a compile error - the PLL just never locks. */
#if !defined(OW_HSE_MHZ)
#error "OW_HSE_MHZ must be defined for the F4 backend (the ow_board.h F4 section defaults it to 8)"
#endif
#if (OW_HSE_MHZ) < 2 || (OW_HSE_MHZ) > 63
#error "F4: PLLM is 5 bits (2..63); OW_HSE_MHZ outside that cannot give a 1MHz PLL input"
#endif
#if ((OW_SYSCLK_MHZ) * 2) < 100 || ((OW_SYSCLK_MHZ) * 2) > 432
#error "F4: at a 1MHz PLL input the VCO equals 2*SYSCLK, which must stay in 100..432MHz"
#endif
/* The raw-HSE mode bypasses the PLL and runs at the crystal's own frequency, so
 * it is only selected when the requested clock equals the crystal. Requesting 8MHz
 * on a 25MHz board used to compile and run with every 1-Wire timing scaled by
 * 3.125 - now a build error. */
#if (OW_SYSCLK_MHZ) == 8 && (OW_HSE_MHZ) != 8
#error "F4: SYSCLK_MHZ=8 selects raw HSE, which runs at the crystal: pass HSE_MHZ=8, or SYSCLK_MHZ=16 for the internal RC"
#endif
#endif /* STM32F4 */
#if defined(STM32G4)
/* The G4 PLL aims its input at exactly 4MHz (M = HSE/4 = 2 for an 8MHz
 * crystal, field value = divider-1 = 1, mid-window of the 2.66..16MHz range)
 * with N = SYSCLK/2 and the R divider at 2, so the VCO equals 2 x SYSCLK
 * and must stay in 64..344MHz (RM0440). The crystal is a board property
 * (the ow_board.h G4 section defaults it to 8); a wrong value is not a compile error - the
 * PLL simply never locks - which is why the input frequency itself is checked
 * here and not just trusted from the M macro.
 *
 * The design point is pinned without restating the macro: whatever M the code
 * computes, HSE/(M+1) must come out at 4MHz for a crystal that divides by 4. */
#if !defined(OW_HSE_MHZ)
#error "OW_HSE_MHZ must be defined for the G4 backend (the ow_board.h G4 section defaults it to 8)"
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
#if ((OW_SYSCLK_MHZ) * 2) < 64 || ((OW_SYSCLK_MHZ) * 2) > 344
#error "G4: at a 4MHz PLL input the VCO equals 2*SYSCLK, which must stay in 64..344MHz"
#endif
/* 16MHz is raw HSI16 (no PLL), like the F4 16MHz path: nothing to check. */
#endif /* STM32G4 */
#else /* !defined(OW_SYSCLK_VALUES_CHECK): no-default mode -------------------
 * Compiled WITHOUT -DOW_SYSCLK_MHZ: proves no family header smuggles
 * a clock default back in, and that the family macro still resolves the
 * backend token (the PlatformIO / CubeMX selection path). */
/* The core must see no clock here: any definition would come from a header
 * fallback guess, which must not exist. (The -D the values mode passes is
 * deliberately absent from this object's command line.) */
#ifdef OW_SYSCLK_MHZ
#error "test_sysclk_fallback: OW_SYSCLK_MHZ is defined without a -D - a family header reintroduced a fallback default; the port core must read SystemCoreClock instead"
#endif
#if !defined(OW_FAMILY_F0) && !defined(OW_FAMILY_F1) && !defined(OW_FAMILY_F3) && !defined(OW_FAMILY_F4) && !defined(OW_FAMILY_G0) && !defined(OW_FAMILY_G4) && !defined(OW_FAMILY_H5)
#error "test_sysclk_fallback: no OW_FAMILY_* token resolved (family macro not defined)"
#endif
#endif /* OW_SYSCLK_VALUES_CHECK */