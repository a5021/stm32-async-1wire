[![STM32 Build Pipeline](https://github.com/a5021/stm32-async-1wire/actions/workflows/build.yml/badge.svg)](https://github.com/a5021/stm32-async-1wire/actions/workflows/build.yml)
[![Code Quality](https://github.com/a5021/stm32-async-1wire/actions/workflows/ci.yml/badge.svg)](https://github.com/a5021/stm32-async-1wire/actions/workflows/ci.yml)
[![Coverage](https://raw.githubusercontent.com/a5021/stm32-async-1wire/gh-pages/coverage-badge.svg)](https://a5021.github.io/stm32-async-1wire/) [![1-Wire viewer](https://img.shields.io/badge/demo-1--Wire%20viewer-1b57c4.svg)](https://a5021.github.io/stm32-async-1wire/viewer/) [![Awesome](https://awesome.re/badge.svg)](https://github.com/iDoka/awesome-embedded-software#sensors)
[![Fuzzed](https://img.shields.io/badge/fuzzed-libFuzzer-blue.svg)](docs/building.md#testing)
[![License: MIT](https://img.shields.io/badge/License-MIT-yellow.svg)](https://opensource.org/licenses/MIT)
# stm32-async-1wire

Non-blocking 1-Wire master for STM32, with DS18B20 temperature, DS2431 EEPROM
and DS2413 dual-channel switch drivers built on top. A generic bus layer
(`src/onewire.c`) owns
the 1-Wire timing — a hybrid of a hardware timer (TIM1) and DMA automates every
slot, so the CPU never performs timing-critical busy-waits inside a transaction
and never enters an interrupt; operations advance by polling hardware
completion flags. The first driver on that layer is `src/ds18b20.c`, the DS2431
EEPROM driver rides it as `src/ds2431.c`, the DS2413 switch driver as
`src/ds2413.c`, and other 1-Wire slaves (DS2408, ...) can ride it as-is.

The core (`src/onewire.c` + `src/ds18b20.c`) is MCU-independent. Its public
surface is the headers under `inc/`: `onewire.h` (the bus API), `ow_board.h`
(the family selection token, `ow_pulse_t` and the board clock defaults),
`ow_config.h` (the compile-time tunables), `ow_stats.h` (the optional statistics
module) and the per-driver headers. The TIM1+DMA engine (`src/ow_port.h`) and
the register-level per-family facts (`src/ow_mcu.h`) are library-internal: a
consumer needs only `-Iinc` and never sees the port.

Ten families ship today; the verified board, wiring and clock details are in
[Hardware](docs/hardware.md).

| Family | Verified part / board | Bus pin | DMA route | Notes |
| --- | --- | --- | --- | --- |
| STM32F0 | STM32F030x6 (TSSOP20) | PA10 (AF2) | DMA1 channels 3/4 | 48 MHz default; PA8 is not bonded out on the TSSOP20 package |
| STM32F1 | STM32F103C8T6 (Blue Pill) | PA10 (default AFIO) | DMA1 channels 3/4 | 72 MHz default; no `OSPEEDR` on this family |
| STM32F3 | STM32F303VC (F3-DISCOVERY) | PA10 (AF6) | DMA1 channels 3/4 | 72 MHz HSE-bypass; bench-validated with 7 × DS18B20 |
| STM32F4 | F407VGT6 / F401CC / F446RE | PA10 (AF1) | DMA2 streams 2/4 (`CHSEL=6`) | 16-bit feed; the F446 runs 180 MHz with the over-drive sequence |
| STM32G0 | STM32G031x6 (TSSOP20) | PA12 pad → logical PA10 | DMAMUX requests 21/23 | 64 MHz HSI16+PLL; PA11/PA12 must stay in AF mode |
| STM32G4 | STM32G474CB (WeAct Long) | PA10 (AF6) | DMAMUX requests 43/45 | 170 MHz HSE+PLL; 8-bit feed tables |
| STM32L4 | STM32L476RG (NUCLEO-L476RG) | PA10 (AF1) | DMA1_CSELR selection 7 | 80 MHz HSI16+PLL, no HSE; bench-validated with 7 × DS18B20 + DS2431 |
| STM32C5 | STM32C552CET6 (WeAct 48-pin) | PA10 (AF1) | LPDMA1 channels 2/3 (`REQSEL` 25/27) | 144 MHz HSE -> PSI, 48 MHz HSIDIV3 or raw 8 MHz HSE; 8-bit feed; bench-validated with 7 × DS18B20 + DS2431 |
| STM32H5 | STM32H503CB (WeAct Core) | PA10 (AF1) | GPDMA1 channels 2/3 (`REQSEL` 59/61) | 64 MHz HSI, 8 MHz HSE or 250 MHz PLL1; 8-bit feed |
| STM32H7 | STM32H743VI (WeAct H7) | PA10 (AF1) | DMA1 streams 2/3, DMAMUX1 requests 12/14 | 240 MHz HSE+PLL, opt-in 480/240 MHz split; 16-bit feed; D-cache off |

## Quick start

```bash
# STM32F103xB is the default part; pick another with OW_MCU.
make OW_MCU=STM32F030x6 APP=1_basic
```

Call `ds18b20_init()` once, then `ds18b20_start_measure()` and poll
`ds18b20_poll()` from the main loop (or an RTOS task); the bus timing is entirely
hardware-generated. The full walk-through, the callback contract and the first
build are in [Getting started](docs/getting-started.md).

## Documentation

| Guide | What it covers |
|---|---|
| [Getting started](docs/getting-started.md) | Requirements, what to know before integrating, and the first build |
| [Building, testing and integration](docs/building.md) | Make/CMake/PlatformIO, the host and fuzz suites, HIL, STM32CubeIDE, VSCode, tunables |
| [Standalone consumer template](templates/standalone-consumer/README.md) | Vendored `inc/` + `src/` CMake template, board glue and build knobs |
| [Examples](docs/examples.md) | The ten examples, including the DMA-pressure diagnostic rig |
| [CPU profile](docs/cpu-profile.md) | Measured driver work for one measurement and one Search ROM pass |
| [Hardware](docs/hardware.md) | Verified boards, wiring, electrical notes |
| [Bench notes: F1](port/stm32f1/HARDWARE-NOTES.md) / [F4](port/stm32f4/HARDWARE-NOTES.md) / [C5](port/stm32c5/HARDWARE-NOTES.md) | Per-port bring-up, DMA and timing measurements |
| [Architecture and RTOS](docs/architecture.md) | The TIM1+DMA engine, layer contracts, state machine, RTOS use |
| [API, performance and configuration](docs/api-reference.md) | The public API, RAM/flash footprint, clocks and timing presets |
| [Migrating from earlier releases](docs/migration.md) | The 3.0 `OW_PORT_*` rename table and other breaking changes |
| [Repository file structure](docs/file-structure.md) | Where the sources, ports, tests and tools live |
| [Troubleshooting](docs/troubleshooting.md) | Common issues and debugging tips |
| [Project](docs/project.md) | Contributing, support and references |

## Supported Devices

Three 1-Wire slaves ship with their own non-blocking driver; any other slave can
be driven directly from the shared bus layer.

| Device | Type | Family code | Driver | Example |
|--------|------|-------------|--------|---------|
| **DS18B20** | Digital temperature sensor, 9-12-bit, ±0.5 °C, parasite or external power | `0x28` | `inc/ds18b20.h` + `src/ds18b20.c` | `1_basic` … `7_low_power`, `9_multi` |
| **DS2431** | 1 kbit (128-byte) EEPROM, 4 × 32-byte pages via an 8-byte scratchpad | `0x2D` | `inc/ds2431.h` + `src/ds2431.c` | `8_ds2431`, `9_multi` |
| **DS2413** | Dual-channel addressable switch, two open-drain PIO pins | `0x3A` | `inc/ds2413.h` + `src/ds2413.c` | `9_multi` |

All three drivers are non-blocking and ride the same TIM1+DMA 1-Wire layer. Only
one of them owns the bus at a time: an operation started on one driver must
finish (its `*_poll()` reports done) before another starts one, and none may be
polled while another is running. All three `*_init()` functions
(`ds18b20_init()`, `ds2431_init()`, `ds2413_init()`) may still run before any
bus operation in the same firmware image — each calls `onewire_init()`, which
re-applies the same idle timer/DMA/pin configuration — and `examples/9_multi`
does exactly that and then hands the bus back and forth between the drivers
through one explicit owner state machine. The DS2431 example filters the shared
bus search by family code (`DS2431_FAMILY_CODE`, 0x2D), the DS2413 search by
0x3A, while the DS18B20 driver's own search filters on 0x28. Slaves without a
driver here (DS2408, ...) can be driven by including `"onewire.h"` and building
on the bus primitives.

## Features

- Pure Bare-Metal: direct register manipulation, no HAL or LL libraries.
- Universal 1-Wire Layer: `inc/onewire.h` + `src/onewire.c` — a reusable,
  non-blocking 1-Wire master (bus primitives + Maxim Search ROM) scheduled on
  TIM1/DMA; the DS18B20, DS2431 and DS2413 drivers are built on it, and other
  1-Wire slaves (DS2408, ...) can reuse it as-is — see
  [1-Wire Layer (shared)](docs/api-reference.md#1-wire-layer-shared).
  - Multi-MCU Backend: one MCU-independent core over a `ow_port_*` interface;
    header-only backends for STM32F0, STM32F1, STM32F3, STM32F4, STM32G0,
    STM32G4, STM32L4, STM32H5 and STM32H7, all on the shared CH3/CH4 scheme.
    Select at build time with `make OW_MCU=STM32F030x6` /
    `make OW_MCU=STM32F303xC` / `make OW_MCU=STM32F407xx` /
    `make OW_MCU=STM32G031xx` / `make OW_MCU=STM32G474xx` /
    `make OW_MCU=STM32L476xx` / `make OW_MCU=STM32C552xx` /
    `make OW_MCU=STM32H503xx` / `make OW_MCU=STM32H743xx` (STM32F103xB is the default).
- DS2431 EEPROM Driver: `inc/ds2431.h` + `src/ds2431.c` — the four DS2431
  commands (Read Memory 0xF0, Write/Read Scratchpad 0x0F/0xAA, Copy Scratchpad
  0x55) as non-blocking, polled state machines on the same shared 1-Wire layer
  and TIM1/DMA backend, with external and parasite power (strong pull-up
  hold-off for Copy Scratchpad) — see
  [DS2431 EEPROM Driver](docs/api-reference.md#ds2431-eeprom-driver) and `examples/8_ds2431`.
- DS2413 Dual-Channel Switch Driver: `inc/ds2413.h` + `src/ds2413.c` — PIO
  Access Read (0xF5) and Write (0x5A) as non-blocking polled operations, with
  Match/Skip ROM, reply/ack validation, a 1 ms parasite prime and three
  retries. `examples/9_multi` blinks PIOA/PIOB while it waits between
  measurement rounds — see
  [DS2413 Driver](docs/api-reference.md#ds2413-dual-channel-switch-driver).
- Multi-Device Application (`9_multi`): a peak/low temperature logger that runs
  DS18B20 sensors, a DS2431 EEPROM and a DS2413 switch on one bus, handing
  ownership back and forth between the drivers; results are stored as
  self-validating CRC records, with the ROM block written before the record
  that refers to it. See
  [Multi-Device Application](docs/api-reference.md#multi-device-application-9_multi).
- DMA Feed Pressure Rig (`0_dma_pressure`): a diagnostic example, not a demo.
  It transmits a known 64-slot pattern under a build-time selectable
  memory-subsystem load and reports whether the CCR3 feed DMA still met its
  slot deadline. See
  [Measuring DMA feed margin](docs/examples.md#measuring-dma-feed-margin-0_dma_pressure).
- Zero NVIC Interrupts: no NVIC interrupts and no ISRs — fully polled
  operation. The optional `-DOW_LOW_POWER=1` mode enables the timer
  update **interrupt source** (UIE) only to generate a pending event that
  wakes `WFE()` via `SEVONPEND` — no NVIC interrupt is enabled and no ISR is
  installed.
- RTOS-Ready: the strict 1-Wire bit timing is generated entirely by TIM1+DMA,
  so `ds18b20_poll()` can be called at any rate from an RTOS task without
  corrupting the bus. Not thread-safe by itself — see
  [RTOS Integration](docs/architecture.md#rtos-integration).
- Hardware Automation: TIM1 output compare + input capture with DMA automate
  waveform generation and data capture — see [Architecture](docs/architecture.md#architecture).
- State Machine Architecture: event-driven operation controlled by hardware
  completion signals — see
  [State Machine Flow](docs/architecture.md#state-machine-flow-hardware-timed-polled-on-uif).
- Weak Function Callbacks (`ds18b20_busy()`, `ds18b20_complete()`) and CRC-8
  validation of every sensor reading — see [Weak Callbacks](docs/api-reference.md#weak-callbacks)
  and [Error Codes](docs/api-reference.md#error-codes).
- Resolution-Aware Conversion Wait: the driver waits exactly as long as the
  configured conversion resolution requires (93.75 ms @ 9-bit … 750 ms @
  12-bit), so lowering the resolution speeds up the measurement cycle.
- Non-blocking across the board — each operation advances one hardware
  operation per `*_poll()` call from the main loop:
  - Device search: `ds18b20_search_start()` / `_poll()` / `_count()` —
    [Device Search](docs/api-reference.md#device-search).
  - Alarm search: `ds18b20_alarm_search_*()` reports only devices in alarm —
    [Alarm Search](docs/api-reference.md#alarm-search).
  - Command transactions: Read ROM, TH/TL, scratchpad, Copy/Recall EEPROM,
    parasite detect — [Command Transactions](docs/api-reference.md#command-transactions),
    `examples/5_commands`.
  - Per-device addressing: `ds18b20_select()` (Match ROM 0x55) —
    [Per-Device Addressing](docs/api-reference.md#per-device-addressing).
  - Resolution change: `ds18b20_set_resolution()` / `_poll()` —
    [Resolution Change](docs/api-reference.md#resolution-change).
  - Simultaneous multi-device conversion: one broadcast `Convert T`, read
    back via Match ROM —
    [Simultaneous Multi-Device Conversion](docs/api-reference.md#simultaneous-multi-device-conversion).
- Optional Signal Statistics Module (`ow_stats`, `-DOW_STATS_ENABLE=1`):
  per-sensor pulse-width min/max, a global histogram, error counters and a
  non-blocking UART dump; all stubs inline to nothing in production builds —
  see [Signal Statistics Module](docs/api-reference.md#signal-statistics-module-ow_stats).

## License

This project is released under the MIT License. See the LICENSE file for details.

See [CONTRIBUTING.md](CONTRIBUTING.md) for the contribution
guide, [docs/project.md](docs/project.md) for support and
references, [docs/troubleshooting.md](docs/troubleshooting.md)
for common wiring and integration issues, and [CHANGELOG.md](CHANGELOG.md)
for the release history. Security reports follow [SECURITY.md](SECURITY.md).
