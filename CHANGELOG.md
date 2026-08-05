# Changelog

All notable changes to the Non-Blocking DS18B20 Driver for STM32F103C8T6
are documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

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
