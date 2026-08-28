# Changelog

All notable changes to stm32-async-1wire — a non-blocking 1-Wire layer for
STM32 and the DS18B20 driver built on top of it — are documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [Unreleased]

### Documentation

- Documented the 1-Wire **bus electrical model** (open-drain signaling; the
  master never drives the line HIGH during a normal slot — `write-1` is a
  release, not a push-pull level; the parasite strong-pull-up is the only
  push-pull usage, confined to the slave-silent conversion window). Clarifies
  the hardware contract referenced by the published repository page.

### Added

- **Active-drive write path (opt-in, `-DOW_DRIVE_ACTIVE`, default off).** During
  pure-write transactions PA10 switches to push-pull so the master actively
  drives *both* bus levels (`write-0`/`write-1`), giving a stronger/faster
  write-1 than the external pull-up RC rise. Every read/reset/slave-response
  phase and the merged write+read op stay open-drain, so the slave's wired-AND
  is preserved and there is no window where the master drives HIGH while a
  slave could pull LOW. Hardware-validated on STM32F1; build/host-tested on
  F0/G0. The published default remains open-drain.
- **Selectable default timing profile** via `-DONEWIRE_TIMING_PROFILE_DEFAULT`
  (e.g. `ONEWIRE_TIMING_ROBUST` / `ONEWIRE_TIMING_SLOW`) for hardware margin
  testing, without changing the standard default.
- `test-active`, `test-active-f0`, `test-active-g0` Makefile targets exercising
  the active-drive pin-mode switching.

## [1.7.0] - 2026-08-28

### Added

- Optional signal statistics module (`inc/ow_stats.h`, `src/ow_stats.c`):
  compile-in with `-DOW_STATS_ENABLE` to collect per-sensor pulse-width
  min/max, a global 13-bucket logarithmic histogram (0–60+ µs) and error
  counters (CRC, presence, other) across measurement cycles.  The module
  hooks into `src/ds18b20.c` automatically when enabled: pulse data is
  captured before `decode_scratchpad()` overwrites the buffer, and errors
  are counted at the four error-reporting paths.  The dump is fully
  non-blocking: `ow_stats_dump_start()` initiates it and
  `ow_stats_dump_poll()` streams one line per call, blocked only on the
  UART TX register (~87 µs/byte at 115200), so a 6-sensor report completes
  in ~30 ms without overflowing the 256-byte ring buffer.  RAM cost: ~96
  bytes.  Zero overhead when `OW_STATS_ENABLE` is not defined (all stubs
  inline to nothing).
- Example application `src/demo5.c` (`make APP=demo5`): startup device
  search + sequential measurement with the `ow_stats` module.  Accumulates
  statistics over `STATS_DUMP_INTERVAL` cycles (default 100), then streams
  the full report over UART and resets for the next window.  Validated on
  STM32G031@64MHz with 6 × DS18B20 in parasite power mode — all sensors
  detected, 0 errors, pulse widths 5–32 µs.
- Example application `src/demo1.c` (`make APP=demo1`): startup device
  search + per-device polling.  Each discovered sensor is converted and read
  back individually via Match ROM (one `Convert T` per device, no broadcast
  conversion) — the minimal multi-sensor counterpart to `demo3`'s
  simultaneous scan.  Parasite power is engaged with `EXT="-DPARASITE_POWER=1"`;
  note that per-device MATCH-ROM conversion is the marginal topology on a
  parasite bus, so a broadcast convert (demo3) is preferred there.
