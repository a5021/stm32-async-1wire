/**
 * @file ow_config.h
 * @brief Central compile-time configuration for the 1-Wire stack.
 *
 * All genuinely tunable build constants are collected here.  Every macro
 * carries its own `#ifndef` guard so that a `-D` on the command line
 * (Makefile EXT, CMake -D, PlatformIO build_flags) overrides the default
 * without touching this file — the existing override mechanism is unchanged.
 *
 * Protocol-inherent constants (ONEWIRE_MAX_SLOTS, ONEWIRE_MAX_READ_BYTES,
 * DS18B20_RES_MIN, DS18B20_RES_MAX, DS18B20_RES_DEFAULT) and the
 * per-family system-clock default (OW_PORT_SYSCLK_MHZ, selected from the
 * OW_PORT_TARGET / STM32F* family token) remain in their respective headers
 * and are intentionally NOT listed here.
 *
 * The three feature flags are value-style: define to 1 to enable, omit or
 * set to 0 to disable.  Old -D presence-only style (-DOW_PORT_LOW_POWER
 * without =1) no longer works with these flags — always pass =1.
 */

#ifndef OW_CONFIG_H
#define OW_CONFIG_H

/* -------------------------------------------------------------------
 *  Parasite bus timing
 * ------------------------------------------------------------------- */

/**
 * @brief Compile-time flag: bus is wired for parasite power.
 *
 * When 1 the default ONEWIRE_GUARD_BAND is raised from 5 µs to 100 µs
 * to leave enough time for the bus-pull-up capacitor to charge.  The
 * Makefile TIMING= presets override GUARD_BAND directly and are unaffected.
 *
 * Example applications use this flag to automatically call
 * ds18b20_set_parasite(1) at startup — a runtime per-bus override that
 * is independent of the compile-time guard-band default.
 */
#ifndef OW_PARASITE_POWER
#define OW_PARASITE_POWER 0
#endif

/* -------------------------------------------------------------------
 *  1-Wire bit-slot timing
 *
 *  The values below are fixed constants valid on every supported clock.
 *  They are NOT clock-derived — a single universal duration is sufficient
 *  because every supported frequency has been bench-validated.
 * ------------------------------------------------------------------- */

/**
 * @brief Duration of a '1' bit write/read pulse in microseconds.
 *
 * DS18B20 requires only ≥ 1 µs and samples the slot at ≥ 15 µs after
 * its start.  The read-slot capture latency (bus RC rise + input filter
 * + timer sync) stays far below the ONEWIRE_SHORT_PULSE_MAX window on
 * all supported clocks.
 *
 * Previous releases tried a 2 µs pulse for ≤ 16 MHz compensation.
 * Hardware on STM32F030 @ 8 MHz showed that a 2 µs master pulse breaks
 * the sensor's slot decoding outright — every capture stretches past the
 * threshold regardless of the answer — while a plain 5 µs pulse
 * measures ~ 9 µs there with every input-filter variant swept (fCK_INT
 * N=2/4/8 and fDTS/4 N=8).  The short-pulse path was tuned on
 * F103 @ 8 MHz bench wiring whose slower rise is not reproduced by other
 * boards; re-validate per board before reintroducing anything similar.
 *
 * @note Hardware-validated at 5 µs on every supported clock:
 *       STM32F030 @ 48/8 MHz, STM32F103 @ 72/8 MHz and STM32G031 @ 64/16 MHz.
 */
#ifndef ONEWIRE_ONE_PULSE
#define ONEWIRE_ONE_PULSE 5
#endif

/** @brief Duration of a '0' bit write pulse in microseconds. */
#ifndef ONEWIRE_ZERO_PULSE
#define ONEWIRE_ZERO_PULSE 60
#endif

/**
 * @brief Guard interval between the end of a write/read slot and the
 *        next slot, in microseconds.
 *
 * 5 µs is sufficient for external-power buses.  When
 * OW_PARASITE_POWER is 1 the default is raised to 100 µs to leave
 * margin for the bus-pull-up capacitor (one full µs-tick period at the
 * slowest supported clock).
 */
#ifndef ONEWIRE_GUARD_BAND
#if OW_PARASITE_POWER
#define ONEWIRE_GUARD_BAND 100 /* parasite-powered bus: wider release margin */
#else
#define ONEWIRE_GUARD_BAND 5
#endif
#endif

/** @brief Upper bound (in µs) for the short-pulse detection window. */
#ifndef ONEWIRE_SHORT_PULSE_MAX
#define ONEWIRE_SHORT_PULSE_MAX 10
#endif

/* -------------------------------------------------------------------
 *  Feature flags (value-style: 0 = off, 1 = on)
 * ------------------------------------------------------------------- */

/**
 * @brief Opt-in low-power WFE sleep path.
 *
 * When 1 the TIM1 update interrupt (UIE) and SEVONPEND are armed for
 * long hardware stages (> 1 ms) so that the application can block in
 * __WFE() without an ISR; the driver itself stays non-blocking.
 * No ISR is ever installed and NVIC_EnableIRQ is never called; the
 * pending bit is cleared explicitly in ow_port_bus_done() so WFE does
 * not degrade into a busy-loop.
 *
 * Enable with -DOW_PORT_LOW_POWER=1.
 */
#ifndef OW_PORT_LOW_POWER
#define OW_PORT_LOW_POWER 0
#endif

/**
 * @brief Opt-in active-drive (push-pull) write path.
 *
 * When 1 the bus pin is temporarily switched to push-pull during
 * master-only write slots (see Bus Electrical Model).
 * Enable with -DOW_DRIVE_ACTIVE=1.
 */
#ifndef OW_DRIVE_ACTIVE
#define OW_DRIVE_ACTIVE 0
#endif

/**
 * @brief Optional per-sensor pulse-width statistics module.
 *
 * When 1 the ow_stats module collects per-sensor pulse-width min/max,
 * a global histogram and error counters; ow_stats_dump_start() streams
 * them over UART.  When 0 every inline body compiles away to nothing
 * so there is zero overhead in production builds.
 * Enable with -DOW_STATS_ENABLE=1.
 * @see ow_stats.h for the public API.
 */
#ifndef OW_STATS_ENABLE
#define OW_STATS_ENABLE 0
#endif

/* -------------------------------------------------------------------
 *  DS18B20 driver configuration
 * ------------------------------------------------------------------- */

/**
 * @brief Maximum number of DS18B20 devices tracked by the driver.
 *
 * Size of the internal device table filled by the device search; the
 * simultaneous-conversion (scan) mode uses it to address every sensor.
 * Each entry costs DS18B20_ROM_BYTES (8) bytes of static RAM.
 */
#ifndef DS18B20_MAX_DEVICES
#define DS18B20_MAX_DEVICES 8
#endif

/**
 * @brief Default inter-measurement pause in microseconds (default 5 s).
 *
 * ds18b20.c recomputes ARR and RCR from this value.  Set to 0 to
 * disable the pause (next measurement starts immediately after decode).
 * Overridable via -DDS18B20_CYCLE_PAUSE_US=(value in µs).
 */
#ifndef DS18B20_CYCLE_PAUSE_US
#define DS18B20_CYCLE_PAUSE_US 5000000
#endif

#endif /* OW_CONFIG_H */
