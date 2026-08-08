# Changelog

All notable changes to the Non-Blocking DS18B20 Driver for STM32F103C8T6
are documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).
## [Unreleased]

### Added

- Low-level blocking 1-Wire primitives for custom protocols such as device
  search: `ds18b20_reset()`, `ds18b20_write_bit()`, `ds18b20_read_bit()`,
  `ds18b20_write_byte()`, `ds18b20_read_byte()`, `ds18b20_restore()`, and
  `ds18b20_crc8()`. These busy-wait on hardware completion and are clearly
  separated from the non-blocking measurement path. See `demo2.c` for a
  complete Search ROM implementation built on these primitives.
- Shared application layer (`inc/app.h`, `src/app.c`): lossless UART TX ring
  buffer, `app_init()` for clock + UART + LED setup, and a default
  `ds18b20_busy()` LED indicator. Both examples now `#include "app.h"` and
  delegate hardware setup to the shared layer.
- Match ROM prefix caching: `ds18b20_select()` builds the invariant 72-slot
  Match ROM prefix once per selection, so each subsequent measurement patches
  only the last byte (8 slots instead of 80).

### Changed

- **Breaking:** `ds18b20_search_devices()` removed from the library. The
  blocking search algorithm now lives in `demo2.c` as `search_all_devices()`,
  built on the public low-level primitives. Users who need device search can
  copy and adapt the demo implementation.
- Protocol constants (`DS18B20_SEARCH_ROM`, `DS18B20_MATCH_ROM`,
  `DS18B20_CONVERT_T`, `DS18B20_READ_SCRATCHPAD`, `DS18B20_ROM_BYTES`,
  `DS18B20_BITS_PER_BYTE`) moved from `src/ds18b20.c` to `inc/ds18b20.h`
  so low-level API users can build custom protocols.
- DMA transfer count sized to `slots-1` instead of using a sentinel value.

### Fixed

- The non-blocking measurement state machine never started after the blocking
  device search: the search clears the timer update flag on every operation,
  which left the driver idling in state 0 forever waiting for a UIF that never
  arrived. The search now calls `ds18b20_restore()` so the first
  `ds18b20_poll()` call begins a measurement cycle immediately.
- The device search reported every 1-Wire device on the bus, not just DS18B20
  temperature sensors: other families (DS2401, DS1990, etc.) were stored and
  polled as if they were DS18B20s. The search now skips any device whose ROM
  family code is not `DS18B20_FAMILY_CODE` (0x28).

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
