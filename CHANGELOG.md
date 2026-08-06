# Changelog

All notable changes to the Non-Blocking DS18B20 Driver for STM32F103C8T6
are documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [Unreleased]

### Added
- Blocking 1-Wire device search (`ds18b20_search_devices()`): enumerates all
  DS18B20 devices on the bus using the Search ROM command (0xF0) with the
  last-discrepancy algorithm and CRC-8 validation, and reports each device's
  64-bit ROM address via a callback. The `demo2` example scans the bus once
  at startup and prints every found device before starting normal measurements.
- Per-device addressing (`ds18b20_select()`): the non-blocking measurement
  path can now target one specific DS18B20 by its 64-bit ROM address using
  the Match ROM command (0x55). Passing NULL keeps the legacy Skip ROM
  (single-sensor) behaviour. The demo measures a single device directly, or
  cycles through all found devices in turn when more than one is present.
- Two example applications, selected with `make APP=demo|demo2`:
  - `src/demo.c` — unconditional polling of a single sensor via Skip ROM
    (the original, pre-search behaviour);
  - `src/demo2.c` — startup device search plus sequential round-robin
    polling of every sensor found (up to `DS18B20_SEARCH_MAX_DEVICES`).

### Fixed

- The non-blocking measurement state machine never started after the blocking
  device search: the search clears the timer update flag on every operation,
  which left the driver idling in state 0 forever waiting for a UIF that never
  arrived. `ds18b20_search_devices()` now leaves a pending update flag so the
  first `ds18b20_poll()` call begins a measurement cycle immediately.

## [1.0.0] - 2026-08-05

### Added
- Non-blocking, interrupt-free DS18B20 driver for STM32F103C8T6:
  TIM1 Output Compare + Input Capture with DMA and a hardware state machine
  handle all 1-Wire timing (reset, write/read slots, 750 ms conversion wait).
- `HSI_8MHZ` conditional build variant for running at 8 MHz without an
  external crystal or PLL.
- Automatic download of CMSIS core/device dependencies in the Makefile
  (`make download-deps`, `make clean-deps`).
- Non-blocking, poll-driven UART debug output with a ring buffer.
- Weak callbacks `ds18b20_busy()` and `ds18b20_complete()` for LED status
  and measurement results.
- VSCode workspace configuration: build tasks, J-Link/ST-Link debug
  configurations, IntelliSense paths and SVD peripheral views.
- CI pipeline: clang-format, cppcheck, GCC `-fanalyzer`, and an ARM build
  workflow with toolchain/dependency caching.
- GCC/Binutils version detection and `--no-warn-rwx-segments` handling in
  the build system.
- `curl` fallback when `wget` is not available.

### Fixed
- BITS macro bug and missing `#endif` in `macro.h`; removed ~220 lines of
  dead, duplicated code.
- Lost negative sign for temperatures between -0.5 and -0.1 °C in UART
  output.
- 1-Wire slot timing: `ONE_PULSE=5`, slot formula 5+60+5 = 70 µs.
- Undefined behavior in `uart_write_int`.
- Missing `__DSB()` memory barrier in the driver initialization.
- Extern C guard in `macro.h` (missing quotes).
- cppcheck false positives from CMSIS headers.

### Changed
- Callback rename: `led_control` -> `busy`, `temp_ready` -> `complete`.
- One-letter macros renamed to descriptive names; unified TIM1 SR access.
- CMSIS dependency restructure, linker script rewrite and licensing cleanup.
- README restructured: hardware connections, architecture, API reference,
  troubleshooting, build and flash instructions.

[1.0.0]: https://github.com/a5021/non-blocking-ds18B20-driver-for-stm32f103c8t6/releases/tag/v1.0.0