- Timing profiles (`inc/onewire.h`, `src/onewire.c`): four selectable slot
  timings spanning fastest-to-slowest, all inside the DS18B20 1-Wire
  specification — `ONEWIRE_TIMING_FAST` (5/60/3µs, 50µs parasite guard),
  `ONEWIRE_TIMING_STANDARD` (5/60/5µs, 100µs; equals the historical defaults),
  `ONEWIRE_TIMING_SLOW` (8/90/20µs, 200µs) and `ONEWIRE_TIMING_ROBUST`
  (10/110/30µs, 250µs).  Select at runtime with
  `onewire_set_timing_profile()` (or pin a compile-time default via
  `ONEWIRE_TIMING_PROFILE_DEFAULT`); `onewire_get_timing_profile()` reports the
  active one.  The `one_pulse`/`zero_pulse`/`guard_band`/`parasite_guard_band`/
  `short_pulse_max` fields drive the timer ARR, the write low-time and the
  read-decode threshold, so each profile adapts to different wire lengths and
  sensor tolerances without touching the per-port timer code.  `onewire_init()`
  applies the default profile; `ow_set_parasite_guard()` keeps the per-profile
  external/parasite guard-band split.

### Removed

- **Breaking:** `ds18b20_read_power_supply()` / `ds18b20_read_power_supply_poll()`
  removed. Detect the bus wiring with `ds18b20_detect_parasite()` and read the
  result via `ds18b20_parasite_mode()` (the driver consumes it to engage the
  strong pull-up). This collapses the two overlapping query APIs that the 1.6.0
  parasite-power work introduced into one.

### Changed

- `demo4` reports the power-supply wiring through `ds18b20_detect_parasite()` /
  `ds18b20_parasite_mode()` instead of the removed `ds18b20_read_power_supply()`.

## [1.6.1]

### Fixed

- The `SYSCLK_MHZ` build knob was dead: the Makefile passed
  `-DOWN_PORT_SYSCLK_MHZ` instead of `-DOW_PORT_SYSCLK_MHZ`, so every
  non-default clock build silently used the family default frequency.
  This also means the v1.6.0 assets named `*_f0_8mhz` and `*_g0_16mhz`
  actually contained 48MHz / 64MHz firmware. The macro name is now spelled
  correctly (`Makefile`, verified by disassembly across clock variants).

### Changed

- `ONEWIRE_ONE_PULSE` is a single universal value (5µs) again: the ≤16MHz
  compensation that shortened it to 2µs is removed. Bench hardware on
  STM32F030F4P6 @8MHz showed that a 2µs master pulse breaks DS18B20 read-slot
  decoding outright (every capture stretches past the '0'/'1' threshold
  regardless of the sensor answer), while a plain 5µs pulse measures ~9µs
  there with every input-capture filter variant swept (fCK_INT N=2/4/8,
  fDTS/4 N=8) — so the capture chain, not the pulse, carries the slow-clock
  margin. Validated on hardware at STM32F030@48MHz/@8MHz and
  STM32F103@72MHz/@8MHz (6 devices, parasite power, hundreds of CRC-clean
  conversion cycles per variant; the F103@8MHz run also retires v1.6.0's
  11-12µs capture estimate, which came from different bench wiring).
  STM32G031@64MHz/@16MHz validated the same way on a WeAct STM32G031F6P6
  board — every supported clock combination is now hardware-confirmed.

 - The CH4 input-capture digital filter is standardized across all backends:
   a single documented rule in `inc/ow_port.h` picks the IC4F configuration
   whose filter time N × T_sample lands nearest ~500ns for the configured
  clock (`≤8MHz`: fCK_INT N=4; `≤16MHz`: fCK_INT N=8; above: fDTS/4 N=8),
  replacing the three hand-mirrored per-port conditionals. Firmware impact
  is limited to STM32G031@16MHz (N=4 → N=8, i.e. a 250ns → 500ns filter
  time); every other supported clock keeps byte-identical firmware, verified
  by md5 against the bench-validated binaries.

## [1.6.0] - 2026-08-23

### Added

