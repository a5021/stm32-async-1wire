/**
 * @file ow_config.h
 * @brief 1-Wire timing constants and clock configuration
 *
 * Minimal config header shared between the bus layer (onewire.c) and the
 * port layer (ow_port_*.h). Contains only compile-time defines and extern
 * runtime timing variables — no types, no function declarations, no
 * dependency on protocol semantics. This breaks the former circular
 * include: ow_port.h included onewire.h (for clock/pulse constants),
 * while onewire.h logically sits above the port layer.
 */

#ifndef OW_CONFIG_H
#define OW_CONFIG_H

#include <stdint.h>

/** @brief System clock frequency in MHz after application clock setup.
 *  Single source of truth for every clock-dependent setting: the timer
 *  prescaler (1µs ticks), the input-capture filter selection and the
 *  '1'-slot pulse width below all derive from it. Family defaults are
 *  provided here; override via -DOWN_PORT_SYSCLK_MHZ=N (see app.c for the
 *  clock sources available per family). */
#if !defined(OW_PORT_SYSCLK_MHZ)
#if defined(OW_PORT_TARGET_F0)
#define OW_PORT_SYSCLK_MHZ 48 /* STM32F030: HSI/2 + PLL x12 */
#elif defined(OW_PORT_TARGET_G0)
#define OW_PORT_SYSCLK_MHZ 64 /* STM32G031: HSI16 + PLL */
#else
#define OW_PORT_SYSCLK_MHZ 72 /* STM32F103: HSE + PLL x9 */
#endif
#endif

/** @brief Opt-in low-power WFE sleep: when defined, every hardware operation
 *  enables the TIM1 update interrupt (UIE) and SEVONPEND so that a pending
 *  update event wakes the core from WFE without an ISR. Disabled by default
 *  so non-low-power builds pay zero cost. Enable with -DOW_PORT_LOW_POWER. */
#ifdef OW_PORT_LOW_POWER
#if defined(OW_PORT_TARGET_F0) || defined(OW_PORT_TARGET_G0)
#define OW_PORT_TIM1_UPD_IRQn TIM1_BRK_UP_TRG_COM_IRQn
#else
#define OW_PORT_TIM1_UPD_IRQn TIM1_UP_IRQn
#endif
#endif

/** @brief Bits per byte */
#define ONEWIRE_BITS_PER_BYTE 8

/** @brief Default timing constants (microseconds)
 *  @note The port layer reads the runtime-adjusted externs (ow_*_us), not
 *        these defines. The defines provide compile-time defaults that the
 *        timing profile system overrides at init. */
#define ONEWIRE_ONE_PULSE 5
#define ONEWIRE_ZERO_PULSE 60
#define ONEWIRE_GUARD_BAND 5
#define ONEWIRE_SHORT_PULSE_MAX 10

/** @brief Runtime timing variables (adjusted by onewire_set_timing_profile) */
extern uint8_t ow_one_pulse_us;
extern uint8_t ow_zero_pulse_us;
extern uint8_t ow_guard_band_us;
extern uint8_t ow_short_pulse_max_us;

#endif /* OW_CONFIG_H */
