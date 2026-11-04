[![STM32 Build Pipeline](https://github.com/a5021/stm32-async-1wire/actions/workflows/build.yml/badge.svg)](https://github.com/a5021/stm32-async-1wire/actions/workflows/build.yml)
[![Code Quality](https://github.com/a5021/stm32-async-1wire/actions/workflows/ci.yml/badge.svg)](https://github.com/a5021/stm32-async-1wire/actions/workflows/ci.yml)
[![Coverage](https://raw.githubusercontent.com/a5021/stm32-async-1wire/gh-pages/coverage-badge.svg)](https://a5021.github.io/stm32-async-1wire/) [![1-Wire viewer](https://img.shields.io/badge/demo-1--Wire%20viewer-1b57c4.svg)](https://a5021.github.io/stm32-async-1wire/viewer/) [![Awesome](https://awesome.re/badge.svg)](https://github.com/iDoka/awesome-embedded-software#sensors)
[![Fuzzed](https://img.shields.io/badge/fuzzed-libFuzzer-blue.svg)](docs/building.md#testing)
[![License: MIT](https://img.shields.io/badge/License-MIT-yellow.svg)](https://opensource.org/licenses/MIT)
# stm32-async-1wire

Non-blocking 1-Wire master for STM32, with a DS18B20 temperature driver and a DS2431 EEPROM driver built on top. A generic bus layer (`src/onewire.c`) owns the 1-Wire timing — a hybrid of a hardware timer (TIM1) and DMA automates every slot, so the CPU never performs timing-critical busy-waits inside a transaction and never enters an interrupt; operations advance by polling hardware completion flags. The first driver on that layer is `src/ds18b20.c`, the DS2431 EEPROM driver rides it as `src/ds2431.c`, and other 1-Wire slaves (DS2413, ...) can ride it as-is.

The core (`src/onewire.c` + `src/ds18b20.c`) is MCU-independent. Its public surface is the headers under `inc/`: `onewire.h` (the bus API), `ow_board.h` (the family selection token, `ow_pulse_t` and the board clock defaults), `ow_config.h` (the compile-time tunables), `ow_stats.h` (the optional statistics module) and the per-driver headers. The TIM1+DMA engine (`src/ow_port.h`) and the register-level per-family facts (`src/ow_mcu.h`) are library-internal: a consumer needs only `-Iinc` and never sees the port. Nine families ship today:

- `src/ow_mcu.h` (STM32F1 section) — STM32F103C8T6 (Blue Pill): bus on PA10, TIM1 CH3 output / CH4 capture, DMA1 channels 3/4.
- `src/ow_mcu.h` (STM32F0 section) — STM32F030x6 (e.g. TSSOP20 STM32F030F4P6): bus on PA10, TIM1 CH3 output / CH4 capture, DMA1 channels 3/4.
- `src/ow_mcu.h` (STM32F3 section) — STM32F303VC (F3-DISCOVERY / MB1035B): bus on PA10 (AF6), TIM1 CH3 output / CH4 capture, DMA1 channels 3/4 — the same fixed pair as F0/F1 (RM0316 Table 78; channel 4 also carries USART1_TX and channel 3 USART3_TX, harmless because no UART byte moves by DMA).
- `src/ow_mcu.h` (STM32G0 section) — STM32G031x6 (e.g. TSSOP20 STM32G031F6P6): bus on PA10 via the SYSCFG PA12 remap, TIM1 CH3 output / CH4 capture, DMA1 channels 3/4 through DMAMUX (requests 21/23).
- `src/ow_mcu.h` (STM32G4 section) — STM32G474CB (WeAct STM32G474CBT6 Long): bus on PA10 (AF6), TIM1 CH3 output / CH4 capture, DMA1 channels 3/4 through DMAMUX (requests 43/45), 8-bit feed tables. HSE+PLL to 170MHz (M=2, N=85, R=2, Range 1 Boost) or raw HSI16 at 16MHz. USART1 TX on PA9 (AF7).
- `src/ow_mcu.h` (STM32L4 section) — STM32L476RG (NUCLEO-L476RG): bus on PA10 (AF1), TIM1 CH3 output / CH4 capture, classic DMA1 channels 3/4 with the request selected in `DMA1_CSELR` (both channels use selection 7: TIM1_CH2 on channel 3, TIM1_CH4 on channel 4), 8-bit feed tables. HSI16 only — the board's HSE crystal is not populated: 80MHz via HSI16+PLL (M=1, N=10, R=2, voltage Range 1, 4 flash wait states) or 16MHz raw HSI16. USART1 TX on PA9 (AF7, external USB-TTL), LED PA5. Bench-validated with 7 DS18B20 in parasite power plus one DS2431: search, measurement, scan, statistics and DS2431 read-back all pass at both clocks.
- `src/ow_mcu.h` (STM32F4 section) — STM32F407VGT6 (STM32F4DISCOVERY), STM32F401CC (e.g. WeAct F401 Black Pill) and STM32F446RE (e.g. WeAct F446RET6): bus on PA10, TIM1 CH3 output / CH4 capture, DMA2 streams 2/4 (feed 16-bit, direct mode). Same section for all three parts — the chip selects the CMSIS device layer, linker script and clock default. The F446 additionally runs 180MHz, which needs the PWR over-drive sequence (see [Clocking invariant](docs/architecture.md#4-clocking-invariant)).
- `src/ow_mcu.h` (STM32H5 section) — STM32H503CB (WeAct STM32H503Cx Core Board): bus on PA10 (AF1), TIM1 CH3 output / CH4 capture, GPDMA1 channels 2/3 (requests 59/61, no DMAMUX), 8-bit feed tables. Raw HSI at 64MHz (bring-up clock, default), raw 8MHz HSE (crystal Y2), or HSE+PLL1 to 250MHz (VOS0, 5 flash wait states). USART1 TX on PA9 (AF7). Bench-validated on the WeAct board — all seven examples flashed and captured at each clock with correct 22.8-23.1C readings and 0 errors in 6_statistics.
- `src/ow_mcu.h` (STM32H7 section) — H743 Rev V (DEV_ID `0x450`, REV_ID `0x2003`; the WeAct Studio STM32H7XX board under current test): bus on PA10 (AF1), TIM1 CH3 output / CH4 capture, DMA1 streams 2/3 through DMAMUX1 (requests 12/14), 16-bit feed tables (direct mode, like F4). HSE+PLL to 240MHz (M=HSE/5, N=96, P=2 through PLL1P); the LDO supply is configured and ACTVOSRDY awaited before the VOS1→VOS0 Rev V sequence. Four flash wait states; APB1/2/3/4 are /2, so TIM1 remains at the 240MHz driver ceiling; Rev V can optionally run CPU/SYSCLK at 480MHz with HCLK and TIM1 at 240MHz. USART1 TX on PA9 (AF7). The D-cache stays off: SRAM1 is cacheable and no maintenance hooks run in this backend (see the H7 section).

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
| [Examples](docs/examples.md) | The ten examples, including the DMA-pressure diagnostic rig |
| [Hardware](docs/hardware.md) | Verified boards, wiring, electrical notes |
| [Architecture and RTOS](docs/architecture.md) | The TIM1+DMA engine, layer contracts, state machine, RTOS use |
| [API, performance and configuration](docs/api-reference.md) | The public API, RAM/flash/CPU numbers, clocks and timing presets |
| [Migrating from earlier releases](docs/migration.md) | The 3.0 `OW_PORT_*` rename table and other breaking changes |
| [Repository file structure](docs/file-structure.md) | Where the sources, ports, tests and tools live |
| [Troubleshooting](docs/troubleshooting.md) | Common issues and debugging tips |
| [Project](docs/project.md) | Contributing, support and references |

## Supported Devices

Two 1-Wire slaves ship with their own non-blocking driver; any other slave can
be driven directly from the shared bus layer.

| Device | Type | Family code | Driver | Example |
|--------|------|-------------|--------|---------|
| **DS18B20** | Digital temperature sensor, 9-12-bit, ±0.5 °C, parasite or external power | `0x28` | `inc/ds18b20.h` + `src/ds18b20.c` | `1_basic` … `7_low_power`, `9_multi` |
| **DS2431** | 1 kbit (128-byte) EEPROM, 4 × 32-byte pages via an 8-byte scratchpad | `0x2D` | `inc/ds2431.h` + `src/ds2431.c` | `8_ds2431`, `9_multi` |

Both drivers are non-blocking and ride the same TIM1+DMA 1-Wire layer. Only one
of them owns the bus at a time: an operation started on one driver must finish
(its `*_poll()` reports done) before the other starts one, and neither may be
polled while the other is running. Both `ds18b20_init()` and `ds2431_init()` may
still run before any bus operation in the same firmware image — each calls
`onewire_init()`, which re-applies the same idle timer/DMA/pin configuration —
and `examples/9_multi` does exactly that and then hands the bus back and forth
between the two drivers through one explicit owner state machine. The DS2431
example filters the shared bus search by family code (`DS2431_FAMILY_CODE`,
0x2D), while the DS18B20 driver's own search filters on 0x28. Slaves without a
driver here (DS2413, ...) can be driven by including `"onewire.h"` and building
on the bus primitives.

## Features

- Pure Bare-Metal: direct register manipulation, no HAL or LL libraries.
- Universal 1-Wire Layer: `inc/onewire.h` + `src/onewire.c` — a reusable,
  non-blocking 1-Wire master (bus primitives + Maxim Search ROM) scheduled on
  TIM1/DMA; the DS18B20 and DS2431 drivers are built on it, and other 1-Wire
  slaves (DS2413, ...) can reuse it as-is — see
  [1-Wire Layer (shared)](docs/api-reference.md#1-wire-layer-shared).
  - Multi-MCU Backend: one MCU-independent core over a `ow_port_*` interface;
    header-only backends for STM32F0, STM32F1, STM32F3, STM32F4, STM32G0,
    STM32G4, STM32L4, STM32H5 and STM32H7, all on the shared CH3/CH4 scheme.
    Select at build time with `make OW_MCU=STM32F030x6` /
    `make OW_MCU=STM32F303xC` / `make OW_MCU=STM32F407xx` /
    `make OW_MCU=STM32G031xx` / `make OW_MCU=STM32G474xx` /
    `make OW_MCU=STM32L476xx` / `make OW_MCU=STM32H503xx` /
    `make OW_MCU=STM32H743xx` (STM32F103xB is the default).
- DS2431 EEPROM Driver: `inc/ds2431.h` + `src/ds2431.c` — the four DS2431
  commands (Read Memory 0xF0, Write/Read Scratchpad 0x0F/0xAA, Copy Scratchpad
  0x55) as non-blocking, polled state machines on the same shared 1-Wire layer
  and TIM1/DMA backend, with external and parasite power (strong pull-up
  hold-off for Copy Scratchpad) — see
  [DS2431 EEPROM Driver](docs/api-reference.md#ds2431-eeprom-driver) and `examples/8_ds2431`.
- Multi-Device Application (`9_multi`): a peak/low temperature logger that runs
  DS18B20 sensors and a DS2431 EEPROM on one bus, handing ownership back and
  forth between the two drivers; results are stored as self-validating CRC
  records, with the ROM block written before the record that refers to it. See
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
  configured conversion resolution requires (93.75ms @ 9-bit … 750ms @
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
references, and [docs/troubleshooting.md](docs/troubleshooting.md)
for common wiring and integration issues.