- STM32G0 backend (`port/stm32g0/ow_port_g0.h`) for the STM32G031x6, e.g. the
  TSSOP20 STM32G031F6P6: same TIM1 CH3-output / CH4-indirect-capture scheme
  as F1/F0, with two family adaptations — the bus sits on logical PA10,
  which lives on the physical PA12 pad via the SYSCFG `PA12_RMP` remap (the
  package does not bond PA9/PA10 out), and DMA requests are routed through
  DMAMUX (TIM1_CC2 = request 21 feeds CCR3 via channel 3, TIM1_CH4 = request
  23 drains CCR4 via channel 4). Clocks ride the shared `OW_PORT_SYSCLK_MHZ`
  knob: default 64MHz (HSI16+PLL), `SYSCLK_MHZ=16` selects the raw HSI16.
  Select with `make OW_TARGET=g0`; host suite gains DMAMUX-routing tests and
  runs green on all three backends. Note: while the driver is initialised,
  pads PA11/PA12 must not be used as standalone GPIOs - reconfiguring them
  clears the remap bits and disconnects the bus. The shared slow-clock timing
  constants (≤16MHz threshold) are hardware-validated at 8MHz only, so the
  G0 raw-HSI16 build awaits bench validation like any new clock variant.

### Changed

- **Breaking:** the `HSI_8MHZ` build flag is replaced by the portable
  `OW_PORT_SYSCLK_MHZ` knob — a single integer carrying the system clock
  frequency in MHz. The old name tied a *source* choice to one family's
  frequency and could not scale (the raw HSI on an STM32G031 runs at
  16MHz, not 8). Every clock-dependent setting now derives from the new
  value: timer prescaler (`PSC = SYSCLK - 1`), input-capture filter
  selection and the '1'-slot pulse width switch on a shared ≤16MHz
  threshold, `app.c` derives its clock source per family (F1: 72 = HSE+PLL,
  8 = raw HSI; F0: 48 = HSI+PLL, 8 = raw HSI; anything else fails the
  build with a clear message) and the USART baud rate computes from it.
  Build via `make SYSCLK_MHZ=8`; defaults are unchanged.

### Fixed

- README architecture sections described the pre-unification register map
  (feed through `DMA1_Channel2`, captures into `CCR2` via `DMA1_Channel3`);
  they now document the scheme actually shipped: the CH2 marker request
  feeds `CCR3` through DMA1 channel 3, captures drain `CCR4` through
  DMA1 channel 4.

### Added

- Parasite-power support: `ds18b20_set_parasite(1)` makes the driver engage
  the strong pull-up (bus pin switched to push-pull HIGH) for every
  temperature-conversion window and EEPROM hold-off, releasing the line back
  to the external pull-up before any further bus activity. Backed by a new
  `ow_port_strong_pullup()` port hook in both the STM32F1 and STM32F0
  backends; seven host tests cover engagement windows, register state,
  mid-window flag clears and the default-off regression.
- Automatic parasite-mode detection: `ds18b20_detect_parasite()` /
  `ds18b20_detect_parasite_poll()` run a Read Power Supply query and store
  the decoded answer in the driver state, so parasite wiring is picked up at
  startup without a hard-coded flag; `ds18b20_parasite_mode()` reports the
  current configuration. Four host tests cover the detect path (parasite
  answer, external answer, no-presence abort leaves the flag untouched,
  getter/setter agreement). The example applications accept a
  `-DPARASITE_POWER=1` build flag to enable the mode out of the box, and the
  README gains a Parasite Power section with supply-current guidance.
