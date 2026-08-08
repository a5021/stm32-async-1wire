# Changelog

All notable changes to the Non-Blocking DS18B20 Driver for STM32F103C8T6
are documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).
## [Unreleased]

### Added

- Non-blocking device search: `ds18b20_search_start()`, `ds18b20_search_poll()`,
  `ds18b20_search_count()`. Implements the Maxim Search ROM (0xF0) algorithm as
  a compact state machine that performs exactly one hardware operation per poll
  call, then hands the timer back to the measurement path automatically. See
  `demo2.c`.
- Shared application layer (`inc/app.h`, `src/app.c`): non-blocking UART TX ring
  buffer, `app_init()` for clock + UART + LED setup, and a default
  `ds18b20_busy()` LED indicator. Both examples now `#include "app.h"` and
  delegate hardware setup to the shared layer.
- Match ROM prefix caching: `ds18b20_select()` builds the invariant 72-slot
  Match ROM prefix once per selection, so each subsequent measurement patches
  only the last byte (8 slots instead of 80).

### Changed

- **Breaking:** the public API is now a small high-level interface only:
  `ds18b20_init()`, `ds18b20_poll()`, `ds18b20_select()`, the weak callbacks
  `ds18b20_busy()`/`ds18b20_complete()`, and the `ds18b20_search_*` family.
  All low-level 1-Wire bus primitives (`ds18b20_bus_*`), `ds18b20_crc8()` and
  the Search ROM state machine moved inside the library (`src/ds18b20.c`) and
  became `static`; the separate `inc/ds18b20_search.h` /
  `src/ds18b20_search.c` files were removed.
- **Breaking:** the blocking low-level primitives (`ds18b20_reset()`,
  `ds18b20_write_bit()`, `ds18b20_read_bit()`, `ds18b20_write_byte()`,
  `ds18b20_read_byte()`) were removed.
- UART TX is now fully non-blocking: `uart_tx_enqueue_byte()` drops a byte when
  the ring buffer is full instead of busy-waiting, and the blocking
  `uart_tx_flush()` was removed.
- Protocol constants (`DS18B20_SEARCH_ROM`, `DS18B20_MATCH_ROM`,
  `DS18B20_CONVERT_T`, `DS18B20_READ_SCRATCHPAD`, `DS18B20_BITS_PER_BYTE`)
  are now private to `src/ds18b20.c`; the public header keeps only
  `DS18B20_FAMILY_CODE`, `DS18B20_ROM_BYTES` and the error codes.
- DMA transfer count sized to `slots-1` instead of using a sentinel value.

### Fixed

- The non-blocking measurement state machine never started after a blocking
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
