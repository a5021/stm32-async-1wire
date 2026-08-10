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

- The public API is strictly non-blocking: the high-level interface
  (`ds18b20_init()`, `ds18b20_poll()`, `ds18b20_select()`, the weak callbacks
  `ds18b20_busy()`/`ds18b20_complete()`) plus the `ds18b20_search_*` family
  and the CRC utility `ds18b20_crc8()`. The internal 1-Wire bus helpers
  (`ds18b20_bus_*`) and the Search ROM state machine live inside the library
  (`src/ds18b20.c`).
- The non-blocking device search is driven from the main loop exactly like the
  measurement state machine: each `ds18b20_search_poll()` performs one
  hardware operation. It filters by `DS18B20_FAMILY_CODE`, validates the ROM
  CRC and forces a timer update event before handing back to `ds18b20_poll()`.
- UART TX is now fully non-blocking: `uart_tx_enqueue_byte()` drops a byte when
  the ring buffer is full instead of busy-waiting, and the blocking
  `uart_tx_flush()` was removed.
- The 1-Wire line is now released to idle HIGH purely in hardware after every
  transaction: DMA-fed writes append a trailing 0 to the CCR1 feed (last-slot
  CC4 event) and direct-write/capture operations use an OC1PE preload of 0,
  both applied exactly when the one-pulse timer stops. The software
  `T1.CCR1 = 0` in `ds18b20_bus_done()` was removed, so the bus idles HIGH
  between slots regardless of RTOS scheduling latency (verified: 5/5 devices
  found with no software release).
- Protocol constants (`DS18B20_SEARCH_ROM`, `DS18B20_MATCH_ROM`,
  `DS18B20_CONVERT_T`, `DS18B20_READ_SCRATCHPAD`, `DS18B20_BITS_PER_BYTE`)
  are exported by the header for use with the public API and host tests.
- DMA transfer count sized to `slots-1` instead of using a sentinel value.
- Removed the global `#define CR` peripheral alias from `inc/macro.h`: it
  collided with the `RCC_TypeDef.CR` field name after CMSIS headers were
  included, expanding the field into a pointer that shifted every RCC register
  offset on 64-bit host builds.
- Added a host test suite (`make test`): the driver is compiled as a single
  translation unit against a TIM1/DMA behavioural model and a register mock.
  118 tests cover the state machine, device search, CRC-8, pulse encoding,
  presence detection, scratchpad decode, temperature conversion, timing and
  bus release. The suite runs in CI.

### Removed

- Legacy low-level blocking 1-Wire primitives (`ds18b20_reset()`,
  `ds18b20_write_bit()`, `ds18b20_read_bit()`, `ds18b20_write_byte()`,
  `ds18b20_read_byte()`, `ds18b20_restore()`) were removed from the public
  API. They busy-waited on hardware completion and were the only blocking
  path in the driver. Device enumeration is now handled exclusively by the
  non-blocking `ds18b20_search_*` family; `ds18b20_crc8()` is kept as a
  public utility.

### Fixed

- The non-blocking measurement state machine never started after a device
  search: the search clears the timer update flag on every operation, which
  left the driver idling in state 0 forever waiting for a UIF that never
  arrived. The search now forces a timer update event on completion so the
  first `ds18b20_poll()` call begins a measurement cycle immediately.
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