- Public CI coverage: the host-test workflow now runs both backend suites
  (F1 and F0) as a test matrix, captures per-backend lcov traces and merges
  them into a single metric over the MCU-independent core (port plumbing is
  excluded, so adding new MCU families never dilutes the number). Every push
  to `main` republishes an HTML report and a self-hosted badge to the
  `gh-pages` branch; the badge links to
  [a5021.github.io/stm32-async-1wire](https://a5021.github.io/stm32-async-1wire/).
  This also fixes silent breakage of the previous coverage job: it captured
  from `build/test`, where no instrumented data ever landed.
- Debug assets for the STM32F0 backend, previously F1-only: Ozone project
  (`port/stm32f0/project.jdebug`), J-Flash project
  (`port/stm32f0/stm32f030f4.jflash`) and VSCode cortex-debug launch
  configurations ("Debug F0 (J-Link)" / "Debug F0 (ST-Link)") with a
  matching `Build F0 (debug)` task. The `STM32F030.svd` peripheral view is
  now downloaded together with the other build dependencies.

### Changed

- Sources and documentation aligned with the generic 1-Wire library
  concept: family-specific wording removed from `inc/ds18b20.h`,
  `CONTRIBUTING.md`, `SECURITY.md` and the Quick Start section; Quick Start
  now mentions raw `onewire.h` usage for non-DS18B20 slaves; VSCode
  IntelliSense gained an STM32F030 configuration matching
  `make OW_TARGET=f0`.
- Project renamed to `stm32-async-1wire`: the documentation now frames the
  library as a generic non-blocking 1-Wire layer for STM32 with the DS18B20
  driver as its first client. Repository URLs updated throughout (README,
  issue templates, release notes); public API and artifact names unchanged.
- Linker scripts and debug projects moved out of the repository root into
  their family port directories (`port/stm32f1/`, `port/stm32f0/`) — one
  self-contained folder per backend. The Makefile references them through
  new per-family variables (`LDS`, `JFLASH`), so `make OW_TARGET=f0
  jprogram` now uses the F0 J-Flash project instead of the F1 one. The
  Ozone projects resolve SVD/ELF paths relative to their own location.

## [1.5.0] - 2026-08-22

### Changed

- **BREAKING:** STM32F1 backend moved to the same TIM1 channel scheme as
  STM32F0: the 1-Wire bus now runs on **PA10** (previously PA8) as TIM1_CH3
  PWM output with CH4 indirect capture (IC4 <- TI3); the slot-end marker sits
  on a plain CC2 compare feeding CCR3 through DMA1 channel 3, while captures
  drain CCR4 through DMA1 channel 4. Re-wire DQ from PA8 to PA10 when
  upgrading. Timer logic, DMA channels and bus pin are now identical across
  both backends; only the prescaler and GPIO pin configuration differ. Both
  DMA request mappings verified empirically on the target (CC2 → channel 3,
  CH4 → channel 4).

## [1.4.1] - 2026-08-22

### Changed

- Release pipeline now ships the full firmware matrix: all four example apps
  (demo…demo4) for both backends (STM32F103 @72MHz/8MHz HSI, STM32F030
  @48MHz/8MHz HSI) — 16 variants with per-file SHA256 checksums.


## [1.4.0] - 2026-08-22

### Added

- STM32F0 backend (`port/stm32f0/ow_port_f0.h`): register-level `ow_port_*`
  implementation for the STM32F030x6 family (validated on an STM32F030F4P6,
  TSSOP20). The 1-Wire bus runs on **PA10** via TIM1 CH3 (PWM output,
  open-drain AF2) with indirect capture on CH4 (IC4 ← TI3, same pin) —
  PA8 is not bonded out in small F030 packages. The slot-end marker moved
  from the CH3 compare to a plain CH2 compare whose DMA request feeds CCR3;
  captures drain CCR4 through DMA1 channel 4 and the feed rides DMA1
  channel 3 (fixed request map — the F0 DMA has no CSELR mux).
- Build-time target selection: `make OW_TARGET=f0` switches the whole build
  (device headers, startup file, linker script `STM32F030X6_FLASH.ld`,
  backend include) to the STM32F030x6; the default remains STM32F103.
- Host test suite for the F0 backend: `make test-f0` runs the same 221-test
  suite against the F0 backend mock. The shared behavioural model
  (`tests/mock/hw_model.c`) and tests use backend-adaptive aliases
  (`MOCK_TIM_*` / `MOCK_BUS_*`), so both wirings are covered by one suite.
- HSI 8MHz clock option for both backends: `make HSI_8MHZ=1` selects a
  per-clock timer prescaler **and input-capture filter** so decode margins
  stay µs-equivalent at any system clock.
- Shared 1-Wire layer decode helpers `onewire_decode_pulses()` and
  `onewire_bit_from_pulse()`: the short/long pulse decode that was open-coded in
  four places (scratchpad decode, transaction read decode, search bit pairing
  and the search WRITE_READ step) is now a single shared path, covered by host
  unit tests.

### Changed

- `ds18b20_recall_eeprom_poll()` now documents that Recall EEPROM is a
  write-only command and does **not** update the tracked `ctx.resolution`.
  Callers that need the resolution to follow a possibly-different EEPROM config
  should follow the recall with `ds18b20_read_scratchpad()` to resynchronise
  `ds18b20_get_resolution()` before the next conversion.

### Fixed

- 8MHz HSI builds decoded '0' bits as '1' on both backends: the fixed
  ~5µs write pulse and the clock-scaled input-capture filter pushed read
  captures outside the decode window. The one-pulse width is now
  clock-dependent (2µs at 8MHz HSI vs 5µs otherwise) and the capture filter
  is selected per clock, so the decode threshold itself stays untouched.
- `ds18b20_select()` is only accepted while the measurement state machine is
  IDLE; calls from a running cycle, a device/alarm search, a resolution change
  or another command transaction are ignored — including from the per-device
  scan callback (`ds18b20_complete()` in scan mode), where a select is now
  explicitly rejected and the scan round continues. The API documentation and
  header notes were corrected to match this behaviour.

## [1.3.0] - 2026-08-17

### Added

- Non-blocking command transactions for the remaining DS18B20 commands, driven
  with the same poll discipline as the device search and the resolution change:
  `ds18b20_read_rom()`, `ds18b20_set_alarm_thresholds()`,
  `ds18b20_read_scratchpad()`, `ds18b20_copy_scratchpad()`,
  `ds18b20_recall_eeprom()`, `ds18b20_read_power_supply()` (each with a
  matching `*_poll()`). Every transaction performs reset → presence → write
  (+ Match ROM when a device is selected) → read or timed wait, owns TIM1/DMA
  while it runs and hands the timer back to `ds18b20_poll()` when finished.
  `ds18b20_last_command_ok()` reports whether the last transaction found a
  device present.
- `DS18B20_READ_ROM` (0x33), `DS18B20_RECALL_EEPROM` (0xB8) and
  `DS18B20_READ_POWER_SUPPLY` (0xB4) protocol constants exported by the header.
- Raw scratchpad read (`ds18b20_read_scratchpad()`): returns all 9 bytes
  including TH/TL and the CRC; on a valid read the conversion resolution is
  auto-derived from the config byte (byte 4, R1/R0).
- Alarm trigger thresholds (`ds18b20_set_alarm_thresholds(th, tl)`): writes
  TH/TL with Write Scratchpad (0x4E) without disturbing the current
  resolution; `ds18b20_copy_scratchpad()` and `ds18b20_recall_eeprom()`
  persist and restore them to/from the EEPROM with a 10 ms timed hold-off.
- Example application `src/demo4.c` (`make APP=demo4`): device search, then
  the full command sequence on the first found sensor — power supply, raw
  scratchpad, TH/TL write with Copy + Recall, Read ROM — followed by
  steady-state measurement of the selected device.

### Changed

- Ownership guards: every `ds18b20_search_start()`,
  `ds18b20_alarm_search_start()`, `ds18b20_scan_start()` and
  `ds18b20_set_resolution()` entry point now rejects starting while a command
  transaction owns the timer, and `ds18b20_poll()` skips its work until the
  active transaction finishes.
- `ds18b20_select()` now also applies to the command transactions: with a
  device selected, every command (except the bare Read ROM) is addressed via
  Match ROM, so the transactions target that specific sensor.

## [1.2.0] - 2026-08-17

### Added

- Non-blocking alarm search: `ds18b20_alarm_search_start()`,
  `ds18b20_alarm_search_poll()`, `ds18b20_alarm_search_count()`. Implements the
  Maxim Alarm Search ROM (0xEC) algorithm with the same state machine as the
  device search: it reports only the DS18B20 devices currently in alarm state
  (temperature outside the TH/TL thresholds set with Write Scratchpad). The
  alarm search never repopulates the scan-mode device table, so the addresses
  found by a previous device search stay valid while the alarm state of the bus
  is polled.
- Universal non-blocking 1-Wire layer: `inc/onewire.h` + `src/onewire.c`. The
  bus primitives (reset, presence, write/read slots, merged write-then-read,
  multi-byte read) and the generic Maxim Search ROM engine
  (`onewire_search_start()`, `onewire_search_poll()`, `onewire_search_count()`,
  `onewire_search_active()`) now live in a shared layer that any 1-Wire slave
  driver (DS18B20 today, DS2413/DS2431 later) is built on. Every operation is
  scheduled on TIM1/DMA and completes asynchronously; callers poll
  `onewire_bus_done()` / `onewire_search_poll()` to advance, never wait. The
  layer is registered for capture via its own edge buffers and keeps the same
  hardware bus release to idle HIGH after every transaction. It also provides
  the Dallas/Maxim CRC-8 utility `onewire_crc8()`.

### Changed

- The 1-Wire bus primitives and the Search ROM state machine moved out of the
  driver into the shared layer. `src/ds18b20.c` now builds on `onewire_*`; the
  public `ds18b20.h` API (including the `ds18b20_bus_*`-free surface) is
  unchanged, and the driver's `ds18b20_init()` initializes the layer for you.
- `inc/macro.h` newlib-nano syscall stubs (`_read`, `_write`, `_close`,
  `_lseek`) are now `weak`: the layer and the driver each include `macro.h`,
  and weak definitions keep the multi-translation-unit firmware link clean.

## [1.1.0] - 2026-08-16

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
- Non-blocking resolution change: `ds18b20_set_resolution()` /
  `ds18b20_set_resolution_poll()` change the conversion resolution (9..12 bit)
  between measurement cycles, mirroring the device search state machine (one
  hardware operation per poll). The config is written with
  Write Scratchpad (0x4E) to the volatile scratchpad; TH/TL are reset to 0
  (alarms disabled) and the change is not persisted to the EEPROM.
- `ds18b20_get_resolution()` reports the current resolution, auto-derived from
  every valid scratchpad read (byte 4, R1/R0 bits).
- Simultaneous multi-device conversion: `ds18b20_scan_start()` converts every
  discovered device in parallel with one broadcast `Convert T` (Skip ROM), then
  reads each one back via Match ROM in device-table order. One conversion wait
  covers all sensors (`750ms + N x read` instead of `N x 750ms`). Each reading
  is reported through `ds18b20_complete()`; `ds18b20_scan_index()`,
  `ds18b20_device_rom()` and `ds18b20_device_count()` identify the sensor. A
  missing device reports `DS18B20_TEMP_ERROR_NO_SENSOR` and the scan continues.
  `ds18b20_select()` (single-device addressing) clears scan mode; the config
  write for a resolution change is broadcast in scan mode. See `demo3.c`.

### Changed

- The public API is strictly non-blocking: the high-level interface
  (`ds18b20_init()`, `ds18b20_poll()`, `ds18b20_select()`, the weak callbacks
  `ds18b20_busy()`/`ds18b20_complete()`) plus the `ds18b20_search_*` family,
  the `ds18b20_scan_*` family, the `ds18b20_set_resolution_*` family and the
  CRC utility `ds18b20_crc8()`. The internal 1-Wire bus helpers
  (`ds18b20_bus_*`), the Search ROM state machine and the resolution-change
  state machine live inside the library (`src/ds18b20.c`).
- The non-blocking device search is driven from the main loop exactly like the
  measurement state machine: each `ds18b20_search_poll()` performs one
  hardware operation. It filters by `DS18B20_FAMILY_CODE`, validates the ROM
  CRC and forces a timer update event before handing back to `ds18b20_poll()`.
- UART TX is now fully non-blocking: `uart_tx_enqueue_byte()` drops a byte when
  the ring buffer is full, and `uart_tx_flush()` was removed.
- The conversion wait is now resolution-aware: `wait_conversion()` waits
  exactly the datasheet time of the configured resolution (93.75ms @ 9-bit,
  187.5ms @ 10-bit, 375ms @ 11-bit, 750ms @ 12-bit) instead of a fixed 750ms,
  so lower resolutions complete 8× faster.
- The 1-Wire line is now released to idle HIGH purely in hardware after every
  transaction: DMA-fed writes append a trailing 0 to the CCR1 feed (last-slot
  CC4 event) and direct-write/capture operations use an OC1PE preload of 0,
  both applied exactly when the one-pulse timer stops. The software
  `T1.CCR1 = 0` in `ds18b20_bus_done()` was removed, so the bus idles HIGH
  between slots regardless of RTOS scheduling latency (verified: 5/5 devices
  found with no software release).
- Protocol constants (`DS18B20_SEARCH_ROM`, `DS18B20_MATCH_ROM`,
  `DS18B20_CONVERT_T`, `DS18B20_READ_SCRATCHPAD`, `DS18B20_WRITE_SCRATCHPAD`,
  `DS18B20_COPY_SCRATCHPAD`, `DS18B20_RES_MIN`/`DS18B20_RES_MAX`/
  `DS18B20_RES_DEFAULT`, `DS18B20_BITS_PER_BYTE`) are exported by the header
  for use with the public API and host tests.
- DMA transfer count sized to `slots-1` instead of using a sentinel value.
- Removed the global `#define CR` peripheral alias from `inc/macro.h`: it
  collided with the `RCC_TypeDef.CR` field name after CMSIS headers were
  included, expanding the field into a pointer that shifted every RCC register
  offset on 64-bit host builds.
- Added a host test suite (`make test`): the driver is compiled as a single
  translation unit against a TIM1/DMA behavioural model and a register mock.
  145 tests cover the state machine, device search, resolution change, CRC-8,
  pulse encoding, presence detection, scratchpad decode, temperature
  conversion, timing and bus release. The suite runs in CI.

### Fixed

- The scan mode stalled after the first device read on real hardware: after a
  per-device read-back the state machine sat in `CONTINUE` waiting for a UIF
  that never came, because `DECODE` arms no timer and only the inter-measurement
  pause (single-device path) or a new conversion provides one. A 1ms scheduling
  bridge timer is now armed between scan-mode reads so every device in the table
  is reported each round (verified on an 8-sensor bus: 8 readings per round).
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

[1.0.0]: https://github.com/a5021/stm32-async-1wire/releases/tag/v1.0.0
[1.1.0]: https://github.com/a5021/stm32-async-1wire/releases/tag/v1.1.0
[1.2.0]: https://github.com/a5021/stm32-async-1wire/releases/tag/v1.2.0
[1.3.0]: https://github.com/a5021/stm32-async-1wire/releases/tag/v1.3.0
[1.4.0]: https://github.com/a5021/stm32-async-1wire/releases/tag/v1.4.0
[1.4.1]: https://github.com/a5021/stm32-async-1wire/releases/tag/v1.4.1
[1.5.0]: https://github.com/a5021/stm32-async-1wire/releases/tag/v1.5.0
[1.6.0]: https://github.com/a5021/stm32-async-1wire/releases/tag/v1.6.0
[1.6.1]: https://github.com/a5021/stm32-async-1wire/releases/tag/v1.6.1
[1.7.0]: https://github.com/a5021/stm32-async-1wire/compare/v1.6.1...v1.7.0
[Unreleased]: https://github.com/a5021/stm32-async-1wire/compare/v1.7.0...HEAD
