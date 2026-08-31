[![STM32 Build CI](https://github.com/a5021/stm32-async-1wire/actions/workflows/build.yml/badge.svg)](https://github.com/a5021/stm32-async-1wire/actions/workflows/build.yml)
[![Code Quality](https://github.com/a5021/stm32-async-1wire/actions/workflows/ci.yml/badge.svg)](https://github.com/a5021/stm32-async-1wire/actions/workflows/ci.yml)
[![Coverage](https://raw.githubusercontent.com/a5021/stm32-async-1wire/gh-pages/coverage-badge.svg)](https://a5021.github.io/stm32-async-1wire/)
[![License: MIT](https://img.shields.io/badge/License-MIT-yellow.svg)](https://opensource.org/licenses/MIT)
# stm32-async-1wire

Non-blocking 1-Wire master for STM32, with a DS18B20 temperature driver built on top. A generic bus layer (`src/onewire.c`) owns the 1-Wire timing — a hybrid of a hardware timer (TIM1) and DMA automates every slot; the CPU never waits, never spins, and never enters an interrupt. The first driver on that layer is `src/ds18b20.c`, and other 1-Wire slaves (DS2413, DS2431, ...) can ride it as-is.

The core (`src/onewire.c` + `src/ds18b20.c`) is MCU-independent and rides on a small port interface (`inc/ow_port.h`); per-MCU backends are header-only implementations under `port/`. Three backends ship today:

- `port/stm32f1/ow_port_f1.h` — STM32F103C8T6 (Blue Pill): bus on PA10, TIM1 CH3 output / CH4 capture, DMA1 channels 3/4.
- `port/stm32f0/ow_port_f0.h` — STM32F030x6 (e.g. TSSOP20 STM32F030F4P6): bus on PA10, TIM1 CH3 output / CH4 capture, DMA1 channels 3/4.
- `port/stm32g0/ow_port_g0.h` — STM32G031x6 (e.g. TSSOP20 STM32G031F6P6): bus on PA10 via the SYSCFG PA12 remap, TIM1 CH3 output / CH4 capture, DMA1 channels 3/4 through DMAMUX (requests 21/23).

## Features

- Pure Bare-Metal: Direct register manipulation, no HAL or LL libraries.
- Universal 1-Wire Layer: `inc/onewire.h` + `src/onewire.c` — a reusable,
  non-blocking 1-Wire master. The bus primitives (reset, presence, write/read
  slots, multi-byte read) and the generic Maxim Search ROM engine are
  scheduled on TIM1/DMA and complete asynchronously. `src/ds18b20.c` is built
  on this layer, and other 1-Wire slaves (DS2413, DS2431, ...) can reuse it
  as-is.
- Multi-MCU Backend: One MCU-independent core over a `ow_port_*` interface; header-only backends for STM32F1, STM32F0 and STM32G0, all on the shared CH3/CH4 scheme. Select at build time with `make OW_TARGET=f0` / `make OW_TARGET=g0` (F1 is the default).
- Zero NVIC Interrupts: No NVIC interrupts or ISRs are used. Fully polled operation. The optional `-DOW_PORT_LOW_POWER` mode uses a timer update event (UIE) with `SEVONPEND` solely as a `WFE()` wake-up mechanism — no NVIC interrupt is enabled and no ISR is installed.
- RTOS-Ready: the strict 1-Wire bit timing is generated entirely by TIM1+DMA, so ds18b20_poll() can be called at any rate from an RTOS task without corrupting the bus. The driver is fully polled and interrupt-free, but is not thread-safe by itself — see RTOS Integration.
- Hardware Automation: Uses TIM1 Output Compare and Input Capture with DMA to automate waveform generation and data capture.
- State Machine Architecture: Event-driven operation controlled by hardware completion signals.
 - Weak Function Callbacks: Hooks for driver busy state and measurement completion.
 - CRC Validation: CRC-8 ensures every sensor reading is checked for data integrity.
 - Optional Signal Statistics Module (`ow_stats`): compile-in
   (`-DOW_STATS_ENABLE`) to collect per-sensor pulse-width min/max, a global
   histogram and error counters across measurement cycles.  The dump is
   non-blocking: `ow_stats_dump_start()` + `ow_stats_dump_poll()` streams the
   report over UART at baud-rate pace without overflowing the ring buffer.
   Zero overhead in production builds (all stubs inline to nothing).
 - Non-Blocking Device Search: `ds18b20_search_start()`, `ds18b20_search_poll()`,
   `ds18b20_search_count()` find every DS18B20 on the bus. The engine is the
   generic Search ROM state machine of the shared 1-Wire layer; the driver
   stays a small high-level interface on top of it.
 - Non-Blocking Alarm Search: `ds18b20_alarm_search_start()`,
    `ds18b20_alarm_search_poll()`, `ds18b20_alarm_search_count()` report only the
    DS18B20 devices currently in alarm state (temperature outside the TH/TL
    thresholds set with Write Scratchpad). It uses the same Maxim search engine
    as the device search and leaves the scan-mode device table untouched.
 - Non-Blocking Command Transactions: `ds18b20_read_rom()`,
    `ds18b20_set_alarm_thresholds()`, `ds18b20_read_scratchpad()`,
    `ds18b20_copy_scratchpad()`, `ds18b20_recall_eeprom()` and
    `ds18b20_detect_parasite()` drive the DS18B20 commands
    (0x33 / 0x4E / 0xBE / 0x48 / 0xB8 / 0xB4) with the same poll discipline as
    the device search — each `*_poll()` advances one hardware operation and
    hands the timer back to `ds18b20_poll()` when the transaction finishes.
    See `demo4.c`.
 - Per-Device Addressing: Select one specific sensor by its ROM address
   (`ds18b20_select()`, Match ROM 0x55) for use with multiple devices on one bus.
 - Resolution-Aware Conversion Wait: The driver waits exactly as long as the
   configured conversion resolution requires (93.75ms @ 9-bit … 750ms @ 12-bit),
   so lowering the resolution speeds up the measurement cycle.
 - Non-Blocking Resolution Change: `ds18b20_set_resolution()` /
    `ds18b20_set_resolution_poll()` change the conversion resolution (9..12 bit)
    between measurement cycles with zero busy-waits, mirroring the device search
    state machine. The resolution is also auto-derived from every valid
    scratchpad read (`ds18b20_get_resolution()`).
 - Simultaneous Multi-Device Conversion: `ds18b20_scan_start()` converts every
    discovered sensor in parallel with one broadcast `Convert T` (Skip ROM) and
    reads each one back via Match ROM, so N devices take one conversion wait
    plus N reads. Each reading is reported through `ds18b20_complete()` in
    device-table order; `ds18b20_scan_index()`, `ds18b20_device_rom()` and
    `ds18b20_device_count()` identify the sensors (requires the device search
    to have run first; assumes a uniform resolution).

## Requirements

- Microcontroller: any STM32 with a single advanced-control timer instance
  that satisfies the complete [Required Timer Capabilities](#required-timer-capabilities)
  and DMA topology (currently supported: STM32F103C8T6, STM32F030x6,
  STM32G031x6; see port backends in `port/`).
- Sensor: DS18B20 digital temperature sensor
- Toolchain: GCC ARM (arm-none-eabi)
- Clock Configuration: STM32F103 — 72MHz via HSE+PLL (default) or 8MHz via internal RC (`make SYSCLK_MHZ=8`); STM32F030 — 48MHz via HSI+PLL (default) or 8MHz via internal RC. Both targets take `SYSCLK_MHZ=8`; STM32G031 — 64MHz via HSI16+PLL (default) or 16MHz via internal RC (`SYSCLK_MHZ=16`). The portable `OW_PORT_SYSCLK_MHZ` define carries the value to every clock-dependent setting.

## File Structure

```
├── inc/                    # Project header files
│   ├── ds18b20.h           # Driver interface (high-level API) and constants
│   ├── onewire.h           # Shared 1-Wire layer (bus primitives + Search ROM)
│   ├── ow_stats.h          # Optional signal statistics module (histogram, per-sensor)
│   ├── app.h               # Shared application layer (UART, clock, init)
│   ├── ow_port.h           # 1-Wire port layer interface (+ backend select)
│   └── macro.h             # STM32 register access macros (shared)
├── port/                   # Per-MCU backends for the ow_port_* interface
│   ├── stm32f1/            # STM32F1: TIM1 + DMA1 + PA10 (header-only static inline)
│   │   ├── ow_port_f1.h    # Register-level ow_port_* implementation for STM32F1
│   │   ├── STM32F103XB_FLASH.ld  # Linker script, STM32F103xB (with .noinit section)
│   │   ├── stm32f103cb.jflash    # J-Flash project file (make jprogram)
│   │   └── project.jdebug  # SEGGER Ozone project (STM32F103C8, SWD)
│   ├── stm32f0/            # STM32F0: TIM1 + DMA1 + PA10 (header-only static inline)
│   │   ├── ow_port_f0.h    # Register-level ow_port_* implementation for STM32F0
│   │   ├── STM32F030X6_FLASH.ld  # Linker script, STM32F030x6 (16KB flash / 4KB RAM)
│   │   ├── stm32f030f4.jflash    # J-Flash project file
│   │   └── project.jdebug  # SEGGER Ozone project (STM32F030F4, SWD)
│   └── stm32g0/            # STM32G0: TIM1 + DMA1 + DMAMUX + PA10 via PA12 remap (header-only static inline)
│   │   ├── ow_port_g0.h    # Register-level ow_port_* implementation for STM32G0
│   │   ├── STM32G031X6_FLASH.ld  # Linker script, STM32G031x6 (32KB flash / 8KB RAM)
│   │   ├── stm32g031f6.jflash    # J-Flash project file
│   │   └── project.jdebug  # SEGGER Ozone project (STM32G031F6, SWD)
├── src/                    # Project source files
│   ├── app.c               # app_init(), UART TX ring buffer, busy LED
│   ├── demo.c              # Example: single sensor, unconditional (Skip ROM)
│   ├── demo1.c             # Example: device search + per-device poll (no broadcast convert)
│   ├── demo2.c             # Example: device search + sequential poll of all
│   ├── demo3.c             # Example: device search + simultaneous conversion
│   ├── demo4.c             # Example: device search + command transactions
│   │                       # (ROM, power supply, TH/TL, Copy/Recall EEPROM)
│   ├── demo5.c             # Example: device search + stats dump every N cycles
│   ├── demo6.c             # Example: device search + WFE low-power sleep on long stages
│   ├── ow_stats.c          # Signal statistics implementation (histogram, UART dump)
│   ├── onewire.c           # 1-Wire layer: state machine + bus primitives
│   │                       #               + non-blocking Search ROM engine
│   └── ds18b20.c           # Driver: DS18B20 command set on the 1-Wire layer
├── tests/                  # Host test suite (no hardware required)
│   ├── mock/               # Behavioural TIM1/DMA model + register mocks
│   ├── fuzz/               # libFuzzer harnesses (ASAN/UBSAN, 4 tiers)
│   └── test/               # Unity-based test cases
├── cmake/                  # CMake toolchain
│   └── arm-none-eabi-gcc.cmake  # Bare-metal cross-compilation toolchain file
├── docs/                   # Documentation assets
│   └── screenshots/        # UART capture screenshots
├── .github/                # GitHub configuration
│   ├── workflows/          # CI (build.yml, ci.yml) and release (release.yml)
│   ├── ISSUE_TEMPLATE/     # Bug report / feature request templates
│   └── PULL_REQUEST_TEMPLATE.md
├── CMSIS/                  # Build-time dependencies (gitignored)
│   ├── core/               # ARM CMSIS 5 core headers
│   └── device/             # STM32 device headers and startup (F1/F0/G0) + SVD
├── .vscode/                # VSCode workspace configuration
│   ├── tasks.json          # Build tasks (Ctrl+Shift+B)
│   ├── launch.json         # Debug configuration (F5, J-Link / ST-Link)
│   ├── c_cpp_properties.json  # IntelliSense paths
│   ├── extensions.json     # Recommended extensions
│   └── settings.json       # Editor settings
├── build/                  # Build artifacts (generated)
├── CMakeLists.txt          # CMake build (FetchContent for CMSIS)
├── library.json            # PlatformIO library metadata
├── library.properties      # Arduino Library Manager metadata
├── CHANGELOG.md
├── CODE_OF_CONDUCT.md
├── CONTRIBUTING.md
├── LICENSE                 # MIT
├── Makefile
├── README.md
└── SECURITY.md
```

## Examples

Seven ready-to-run example applications are provided; select one with `APP`:

| APP     | File             | Behaviour                                                        |
|---------|------------------|------------------------------------------------------------------|
| `demo`  | `src/demo.c`     | Unconditional polling of a single DS18B20 via Skip ROM (0xCC).   |
| `demo1` | `src/demo1.c`    | Startup device search + per-device polling: each sensor is converted and read back individually via Match ROM (one `Convert T` per device, no broadcast conversion). |
| `demo2` | `src/demo2.c`    | Startup device search + sequential polling of every sensor found (up to `DS18B20_MAX_DEVICES`). |
| `demo3` | `src/demo3.c`    | Startup device search + simultaneous broadcast conversion: one `Convert T` (Skip ROM) converts all sensors in parallel, then each is read back via Match ROM. |
| `demo4` | `src/demo4.c`    | Startup device search + non-blocking command transactions on the first sensor: Read Power Supply (0xB4), raw Read Scratchpad (0xBE), Write Scratchpad TH/TL (0x4E), Copy Scratchpad (0x48) to the EEPROM, Recall EEPROM (0xB8), single-device Read ROM (0x33), then steady-state measurement of the selected device. |
| `demo5` | `src/demo5.c`    | Startup device search + sequential measurement with signal statistics. The `demo5` target auto-enables `-DOW_STATS_ENABLE`. Accumulates per-sensor pulse-width min/max, a global histogram and error counters over N cycles (shipped build default 5000 via `STATS_DUMP_INTERVAL`, overridable), then streams the full report over UART as a non-blocking dump. |
| `demo6` | `src/demo6.c`    | Low-power example (same search + sequential loop as `demo1`): with `-DOW_PORT_LOW_POWER` the main loop enters `__WFE()` while a long 1-Wire stage (> 1 ms: temperature conversion, scratchpad read, EEPROM hold-off, inter-cycle pause) is running. Without the define, the example uses the standard polling loop. |

```bash
make                # build demo  -> build/ds18b20_demo.elf
make APP=demo1      # build demo1 -> build/ds18b20_demo1.elf
make APP=demo2      # build demo2 -> build/ds18b20_demo2.elf
make APP=demo3      # build demo3 -> build/ds18b20_demo3.elf
make APP=demo4      # build demo4 -> build/ds18b20_demo4.elf
make APP=demo5                   # build demo5 -> build/ds18b20_demo5.elf (OW_STATS_ENABLE auto-added)
make APP=demo6 EXT="-DOW_PORT_LOW_POWER"   # build demo6 -> build/ds18b20_demo6.elf (WFE low-power)
make debug APP=demo2  # debug build of demo2 (for J-Link/ST-Link)

# STM32F030 target (same examples, bus on PA10):
make OW_TARGET=f0 APP=demo3
```

Notes:

- All examples use `app_init()` (from `inc/app.h`) to set up the system
  clock, USART1 TX and the busy LED in a single call.
- `demo` uses Skip ROM, so it is meant for a **single sensor** on the bus.
  With several sensors connected, all of them respond to the read command and
  the bus data collides (CRC failures are expected).
- `demo1` performs a startup Search ROM, then measures each discovered sensor
  **individually** via Match ROM (one `Convert T` per device, no broadcast
  conversion) in round-robin order. A separator `--------------------------------`
  is printed between full rounds. With one sensor it behaves like `demo` but
  with ROM addressing; with N sensors a round costs `N × conversion`. Supports
  `-DPARASITE_POWER=1` (strong pull-up handled per conversion).
- `demo2` measures the devices found at startup one at a time, in round-robin
  order. With exactly one sensor it behaves like `demo`.
- `demo3` (scan mode) converts every discovered sensor in parallel: a single
  conversion wait covers all devices, so N devices take `1 x conversion + N x
  read` instead of `N x conversion`. Each reading is reported through
  `ds18b20_complete()` in device-table order; `ds18b20_scan_index()` /
  `ds18b20_device_rom()` identify the sensor. Scan mode assumes a uniform
  resolution (the config is written broadcast) and is mutually exclusive with
  `ds18b20_select()`.
- `demo4` targets the first sensor found by the search (Match ROM) and runs the
  non-blocking command sequence once at startup: power supply, raw scratchpad,
  TH/TL write with a Copy/Recall pair to demonstrate EEPROM persistence, and
  the single-device Read ROM. Each command advances by one hardware operation
  per `*_poll()` call; `ds18b20_last_command_ok()` verifies the result.
- `demo5` extends the `demo2` sequential loop with signal statistics
  (`-DOW_STATS_ENABLE`, auto-enabled by `make APP=demo5`). After
  `STATS_DUMP_INTERVAL` full rounds (source default 100, shipped `demo5` build
  5000 via `Makefile`) the accumulated per-sensor pulse-width min/max,
  13-bucket histogram (0–60+ µs) and error counters are streamed over UART by
  `ow_stats_dump_poll()` (one line per call, non-blocking); the measurement
  loop is paused during the dump and resumed afterwards via `ow_stats_reset()`.
  Supports `-DPARASITE_POWER=1`.
- Programming targets (`make jprogram` / `make program`) flash whichever
  example is currently selected by `APP`.

## Hardware Verified

### STM32F103C8T6 (Blue Pill)

The following captures were taken on real hardware: STM32F103C8T6 (Blue Pill),
8 × DS18B20 on one 1-Wire bus (PA10), flashed via ST-Link, USART1 TX at
115200 8N1 read through a CP2102 USB-UART adapter. The shared 1-Wire layer
found all 8 sensors, and every measurement round reported all of them — no
missing devices, no CRC failures.

**demo2 — device search + round-robin + resolution cycling** (`src/demo2.c`):
the startup Search ROM finds all 8 devices, then each sensor is measured in
turn while the resolution cycles 9 → 10 → 11 → 12 bit between measurements.

<p align="center">
  <img src="docs/screenshots/demo2_uart.png" alt="demo2 on real hardware: device search, round-robin measurement, resolution cycling" width="600">
</p>

**demo3 — simultaneous multi-device conversion** (`src/demo3.c`): one broadcast
`Convert T` converts all sensors in parallel, then each is read back via
Match ROM — 8 readings per round in device-table order.

<p align="center">
  <img src="docs/screenshots/demo3_uart.png" alt="demo3 on real hardware: simultaneous multi-device conversion (scan mode)" width="600">
</p>

**demo4 — command transactions** (`src/demo4.c`): after the startup search, the
first sensor (Match ROM) answers every non-blocking command in turn — external
power confirmed, raw scratchpad read with CRC ok and the resolution auto-derived
from the config byte, TH/TL written (0x19/0x0F), copied to the EEPROM, then a
volatile write (0x05/0x02) and Recall restoring the persisted values, and the
bare Read ROM reporting a CRC failure as expected with 8 devices on the bus
(0x33 is single-device only).

<p align="center">
  <img src="docs/screenshots/demo4_uart.png" alt="demo4 on real hardware: command transactions (power supply, scratchpad, TH/TL, EEPROM, Read ROM)" width="600">
</p>

### STM32F030F4P6 (TSSOP20)

The same examples were validated on an STM32F030F4P6 minimum board: the
1-Wire bus on **PA10** (TIM1 CH3/CH4 pair — PA8 is not bonded out in this
package), USART1 TX on PA9, busy LED on PA4, flashed via ST-Link SWD. The bus
again carried 8 × DS18B20; all rounds complete with valid CRCs and no errors:

| Test | Clock | Result |
|------|-------|--------|
| `demo2` — search + round-robin + resolution cycling | HSI+PLL 48MHz | 163 samples / 7+ sensors, 0 CRC or timeout errors |
| `demo2` — same | HSI 8MHz | 161 samples, 0 errors |
| `demo3` — simultaneous conversion scan | HSI+PLL 48MHz | all 8 devices found, 56 readings (7 × 8), 0 errors |
| `demo3` — same | HSI 8MHz | all 8 devices found, 56 readings, 0 errors |
| `demo4` — command transactions validator | both clocks | all checks pass (power supply, TH/TL write, Copy/Recall EEPROM round-trip, expected multi-device Read ROM CRC failure) |

### STM32G031F6P6 (WeAct TSSOP20 board)

Validated on a WeAct STM32G031F6P6 minimum board: 6 × DS18B20 in parasite
power mode on one 1-Wire bus (logical PA10 on the physical PA12 pad), USART1
TX on logical PA9 (physical PA11), flashed via ST-Link SWD. `demo3`
(simultaneous conversion scan) runs with every device reported each round,
valid CRCs and zero errors at both supported clocks — the default 64MHz
(HSI16+PLL) and the raw-HSI16 `SYSCLK_MHZ=16` build, which exercises the
slow-clock timing path natively. The bus pads are reachable only through the
SYSCFG remap described in Hardware Connections below; the USB-C connector of
this board is wired to PA11/PA12 and must stay unplugged while the driver
owns the bus.

**demo5 — signal statistics** (`src/demo5.c`): startup device search +
sequential measurement with the optional `ow_stats` module. Over 100
measurement cycles (configurable via `STATS_DUMP_INTERVAL`), the module
accumulates per-sensor pulse-width min/max, a 13-bucket logarithmic histogram
(0–60+ µs) and error counters (CRC, presence, other), then streams the full
report over UART. Validated on STM32G031@64MHz with 6 × DS18B20 in parasite
power mode — all six sensors detected, 0 errors, pulse widths 5–32 µs,
histogram buckets populated across the normal decode range.

Build and run:

```sh
make OW_TARGET=g0 APP=demo5 EXT="-DOW_STATS_ENABLE -DPARASITE_POWER=1"
```

**demo6 — low power** (`src/demo6.c`): the same search + sequential loop as
`demo1`, but built with `-DOW_PORT_LOW_POWER`.

> **What low-power mode does not change.** Low-power mode does not change
> 1-Wire execution. TIM+DMA continue to control all bus timing; `WFE` allows
> the CPU to sleep during sufficiently long hardware-controlled transaction
> stages.

The one-wire driver then enables the TIM1 update interrupt (UIE) and the
`SEVONPEND` system-control bit, so the application main loop can block in
`__WFE()` while a *long* 1-Wire stage is running and be woken by the timer's
update event — no ISR is ever installed, no `NVIC_EnableIRQ` call is made, and
the driver itself stays fully non-blocking. Stages treated as "long" (strictly
> 1 ms) are the temperature conversion (up to 750 ms), the scratchpad read
(~5 ms), an EEPROM hold-off (10 ms) and the inter-measurement pause; short
stages (reset, commands, search reads) are still handled by standard polling.
Power is **not measured** yet — this demo's goal is only to establish the
mechanism and measure the CPU-time saving.

> **Verified on hardware (STM32F103C8 Blue Pill).** With
> `-DOW_PORT_LOW_POWER -DPARASITE_POWER=1` and six DS18B20 sensors powered
> parasitically, demo6 found all six devices, read them in turn (*24.0 °C /
> 85.0 °C / 23.8 °C ...*) and the core demonstrably entered `__WFE()`: a
> temporary instrumented run printed `[WFE iters=1]` before every measurement,
> i.e. the first `__WFE()` after arming the long stage blocked and woke exactly
> once on the timer's update event. The sleep path keeps the driver fully
> functional (no ISR, no `NVIC_EnableIRQ`), only the CPU stops spinning while a
> >1 ms stage runs.

Build and run:

```sh
make OW_TARGET=g0 APP=demo6 EXT="-DOW_PORT_LOW_POWER"   # (append -DPARASITE_POWER=1 on a parasite bus)
make OW_TARGET=g0 APP=demo6                              # same example, but standard polling (define omitted)
```

## Hardware Connections

### STM32F103 (Blue Pill)

#### DS18B20 Sensor

| STM32F103 Pin | Function     | DS18B20 Pin |
|---------------|--------------|-------------|
| PA10          | 1-Wire Data  | DQ (Data)   |
| 3.3V          | Power        | VDD         |
| GND           | Ground       | GND         |

Note: A 4.7kΩ pull-up resistor is required between the PA10 and 3.3V lines.

#### Debug UART (optional)

| STM32F103 Pin | Function            | USB-UART Adapter |
|---------------|---------------------|------------------|
| PA9           | USART1 TX (115200)  | RX               |
| GND           | Ground              | GND              |

Connect a USB-UART adapter to see diagnostic output (sensor errors,
temperature readings). No RX connection is needed — the firmware is
transmit-only.

The UART output uses a ring buffer with polled TX (TXE flag checked
in main loop) — fully non-blocking, no interrupts.

### STM32F030 (e.g. STM32F030F4P6, TSSOP20)

| Pin  | Function            | Notes                              |
|------|---------------------|------------------------------------|
| PA10 | 1-Wire Data         | TIM1_CH3, open-drain AF2 (default topology) |
| PA9  | USART1 TX (115200)  | RX line of the USB-UART adapter    |
| PA4  | Busy LED (optional) | Active-high                        |
| PA13/PA14 | SWDIO/SWCLK    | ST-Link SWD programming            |

Note: the same 4.7kΩ pull-up is required between PA10 and 3.3V.

### STM32G031F6P6 (TSSOP20)

The STM32G0 backend is hardware-validated (see Hardware Verified above); the
notes below cover the TSSOP20 package wiring:

| Pin | Function | Notes |
|-----|----------|-------|
| PA12 | 1-Wire Data (logical PA10) | TIM1_CH3 AF2 after the SYSCFG `PA12_RMP` remap; open-drain AF (default topology) |
| PA11 | USART1 TX (logical PA9) | AF1 after the `PA11_RMP` remap |
| PA4 | Busy LED (optional) | Active-low |
| PA13/PA14 | SWDIO/SWCLK | ST-Link SWD programming |

Important: while the driver is initialised, pads PA11/PA12 must not be used as standalone GPIOs - configuring them as PA11/PA12 clears the SYSCFG remap bits and silently disconnects the bus.

Note: the same 4.7kΩ pull-up is required between the bus pin and 3.3V.

Note: "open-drain" above describes the **default/idle** bus topology, not a
static pin configuration. With the optional active-drive write mode
(`-DOW_DRIVE_ACTIVE`) the pin is temporarily switched to push-pull during
master-only write slots and restored to open-drain afterwards — see
[Bus Electrical Model](#bus-electrical-model).

## Quick Start

### 1. Include the Driver

```C
#include "ds18b20.h"
```

> Working with a non-DS18B20 1-Wire slave (DS2413, DS2431, ...)? Include
> `"onewire.h"` instead and build directly on the shared bus primitives
> (`onewire_reset()`, `onewire_write_then_read()`, the Search ROM engine) —
> no DS18B20 code is pulled in.

### 2. Initialize the Driver

```C
int main(void) {
    ds18b20_init();  // One-time initialization

    // Optional: run the non-blocking device search to find every sensor on
    // the bus. See demo2.c for a complete example. The search hands the
    // driver back to poll() automatically when finished.

    // Optional: measure one specific device by its ROM address
    ds18b20_select(my_rom);  // my_rom from a bus search

    while (1) {
        ds18b20_poll();  // Call repeatedly from main loop
        // Other application code...
    }
}
```

### 3. Implement Callbacks (Optional)

Both callbacks are optional. Default weak implementations are provided by the
driver, and `src/app.c` additionally supplies a default `ds18b20_busy()` that
drives the onboard LED (PC13). The examples override both: `ds18b20_busy()`
switches the LED and `ds18b20_complete()` formats and prints the result.

```C
// Busy indicator — e.g. LED toggling during measurement
void ds18b20_busy(unsigned action) {
    if (action) {
        // Turn LED on (measurement in progress)
        GPIOC->BSRR = GPIO_BSRR_BR13;
    } else {
        // Turn LED off (measurement complete)
        GPIOC->BSRR = GPIO_BSRR_BS13;
    }
}

// Measurement complete callback — handle result or error
void ds18b20_complete(int16_t temp) {
    if (temp >= -550 && temp <= 1250) {
        // Valid temperature in tenths of °C
        printf("Temperature: %d.%d°C\n", temp/10, abs(temp%10));
    } else {
        // Error condition
        switch (temp) {
            case DS18B20_TEMP_ERROR_NO_SENSOR:
                printf("Error: No sensor detected\n");
                break;
            case DS18B20_TEMP_ERROR_CRC_FAIL:
                printf("Error: CRC check failed\n");
                break;
        }
    }
}
```
## Building

### Prerequisites

-   **Toolchain:** `arm-none-eabi-gcc` (GCC 12+ recommended) and related
    utilities (`objcopy`, `size`). Clang is **not** a supported firmware
    toolchain — Clang references in this project refer to optional
    host-side tooling only (fuzz testing, `clang-format`, static analysis).
-   **wget:** Required for downloading CMSIS build dependencies.
-   **Programmer tools:**
    -   **ST-LINK:** `st-flash` (Linux/macOS) or `ST-LINK_CLI.exe` (Windows)
    -   **J-LINK:** `JFlashExe` / `JFlash.Exe` / `JLinkGDBServerCL.exe`

### CMSIS Dependencies

ARM CMSIS core headers and STM32 device files (F1/F0/G0) are not stored in the
repository. They are downloaded automatically at build time to
`CMSIS/core/` and `CMSIS/device/`:

```bash
make download-deps
```

To remove them:

```bash
make clean-deps
```

License files are also downloadable:

```bash
make download-licenses
```

### Build

```bash
make            # Release build (-Os -flto -g0)
make debug      # Debug build (-Og -g3 -gdwarf)
```

Output goes to `build/` (`ds18b20_demo.elf`, `.hex`, `.bin`).

### Common Targets

| Target | Description |
|--------|-------------|
| `make` / `make all` | Build release |
| `make debug` | Build with debug symbols |
| `make test` | Build and run host tests (PC toolchain) |
| `make clean` | Remove build artifacts |
| `make download-deps` | Download CMSIS dependencies |
| `make clean-deps` | Remove downloaded dependencies |
| `make program` | Flash via ST-LINK |
| `make jprogram` | Flash via J-LINK |
| `make test-f0` | Build and run host tests against the STM32F0 backend mock |
| `make test-g0` | Build and run host tests against the STM32G0 backend mock |
| `make test COVERAGE=1` | Host tests with gcov instrumentation (coverage report) |
| `make test-active` | Build and run host tests for the active-drive write path (`-DOW_DRIVE_ACTIVE`) |
| `make test-active-f0` | Same as above against the STM32F0 backend mock |
| `make test-active-g0` | Same as above against the STM32G0 backend mock |
| `make fuzz-all` | Build and run all fuzz harnesses (requires host-side Clang; `FUZZ_TIME=N` for duration) |
| `make fuzz-crc8` | Fuzz `onewire_crc8` alone |
| `make help` | Show all targets |

Optional build flags (append via `EXT="..."` or `OW_DRIVE_ACTIVE=1`):

| Flag | Effect |
|------|--------|
| `OW_DRIVE_ACTIVE=1` | Enable the optional active-drive write path (`-DOW_DRIVE_ACTIVE`): during master-only write slots the bus pin is temporarily switched to push-pull (see [Bus Electrical Model](#bus-electrical-model)). The default remains open-drain. |
| `TIMING=SLOW` | Override the compile-time default timing profile (default `STANDARD`; also `FAST`/`SLOW`/`ROBUST`). Low-level: `EXT="-DOW_TIMING_DEFAULT=ONEWIRE_TIMING_SLOW"` or `EXT="-DONEWIRE_TIMING_PROFILE_DEFAULT=..."`. See [Configuration → Timing Profiles](#timing-profiles). |
| `EXT="-DPARASITE_POWER=1"` | Build for parasite-powered buses (enables the strong-pull-up window; see demo5). |
| `EXT="-DOW_PORT_LOW_POWER"` | Enable the opt-in low-power path: TIM1 UIE + `SEVONPEND` so the application can `__WFE()`-sleep during long 1-Wire stages (> 1 ms) while the hardware completes the transaction. The driver itself stays non-blocking; no ISR is installed. Without this define builds are byte-identical to the original. |

### Flash

-   **ST-LINK:** `make program` (uses `st-flash` / `ST-LINK_CLI.exe`)
-   **J-LINK:** `make jprogram` (uses `JFlashExe` / `JFlash.Exe`)

### Testing

The driver ships with a host test suite that runs entirely on the PC, no
hardware required:

```bash
make test        # host tests against the STM32F1 backend mock
make test-f0     # same suite against the STM32F0 backend mock
make test-g0     # same suite against the STM32G0 backend mock
```

Both `src/onewire.c` and `src/ds18b20.c` are compiled as a single translation
unit (`tests/mock/ds18b20_test_access.c`) against a behavioural model of the
TIM1/DMA hardware (`tests/mock/hw_model.c`) and a register mock of the target
CMSIS header — each suite runs the full driver against its own backend's
channel/DMA wiring. 267 tests per backend cover:

-   State machine transitions (idle → start → measure → read → decode)
-   Non-blocking device search (Search ROM, ROM CRC validation, multi-device)
-   Non-blocking alarm search (Alarm Search ROM, 0xEC command feed, scan-table
    isolation)
-   Non-blocking resolution change (`ds18b20_set_resolution_*`): exact wait
    timings for 9/10/11/12 bit, Skip ROM and Match ROM config writes, CCR3-feed
    bus release, ownership guards, presence-abort and scratchpad auto-derivation
-   Non-blocking command transactions (`ds18b20_read_rom`,
    `ds18b20_set_alarm_thresholds`, `ds18b20_read_scratchpad`,
    `ds18b20_copy_scratchpad`, `ds18b20_recall_eeprom`,
    `ds18b20_detect_parasite`): command feed builds (Skip/Match ROM),
    resolution-preserving TH/TL writes, raw scratchpad read + CRC +
    resolution auto-derivation, 10 ms Copy/Recall hold-offs, power-supply
    decode, ownership guards, presence-abort and result reporting
-   CRC-8 (Dallas/Maxim) verification
-   1-Wire pulse encoding and presence detection
-   1-Wire layer coverage: reset/presence timing, write-then-read merge,
      multi-slot writes, multi-byte reads, search engine (device + alarm),
      ownership guards and the search edge buffers
-   Scratchpad decode and temperature conversion (incl. negative values)
-   Timing configuration and register setup
-   Bus release behaviour between slots

### PlatformIO

The repository ships with `library.json` and `library.properties` so
PlatformIO can discover the library automatically.

```bash
# One-time: fetch CMSIS headers (the ststm32 platform provides its own
# copy, but the driver's ow_port_* headers expect the standard layout
# under CMSIS/core/ and CMSIS/device/).
make download-deps
```

Minimal `platformio.ini`:

```ini
[env:bluepill]
platform  = ststm32
board     = bluepill_f103c8
framework = stm32cube
lib_deps  = a5021/stm32-async-1wire
```

Or point to a local checkout:

```ini
lib_deps = symlink:///path/to/stm32-async-1wire
```

The library is also compatible with the Arduino Library Manager (see
`library.properties`).

### CMake (FetchContent)

The root `CMakeLists.txt` provides a `stm32_async_1wire` static library
target with FetchContent-managed CMSIS dependencies — no pre-downloaded
headers needed. A bare-metal toolchain file is included.

```bash
# ARM GCC must be on PATH
cmake -DCMAKE_TOOLCHAIN_FILE=cmake/arm-none-eabi-gcc.cmake \
      -DOW_TARGET=f1 -B build .
cmake --build build
```

Select the MCU family with `-DOW_TARGET=f1` (default), `f0`, or `g0`.

In a downstream project:

```cmake
FetchContent_Declare(stm32_1wire
    GIT_REPOSITORY https://github.com/a5021/stm32-async-1wire.git
    GIT_TAG        v1.7.1
)
FetchContent_MakeAvailable(stm32_1wire)
target_link_libraries(your_app PRIVATE stm32_async_1wire)
```

### STM32CubeIDE

1.  Run `make download-deps` once to fetch CMSIS headers.
2.  In STM32CubeIDE: **File → New → STM32 Project from an Existing Makefile**.
3.  Point to the repository root directory.
4.  The IDE auto-generates the CDT project; select your MCU target
    (e.g. STM32F103C8).
5.  Build and flash as usual.

### **Configuration Notes**

-   **Target Name:** The firmware target name is `ds18b20_demo`.

-   **Build Directory:** Default is `build/`.

-   **Optimization Level:**

    -   **Release:** `-Os -flto -g0` (default).
    -   **Debug:** `-Og -g3 -gdwarf`.

-   **MCU Flags:** `STM32F103xB` (Cortex-M3) by default; `STM32F030x6`
    (Cortex-M0) with `OW_TARGET=f0`; `STM32G031xx` (Cortex-M0+) with
    `OW_TARGET=g0`.

-   **Target Selection:** `make OW_TARGET=f0` builds for the STM32F0 backend
    (48MHz default clock, `port/stm32f0/STM32F030X6_FLASH.ld`),
    `make OW_TARGET=g0` for the STM32G0 backend (64MHz default clock,
    `port/stm32g0/STM32G031X6_FLASH.ld`). The default target is STM32F103
    (bus on PA10 for F1/F0, logical PA10 via PA12 remap for G0).

-   **8MHz RC Build:** By default the firmware runs on HSE 8MHz + PLL
    ×9 = 72MHz. Pass `SYSCLK_MHZ=8` to use the internal RC oscillator
    (HSI) at 8MHz without an external crystal or PLL:

    ``` bash
    make SYSCLK_MHZ=8
    make debug SYSCLK_MHZ=8
    ```

    On the STM32F030 target the default clock is already HSI+PLL (48MHz);
    there `SYSCLK_MHZ=8` selects the raw 8MHz HSI instead.

    The knob maps to the portable `OW_PORT_SYSCLK_MHZ` define — a single
    value in MHz that every clock-dependent setting derives from: the
    timer prescaler, the input-capture filter and the USART baud rate
    adjust automatically. Useful for testing on bare minimum hardware
    (no HSE crystal).

## VSCode Integration

The repository includes `.vscode/` workspace configuration for a
convenient development workflow.

### Building

Press **Ctrl+Shift+B** to run the default build task (`make`). Other
tasks are available via **Ctrl+Shift+P** → "Tasks: Run Task":

- `Build (release)` — `make` (default)
- `Build (debug)` — `make debug`
- `Build F0 (debug)` — `make OW_TARGET=f0 debug` (debug build for the STM32F030 target)
- `Build G0 (debug)` — `make OW_TARGET=g0 debug` (debug build for the STM32G031 target)
- `Clean` — `make clean`
- `Program (J-Link)` / `Program (ST-Link)` — flash the device
- `Download dependencies` — `make download-deps`

### Debugging

1. In the **Run and Debug** panel (`Ctrl+Shift+D`), select the debug
   configuration: **"Debug F1 (J-Link)"** / **"Debug F1 (ST-Link)"** for
   the STM32F103 target, or **"Debug F0 (J-Link)"** /
   **"Debug F0 (ST-Link)"** for the STM32F030 target, or **"Debug G0
   (J-Link)"** / **"Debug G0 (ST-Link)"** for the STM32G031 target. The F0
   configurations build with `OW_TARGET=f0` automatically, the G0 ones with
   `OW_TARGET=g0`.
2. Open `src/demo.c` and set a breakpoint in `main()`.
3. Press **F5** — Cortex-Debug will build the firmware in debug mode,
   flash it, run to `main()`, and halt.

The SVD file for the selected family is downloaded by `make download-deps`
and loaded automatically for peripheral register views in the debug
sidebar. Standalone SEGGER Ozone users can open `port/<mcu>/project.jdebug`
from either backend directory; the project resolves its SVD and ELF paths
relative to its own location.

**J-Link:** Connect a SEGGER J-Link debugger via SWD.  
**ST-Link:** Connect an ST-Link programmer (built into most Blue Pill
boards) via SWD.

## Comparison with Common 1-Wire Techniques

The DS18B20 uses the 1-Wire bus protocol, which communicates over a single data
line with strict timing requirements. Several approaches exist to handle this
protocol on embedded systems:

| Technique | How it works | Blocking? | Timing precision | Typical use |
|---|---|---|---|---|
| **Bit-banging + delay** (e.g. OneWire Arduino) | GPIO toggling with `delayMicroseconds()`, interrupts disabled | Yes | Low (compiler/optimization dependent) | Hobbyist Arduino projects |
| **Bit-banging + timer ISR** | Timer interrupt drives GPIO transitions | Semi- | Medium | RTOS-based firmware |
| **UART bit-banging** | UART at 9600/115200 baud emulates 1-Wire timings | Depends | Medium | Systems with spare UARTs |
| **Hardware 1-Wire master** | Dedicated IC (DS2482) or kernel subsystem (Linux w1-gpio) | No | High | Linux SBCs, complex systems |
| **Timer + DMA + One-Pulse Mode** (this driver) | DMA feeds CCR values autonomously; timer self-disables after each transaction | No | High (1µs resolution, zero jitter) | STM32 resource-constrained firmware |

### Trade-offs

**Cost.** This driver consumes dedicated hardware resources — TIM1 and two DMA1
channels (the capture drain from CCR4 and the marker feed into CCR3) — that
cannot be used for other purposes. The 1-Wire data line itself occupies one GPIO (PA10), but any approach
needs a GPIO pin for the bus, so that is not an extra cost. Bit-banging
approaches, by contrast, need only that one pin and no DMA, making them more
portable across MCUs with limited peripherals.

**Precision vs. portability.** Timer+DMA provides deterministic 1µs resolution
with zero jitter, because the CPU is never in the timing-critical path. Software
delays degrade under interrupt load, and even timer-ISR approaches incur jitter
from preemption. The trade-off is complexity: this driver's hardware configuration
is ~150 lines of register-level code versus ~20 lines for a typical bit-bang
implementation.

## Architecture

### Hybrid Hardware Automation

The 1-Wire layer uses a hybrid of several hardware features:

1. Timer-Driven Sequences: TIM1 is configured in One-Pulse Mode (OPM). Each state machine step configures the timer for a specific operation (reset, write byte, read byte, wait) and starts it.
2. DMA for Data Transfer: DMA is used in two key ways:
   - Transmit: Feeds a pre-calculated sequence of Compare Register (CCR) values to TIM1->CCR3 to automatically generate the precise waveform for writing commands or bits. The feed request comes from the CH2 slot-end marker compare and rides DMA1 channel 3.
   - Capture: Automatically stores values from the TIM1->CCR4 capture register into memory to record pulse timings during read operations or presence detection; the capture drain rides DMA1 channel 4.
3. Update Event as Completion Signal: The core polling mechanism checks the Timer Update Flag (TIM1->SR UIF). This flag is set when the timer completes its one-pulse countdown, signaling that the autonomous hardware operation (e.g., sending a reset pulse, waiting 750ms) is finished.
4. True Zero-ISR Overhead: The ds18b20_poll() function checks this flag. When set, it clears the flag and advances the state machine to the next step. This makes the entire driver event-driven by hardware completion signals without using interrupts.

### Non-Blocking, Interrupt-Free Design Principles

1. No Software Delays: No `delay_us()` or similar functions.
2. No Interrupts: Does not configure or use the NVIC. Fully deterministic.
3. Hardware Completion Events: The state machine advances only when the hardware timer signals that its current automated task is complete.
 4. Minimal CPU During Operations: The CPU is only actively involved to set up a hardware operation and to process the result once it completes.

> The driver ships with a built-in non-blocking device search
> (`ds18b20_search_*`) for multi-sensor buses. The Maxim Search ROM (0xF0)
> algorithm is implemented as a compact state machine in the shared 1-Wire
> layer; it performs exactly one hardware-timed operation per poll call,
> consistent with the non-blocking measurement path. See `demo2.c` for a
> complete Search ROM example.

### Shared 1-Wire Layer

All bus-level protocol lives in `src/onewire.c` (interface in `inc/onewire.h`),
a reusable 1-Wire master that the DS18B20 driver builds on:

- `onewire_init()`, `onewire_reset()`, `onewire_present()`,
  `onewire_write_slots()`, `onewire_write_bit()`, `onewire_encode_byte()`,
   `onewire_read_pair()`, `onewire_write_then_read()`, `onewire_pair_bits()`,
   `onewire_read_data()`, `onewire_decode_pulses()`, `onewire_start_timer()`,
   `onewire_bus_done()` — the
  TIM1/DMA bus primitives.
- `onewire_search_start()`, `onewire_search_poll()`,
  `onewire_search_count()`, `onewire_search_active()` — the generic Maxim
  Search ROM engine, shared by `ds18b20_search_*` and
  `ds18b20_alarm_search_*`.
- `onewire_crc8()` — the Dallas/Maxim CRC-8 utility.

Every operation is scheduled as one hardware transaction on TIM1/DMA and
completes asynchronously; the caller advances it by polling
`onewire_bus_done()` / `onewire_search_poll()`. The layer owns its own capture
edge buffers, keeps the line released to idle HIGH after every transaction, and
is fully covered by the host test suite. See the API Reference below for the
complete `onewire_*` surface.

### Required Timer Capabilities

The contract describes the functional peripheral topology required by the
driver.  A new backend is valid only if the target MCU can realise the
complete topology simultaneously on **one timer instance** and its DMA
routing; having the individual features somewhere in the MCU is not
sufficient.  The topology is:

```
                ONE TIMER
                    │
        ┌───────────┼───────────┐
        ▼           ▼           ▼
      CH2         CH3         CH4
        │           │           │
      DMA         GPIO        IC
                    │           │
                    └── TI3 ◄───┘
```

The driver does not care which timer is used — it cares about what the timer
can do.  The following capabilities are hard requirements; a timer that lacks
any one of them cannot run this driver, which is why the port layer
(`port/stm32f*/ow_port_*.h`) pins each backend to one specific timer rather
than letting the user choose.

#### 1. Advanced-control timer (not general-purpose)

| Capability | Why it is required |
|---|---|
| **RCR (Repetition Counter)** | Batches *N* PWM periods into a single Update Event.  Without RCR, every bit slot generates its own update — a 16-bit command would need 16 interrupt handlers or 16 poll rounds instead of one.  RCR=15 lets the timer autonomously generate 16 slots, then assert UIF once so the state machine advances in a single step.  *Only advanced-control timers (TIM1/TIM8 on STM32) have RCR.* |
| **BDTR + MOE (Main Output Enable)** | Gates the entire output stage.  The driver sets MOE once at init and never touches it again; when the timer stops (OPM) the output goes high-impedance and the external pull-up takes over.  General-purpose timers lack BDTR — their output is always driven. |
| **One-Pulse Mode (OPM)** | The timer auto-stops after the scheduled operation completes (one reset pulse, one byte write, one long conversion wait).  Each bus transaction is a self-contained hardware run; OPM guarantees the timer does not free-run and re-enter a spurious slot. |

#### 2. Channel topology

The driver requires three channels on the same timer, with a specific
capture-routing relationship:

| Channel | Role | Requirement |
|---|---|---|
| **CH3** | PWM output (active-low) | PWM Mode 2, pin drives the 1-Wire bus.  Each bit slot is one PWM period: short low (~5 µs) = '1', long low (~60 µs) = '0'.  Reset is an extended low within a ~960 µs slot. |
| **CH4** | Input capture (indirect) | **Must be routable to TI3** (the CH3 pin) via `CC4S=01`.  This is the constraint that eliminates many timer/pin combinations: only one input capture channel on each STM32 timer can watch a given output channel's pin, and on every supported family that channel is IC4→TI3.  CH4 captures presence pulses and read-slot timings after CH3 releases the bus to idle-HIGH. |
| **CH2** | Compare (end-of-slot marker) | A plain compare at `ONEWIRE_ONE_PULSE + ONEWIRE_ZERO_PULSE` µs whose DMA request feeds the next CCR3 value from a precomputed pulse buffer.  CH2's pin is unused — only its compare event and DMA request matter. |

The indirect-capture constraint (IC4 must see TI3) is why the driver cannot
be moved to an arbitrary pin: the chosen GPIO must be the CH3/CH4 pair's
output/capture pin on the selected timer.  On STM32F1 this is PA10
(default AFIO map); on STM32F0, PA10 (AF2); on STM32G0, PA12 pad remapped
to logical PA10 via `SYSCFG_CFGR1.PA12_RMP`.

#### 3. DMA

Two DMA channels are required, each carrying a specific peripheral request:

| Channel | Direction | Request | Purpose |
|---|---|---|---|
| DMA channel A | Memory → Peripheral | TIM1_CC2 (CH2 compare) | Feeds CCR3 with the next pulse width on each slot boundary (the "feed" path). |
| DMA channel B | Peripheral → Memory | TIM1_CH4 (capture) | Drains CCR4 capture values into a memory buffer (the "capture" path). |

F0/F1 have a fixed request map (no `DMA_CSELR` mux) — channel 3 = CC2,
channel 4 = CC4, confirmed empirically.  G0 uses a DMAMUX: TIM1_CC2 = request
21, TIM1_CH4 = request 23.  A new backend must verify the DMA request
numbers for its target; the channel roles are identical across all families.

#### 4. Clocking invariant

The APB prescaler feeding the timer **must be /1**.  STM32 timers double
their clock when the APB prescaler is >1 (`timer clock = 2 × PCLK`), which
would break every µs-based timing constant in the driver.  All three
supported families satisfy this by construction: F1 keeps PPRE2=/1 (TIM1 is
on APB2), F0 and G0 have a single APB bus at /1.

#### 5. GPIO

The bus pin must support alternate-function open-drain (for normal bus
operation) and runtime switching to alternate-function push-pull (for
parasite-power strong pull-up and the optional active-drive write path).
The pin never leaves alternate function — only the output-stage topology
changes.

#### Supported families (same scheme, different prescaler and pin config)

| Family | Timer | Bus pin | DMA routing | Notes |
|---|---|---|---|---|
| STM32F1 | TIM1 | PA10 (default AFIO) | Fixed: CH3→DMA1 ch3, CH4→DMA1 ch4 | APB2=/1 by default |
| STM32F0 | TIM1 | PA10 (AF2) | Fixed: same mapping | TSSOP20: PA8 not bonded out, CH3/CH4 is the only viable pair |
| STM32G0 | TIM1 | PA10 via PA12 remap | DMAMUX: CC2=#21, CH4=#23 | SYSCFG `PA12_RMP`; PA11/PA12 cannot be used as GPIO while driver is active |

#### Bus Electrical Model

- **Open-drain bus.** PA10 is configured as an alternate-function **open-drain**
  pin and idles HIGH; a single external pull-up resistor on the bus is required.
  Both the master and every slave are open-drain, so the bus is a **wired-AND**:
  if any device drives the line LOW the bus is LOW, otherwise it is pulled HIGH.
- **Normal slot signaling (master never drives HIGH).** A `write-0` or bus reset
  is the master actively pulling the line LOW; a `write-1` or read slot is the
  master **releasing** the pin to Hi-Z and letting the external pull-up return
  the line HIGH. The master therefore never actively drives the line HIGH during
  a normal slot — `write-1` is a *release*, not a push-pull HIGH.
- **Parasite strong-pull-up (the only unconditional push-pull usage in the default build).** `onewire_strong_pullup()`
  (`ow_port_strong_pullup()`) is the *only* place the pin is switched to
  push-pull in the default build; it actively sources current by driving the line
  HIGH during the temperature-conversion / EEPROM-programming window — a phase
  where the DS18B20 is silent and cannot respond on the bus. It is restored to
  open-drain afterwards.

- **Active-drive write mode (optional).** During master-only write slots the bus
  pin may be placed in alternate-function push-pull, actively driving both HIGH
  and LOW levels instead of relying on the pull-up for HIGH. The driver
  automatically retains open-drain operation for reset, presence, read slots and
  write/read transactions where the slave may drive the bus. This is what makes
  push-pull acceptable on a 1-Wire bus at all: it is confined to phases where no
  slave can answer. Enabled with `-DOW_DRIVE_ACTIVE`; the published default
  remains open-drain.

### State Machine Flow (hardware-timed; polled on UIF)

Kickstart behavior
- After ds18b20_init(), the timer update flag (UIF) is already set. This ensures the very first call to ds18b20_poll() advances the state machine immediately without any extra priming step.

- IDLE (state 0)
  - Immediately falls through into START with no events required. Prepares context (ctx.fill_union = -1), ensures LED is off.
  - Set state=1.

- START (state 1)
  - LED on. Run reset_bus():
    - CH3 issues active-low reset pulse (~480µs within ~960µs slot).
    - CH4 (indirect input) captures presence timing into ctx.edge[0..1] via DMA from CCR4.
  - Set state=2.

- CONVERT (state 2)
  - On UIF, check_presence() with ctx.edge[].
    - If present: send convert command via CH3+DMA. With no selected device,
      this is "Skip ROM 0xCC + Convert T 0x44" (16 slots, RCR=15). With a
      device selected via ds18b20_select(), it is "Match ROM 0x55 + 8-byte ROM
      + Convert T 0x44" (80 slots, RCR=79). Set state=3.
    - Else: report NO_SENSOR; start 5s pause; set state=0.

- WAIT (state 3)
  - On UIF: wait_conversion() schedules exactly the conversion time of the
    configured resolution `ctx.resolution` (9-bit: 10×9.375ms; 10-bit:
    10×18.75ms; 11-bit: 20×18.75ms; 12-bit: 12×62.5ms = 750ms); set state=4.

- CONTINUE (state 4)
  - On UIF: run reset_bus() again; set state=5.

- REQUEST (state 5)
  - On UIF, check_presence().
    - If present: send read command via CH3+DMA. With no selected device,
      this is "Skip ROM 0xCC + Read Scratchpad 0xBE" (16 slots). With a device
      selected, it is "Match ROM 0x55 + 8-byte ROM + Read Scratchpad 0xBE"
      (80 slots). Set state=6.
    - Else: report NO_SENSOR; start 5s pause; set state=0.

- READ (state 6)
  - On UIF: read_data() schedules 72 slots (RCR=71; ARR=70µs). CH3 emits ~5µs active-low kick at each slot start and then releases; CH4 captures sensor pulse timing; DMA fills ctx.pulse[72]. Set state=7.

- DECODE (state 7)
  - On UIF: decode_scratchpad() from ctx.pulse[] into ctx.scratchpad[], LED off; verify CRC; report temperature or CRC_FAIL; start 5s pause; set state=0.

#### Detailed State Descriptions

| State Number | State Name     | Description                                                                                                                              | Next State(s)                                                                                               |
| :----------- | :------------- | :--------------------------------------------------------------------------------------------------------------------------------------- | :---------------------------------------------------------------------------------------------------------- |
| **0**        | **IDLE**       | Initial state. Immediately falls through into START with no events required; prepares the context for a new measurement cycle by initializing the data union. | State 1 (START)                                                                                             |
| **1**        | **START**      | Begins the measurement cycle. Turns on the user LED (if implemented) and initiates a **1-Wire bus reset** sequence to detect devices.     | State 2 (CONVERT)                                                                                           |
| **2**        | **CONVERT**    | Checks if a DS18B20 responded correctly to the reset. If present, sends the **Convert T (`0x44`)** command — either via **Skip ROM (0xCC)** to all devices, or via **Match ROM (0x55) + device ROM** to the device selected with `ds18b20_select()`. If not, reports an error.     | State 3 (WAIT) on success.<br/>State 0 (IDLE) after pause on error. |
| **3**        | **WAIT**       | Starts a non-blocking timer delay for the temperature conversion of the configured resolution (93.75ms @ 9-bit … 750ms @ 12-bit) to complete. The resolution is auto-derived from each valid scratchpad read and updated by `ds18b20_set_resolution()`.                       | State 4 (CONTINUE)                                                                                          |
| **4**        | **CONTINUE**   | Initiates a second **1-Wire bus reset** sequence to prepare for reading the converted data.                                              | State 5 (REQUEST)                                                                                           |
| **5**        | **REQUEST**    | Checks for the DS18B20's presence again. If present, sends the **Read Scratchpad (`0xBE`)** command — via **Skip ROM (0xCC)** or **Match ROM (0x55) + device ROM** depending on the selected device. If not, reports an error.            | State 6 (READ) on success.<br/>State 0 (IDLE) after pause on error.                                         |
| **6**        | **READ**       | Reads the **9 bytes of scratchpad data** (including CRC) from the sensor using precise pulse-width measurement via timer input capture. | State 7 (DECODE)                                                                                            |
| **7**        | **DECODE**     | **Decodes** the captured pulse widths into data bytes, validates the **CRC**, converts the raw temperature, and reports the result. Turns off the user LED. Starts a pause before the next cycle. | State 0 (IDLE) after pause.                                                                                 |

## RTOS Integration

### Bus Idle Behaviour

Verified on hardware (this driver, TIM1+DMA one-pulse bus) against the DS18B20
datasheet. Relevant if `ds18b20_search_poll()` / `ds18b20_poll()` are called
from an RTOS task and scheduling delays can land between 1-Wire slots:

- **Idle-HIGH is harmless.** The datasheet states the 1-Wire bus must be left
  in the inactive (high) state when suspending a transaction and that
  *"infinite recovery time can occur between bits so long as the bus is in the
  inactive (high) state during the recovery period."* The DS18B20 re-synchronises
  to the next falling edge; it has no internal timeout that expires during an
  idle-high pause.
- **Measured:** injecting real idle-high gaps of 10 µs … 5 ms between Search ROM
  slots finds all 5 devices in 100/100 runs at every gap size.
- **Idle-LOW > 480 µs resets all devices** (datasheet: *"if the bus is left low
  for more than 480 µs, all components on the bus will be reset"*). This is the
  only real hazard.

Practical consequences for RTOS use:

1. A delayed poll is safe **because the line is released to HIGH in hardware**,
   not by software. Every bus operation now ends with the line idle-HIGH
   automatically: the CCR3-fed writes (`send_command_n`, the merged search op)
   append a trailing 0 to the DMA feed, and the direct-write/capture operations
   (reset, read, single-slot write) use an OC3PE preload of 0 — both applied at
   the instant the one-pulse timer stops. There is no software `T1.CCR3 = 0`
   anywhere; the bus cannot be left LOW by a stale compare value, no matter how
   long the RTOS delays the next poll.
2. The usable scheduling latency budget is ~480 µs of LOW, not a tight
   microsecond window. Any RTOS that resumes the poll within hundreds of µs is
   fine; longer delays only require that the bus idles HIGH, which the hardware
   release guarantees by construction.

### Multithreading and Concurrency

The driver is safe to call from an RTOS task, but it is **not re-entrant and
not thread-safe by itself**. All driver state is global and shared: the DS18B20
state machine, the 1-Wire search context (`search_ctx`, in `onewire.c`), and
the TIM1/DMA1 peripherals. There is no internal lock. The ownership guards
(`txn_can_start`, the `ds18b20_search_*` checks) only prevent *logical*
conflicts within a single-threaded model — they are plain flag checks, not
atomic across tasks.

Rules for correct RTOS use:

1. **Confine every `ds18b20_*` call to a single task, or serialise with a
   lock.** Either drive the whole driver from one task (the same one that calls
   `ds18b20_poll()`), or wrap every entry point — each `*_start`, each
   `*_poll`, `ds18b20_select()`, `ds18b20_scan_start()`,
   `ds18b20_search_start()` — in a mutex/semaphore taken for the entire
   sequence (start + poll loop). Two tasks touching the driver at once corrupt
   the shared state machine and the TIM1/DMA registers. Example (FreeRTOS):

   ```C
   xSemaphoreTake(ow_mutex, portMAX_DELAY);
   ds18b20_set_alarm_thresholds(0x19, 0x0F);
   while (!ds18b20_set_alarm_thresholds_poll()) {
       osDelay(1);   /* the transaction owns TIM1/DMA; just yield */
   }
   xSemaphoreGive(ow_mutex);
   ```

   While a search, a resolution change or a command transaction is running,
   `ds18b20_poll()` returns immediately without doing anything, so the operation
   must be advanced by its own `*_poll()` (as in the loop above) — not by
   `ds18b20_poll()`.

2. **TIM1 and DMA1 (channels 3 and 4) are owned by the driver.** No other task
   or peripheral may use them while the driver is initialised.

3. **Poll cadence vs latency.** Because the 1-Wire bit timing is generated
   entirely by hardware, `ds18b20_poll()` may be called at any rate — slow
   polling only increases latency, never causes errors (see *Bus Idle
   Behaviour*). Two practical patterns:
   - **Dedicated polling task:** loop `ds18b20_poll(); osDelay(1);` (or
     `vTaskDelay(1)`). This gives low latency without saturating the CPU; the
     750 ms conversion wait is a hardware timer, so the task yields during it.
     If other tasks also use the driver, take the mutex around each
     `ds18b20_poll()` call (or skip the poll when the mutex is busy).
   - **Periodic timer / idle hook:** call `ds18b20_poll()` from a
     high-frequency timer callback or the RTOS idle hook.
   Avoid a tight `while (!done) poll();` busy loop — it consumes the task's
   entire timeslice.

4. **Callbacks run in task context, not in an ISR.** `ds18b20_complete()` and
   `ds18b20_busy()` are invoked synchronously from inside `ds18b20_poll()`,
   which runs in your task. You may therefore use ordinary RTOS primitives
   there — e.g. `xSemaphoreGive()` / `xTaskNotifyGive()` from
   `ds18b20_complete()` to wake a consumer task. No `...FromISR` variant is
   needed.

5. **No built-in blocking API.** The driver never blocks; there is no
   `ds18b20_read_temperature_blocking()`. If a task must sleep until a result is
   ready, start the operation (`ds18b20_scan_start()`, `ds18b20_search_start()`
   or a command transaction), then either poll in a loop that yields
   (`osDelay(1)`), or block on a semaphore that `ds18b20_complete()` releases.
   Do **not** add a TIM1 interrupt just to signal completion — polling is
   sufficient and preserves the zero-interrupt design.

6. **Preemption mid-byte is safe.** A task may be preempted while a byte is
   being transmitted: the DMA completes the whole byte in hardware and the bus
   returns idle-HIGH, so resuming later is harmless (it complements *Bus Idle
   Behaviour*). Only the *next* operation must be scheduled by a `poll()` call,
   which any task may do once it holds the lock from rule 1.

## API Reference

### Core Functions

```C
void ds18b20_init(void);
```
Initialize the DS18B20 driver. Enables peripherals (GPIOA, TIM1, DMA1) and sets up the timer prescaler for 1µs resolution. System clock configuration is handled separately in the application (see `app.c`). This function does NOT start the state machine.

```C
void ds18b20_poll(void);
```
The Core Driver Function: Must be called from the main loop. It checks the Timer Update Flag (UIF). If the flag is set, it means the hardware has finished the previous operation (e.g., sending a command, waiting for conversion). The function then clears the flag and advances the internal state machine to the next step. The driver's state is persistent, so this function can be called at any rate without risk of getting stuck.

### 1-Wire Layer (shared)

The 1-Wire bus primitives and the Search ROM engine are **not** part of the
driver — they live in the shared 1-Wire layer (`inc/onewire.h` +
`src/onewire.c`) that `src/ds18b20.c` is built on. Full interface:

```C
void        onewire_init(void);
uint8_t     onewire_bus_done(void);
void        onewire_reset(volatile uint16_t *edge_out);
uint8_t     onewire_present(const volatile uint16_t *edge);
void        onewire_write_slots(const uint8_t *pulses, uint16_t slots);
void        onewire_write_bit(uint8_t bit);
void        onewire_encode_byte(uint8_t *out, uint8_t byte);
void        onewire_read_pair(volatile uint16_t *edge_out);
void        onewire_write_then_read(uint8_t bit);
void        onewire_pair_bits(const volatile uint16_t *edge,
                              uint8_t *id_bit, uint8_t *cmp_bit);
void        onewire_read_data(volatile uint8_t *dst, uint8_t bytes);
void        onewire_decode_pulses(uint8_t *dst, const volatile uint8_t *pulse,
                                 uint8_t nbytes);
void        onewire_start_timer(uint16_t arr, uint8_t rcr);
uint8_t     onewire_crc8(const uint8_t *data, uint8_t len);
void        onewire_search_start(onewire_search_sink_t sink,
                                 uint8_t max_devices, uint8_t command,
                                 uint8_t family);
uint8_t     onewire_search_poll(void);
uint8_t     onewire_search_count(void);
uint8_t     onewire_search_active(void);
```

#### Timing Profiles

The slot timing is selected at **runtime** from four built-in profiles
(`inc/onewire.h`); `ONEWIRE_TIMING_PROFILE_DEFAULT` chooses the compile-time
default (`ONEWIRE_TIMING_STANDARD`). Switching profiles is non-blocking and
applies immediately to every subsequent bus operation, including the Search ROM
engine. See [Configuration → Timing Profiles](#timing-profiles) for the full
per-profile value table.

```C
typedef enum {
    ONEWIRE_TIMING_FAST = 0,
    ONEWIRE_TIMING_STANDARD,
    ONEWIRE_TIMING_SLOW,
    ONEWIRE_TIMING_ROBUST,
    ONEWIRE_TIMING_COUNT
} onewire_timing_profile_t;

void onewire_set_timing_profile(onewire_timing_profile_t profile);
onewire_timing_profile_t onewire_get_timing_profile(void);
void ow_set_parasite_guard(uint8_t parasite);   /* 0 = standard guard, !=0 = parasite guard */
void onewire_strong_pullup(uint8_t on);          /* parasite power: drive bus HIGH */
```

The macro `ONEWIRE_TIMING_PROFILE_DEFAULT` (short alias `OW_TIMING_DEFAULT`) chooses
the compile-time default; the Makefile sugar `TIMING=SLOW` (or `FAST`/`STANDARD`/`ROBUST`)
is the short form — e.g. `make TIMING=SLOW` instead of the railway-station
`EXT="-DONEWIRE_TIMING_PROFILE_DEFAULT=ONEWIRE_TIMING_SLOW"`. The live values are
also exposed as the runtime globals `ow_one_pulse_us`, `ow_zero_pulse_us`,
`ow_guard_band_us` and `ow_short_pulse_max_us`.

Any other 1-Wire slave driver can use the same layer. The DS18B20 driver calls
`onewire_init()` from `ds18b20_init()` and keeps the layer's Search ROM engine
for its own `ds18b20_search_*` / `ds18b20_alarm_search_*` wrappers.

### CRC Utility

```C
uint8_t ds18b20_crc8(const uint8_t *data, uint8_t len);
```
Calculates the Dallas/Maxim CRC-8 used by the driver to validate ROM codes and
scratchpad data. Exposed publicly as a small utility (e.g., for host tools).

### Device Search

```C
void ds18b20_search_start(ds18b20_search_sink_t sink, uint8_t max_devices);
uint8_t ds18b20_search_poll(void);
uint8_t ds18b20_search_count(void);
```
Non-blocking Maxim Search ROM (0xF0) over the whole bus, implemented as a
compact state machine that performs exactly one hardware operation per
`ds18b20_search_poll()` call. `sink` is invoked once per found DS18B20 device
with its 8-byte ROM address; `max_devices` caps the reported count. Poll
`ds18b20_search_poll()` from the main loop until it returns 1 — it restores
`ds18b20_poll()` state automatically. `ds18b20_search_count()` returns how many
devices were found. Only devices with family code `DS18B20_FAMILY_CODE` (0x28)
are reported. See `demo2.c`.

### Alarm Search

```C
void ds18b20_alarm_search_start(ds18b20_search_sink_t sink, uint8_t max_devices);
uint8_t ds18b20_alarm_search_poll(void);
uint8_t ds18b20_alarm_search_count(void);
```
Non-blocking Maxim Alarm Search ROM (0xEC): reports only the DS18B20 devices
currently in alarm state, i.e. whose last measured temperature is outside the
TH/TL thresholds configured with Write Scratchpad (0x4E). It shares the device
search engine, so the callback, `max_devices` cap, family filter and ownership
rules are identical; only the command byte and the reported set differ. Unlike
`ds18b20_search_start()`, the alarm search never repopulates the scan-mode
device table (`ds18b20_device_count()` / `ds18b20_device_rom()` keep the
addresses from the last device search). Poll `ds18b20_alarm_search_poll()` from
the main loop until it returns 1, then read `ds18b20_alarm_search_count()`.

### Per-Device Addressing

```C
void ds18b20_select(const uint8_t *rom);
```
Selects which DS18B20 device the non-blocking measurement path targets, using
its 64-bit ROM address (LSB first, e.g. from a bus search). With a device
selected, the driver sends the **Match ROM (0x55)** command plus the device ROM
before every Convert T / Read Scratchpad operation, so only that device
responds. Pass `NULL` to clear the selection and return to the legacy **Skip
ROM (0xCC)** single-sensor behaviour.

`ds18b20_select()` is only accepted while the measurement state machine is
**IDLE** — calls made while a cycle is running, during a device/alarm search,
a resolution change or another command transaction are ignored. In particular
it is **rejected from the per-device scan callback** (`ds18b20_complete()` in
scan mode): that callback runs at decode time mid-round, so a select there is
ignored and the scan round continues. To switch out of scan mode, call
`ds18b20_select()` from the main loop after the scan completes, then
`ds18b20_scan_start()` to resume simultaneous conversion.

The demo measures the single device directly when exactly one is found, and
cycles through all found devices in turn when several are present.

### Command Transactions

The remaining DS18B20 commands run with the same non-blocking discipline as the
device search and the resolution change: each transaction owns TIM1/DMA while
it runs (reset → presence → write → read | timed wait) and hands the timer
back to `ds18b20_poll()` when it finishes.

```C
void     ds18b20_read_rom(uint8_t *rom);
uint8_t  ds18b20_read_rom_poll(void);
void     ds18b20_set_alarm_thresholds(uint8_t th, uint8_t tl);
uint8_t  ds18b20_set_alarm_thresholds_poll(void);
void     ds18b20_read_scratchpad(uint8_t *buf);
uint8_t  ds18b20_read_scratchpad_poll(void);
void     ds18b20_copy_scratchpad(void);
uint8_t  ds18b20_copy_scratchpad_poll(void);
void     ds18b20_recall_eeprom(void);
uint8_t  ds18b20_recall_eeprom_poll(void);
void     ds18b20_set_parasite(uint8_t parasite);
void     ds18b20_detect_parasite(void);
uint8_t  ds18b20_detect_parasite_poll(void);
uint8_t  ds18b20_parasite_mode(void);
uint8_t  ds18b20_last_command_ok(void);
```

- Every command is a `start`/`poll` pair: call the start function, then poll
  the matching `*_poll()` from the main loop until it returns 1, then resume
  `ds18b20_poll()`. Commands are ignored mid-cycle, while a device search, an
  alarm search or a resolution change owns the timer, or while another command
  transaction is still running. Result buffers must stay valid until the
  transaction finishes.
- With a device selected via `ds18b20_select()`, the command is preceded by
  Match ROM (0x55) + device ROM so only that device responds; without a
  selection it broadcasts via Skip ROM (0xCC). `ds18b20_read_rom()` is always
  sent bare (0x33) — valid only when exactly one device is on the bus.
- `ds18b20_read_scratchpad()` returns the raw 9 scratchpad bytes; verify with
  `ds18b20_last_command_ok()` or the CRC byte (`buf[8] ==
  ds18b20_crc8(buf, 8)`). A valid read also updates the auto-derived
  resolution (`ds18b20_get_resolution()`).
- `ds18b20_set_alarm_thresholds(th, tl)` writes TH/TL into the volatile
  scratchpad with Write Scratchpad (0x4E), keeping the current resolution in
  the config byte. The DS18B20 8-bit sign-extended temperature code is used
  (e.g. 0x19 = +25°C, 0x0F = +15°C).
- `ds18b20_copy_scratchpad()` persists TH/TL/CFG to the EEPROM and
   `ds18b20_recall_eeprom()` loads the EEPROM copy back into the scratchpad.
   Both wait the datasheet hold-off (10 ms) with the timer before finishing.
   Recall is a write-only command: it does not return the restored config, so
   the driver's tracked `ctx.resolution` is **not** updated. If the EEPROM
   resolution may differ from the current one, follow `ds18b20_recall_eeprom()`
   with `ds18b20_read_scratchpad()` to resynchronise `ds18b20_get_resolution()`
   before the next conversion (see `demo4.c`, which chains recall → scratchpad
   read for this reason).
- `ds18b20_detect_parasite()` runs a Read Power Supply query and stores the
   answer straight into the driver state — after
  `ds18b20_detect_parasite_poll()` returns 1 (check `ds18b20_last_command_ok()`)
  the wiring is configured and `ds18b20_parasite_mode()` reports it. On a mixed
  bus the open-drain answer is a wired-AND: any externally powered device masks
  the parasite report, so detect per-device in Match ROM addressing mode for
  heterogeneous wiring.
- `ds18b20_set_parasite(1)` enables parasite-power support: the driver then
  engages the strong pull-up (bus pin switched to push-pull HIGH) for every
  conversion window (t_CONV) and EEPROM hold-off (t_COPY / t_RECALL), and
  releases the line before any further bus activity. Default is 0 — external
  VDD wiring, no pin mode changes. Applying the strong pull-up is harmless
  for externally powered devices, so a mixed bus works with the flag set.
- `ds18b20_last_command_ok()` reports whether the last transaction found a
  device present (and, for read commands, read its data back).

Example — set TH/TL and persist them to the EEPROM:

```C
ds18b20_set_alarm_thresholds(0x19, 0x0F);   // +25°C / +15°C
while (!ds18b20_set_alarm_thresholds_poll()) {
    /* keep calling from the main loop */
}
ds18b20_copy_scratchpad();                  // persist to the EEPROM
while (!ds18b20_copy_scratchpad_poll()) {
    /* keep calling from the main loop */
}
```

### Simultaneous Multi-Device Conversion

```C
void ds18b20_scan_start(void);
uint8_t ds18b20_device_count(void);
const uint8_t* ds18b20_device_rom(uint8_t index);
uint8_t ds18b20_scan_index(void);
```

Convert every discovered device in parallel. `ds18b20_scan_start()` schedules one
broadcast `Convert T` (Skip ROM 0xCC) so all sensors convert simultaneously, then
reads each one back via Match ROM in device-table order, reporting every result
through `ds18b20_complete()`. N devices take one conversion wait plus N reads
instead of N conversion waits. A missing device reports
`DS18B20_TEMP_ERROR_NO_SENSOR` and the scan continues. See `demo3.c`.

- The device table must be populated first by the non-blocking device search
  (`ds18b20_search_*`).
- Scan mode assumes a single resolution across all sensors (the config is written
  broadcast) and is mutually exclusive with the single-device
  `ds18b20_select()` addressing — calling `ds18b20_select()` clears scan mode,
  call `ds18b20_scan_start()` again to resume.
- `ds18b20_device_count()` returns how many DS18B20 devices are stored;
  `ds18b20_device_rom(index)` returns the 8-byte ROM (LSB first) of one of them,
  or NULL for an out-of-range index (the pointer is valid until the next search).
- `ds18b20_scan_index()` returns the index of the device whose result
  `ds18b20_complete()` just reported (valid during scan mode).

Example:

```C
ds18b20_scan_start();   // begin simultaneous conversion of all sensors
while (1) {
    ds18b20_poll();     // scan reports each device via ds18b20_complete()
}

// inside ds18b20_complete(): identify the sensor
void ds18b20_complete(int16_t temp) {
    uint8_t idx = ds18b20_scan_index();
    printf("Sensor %u: %d.%d C\n", idx, temp / 10, abs(temp % 10));
}
```

### Parasite Power

With VDD tied to GND the DS18B20 draws its operating current from the data
line. The bus pull-up then has to deliver the conversion current — about
**1.5mA per converting sensor** — for the whole conversion window (up to
750ms at 12-bit), which a passive resistor cannot do. The driver solves this
by switching the bus pin to push-pull HIGH for every conversion and EEPROM
hold-off window (`ds18b20_set_parasite(1)`), releasing it back to the passive
pull-up before any further bus activity.

```C
ds18b20_init();
ds18b20_detect_parasite();          // query the wiring (0xCC + Read Power Supply)
while (!ds18b20_detect_parasite_poll()) {
    ds18b20_poll();                 // keep advancing the state machine
}
/* ds18b20_parasite_mode() now reflects the detected wiring */
```

Wiring guidance, from bench validation on an STM32F103 with a 2.2k pull-up:

- A handful of sensors on short wires (<30cm) converts reliably on the MCU pin
  alone: a six-device broadcast cycle completed without a single CRC error.
- Budget ~1.5mA per simultaneously converting sensor against the pin's drive
  capability (~25mA source on F1/F0/G0) and the VOH droop across your pull-up
  arrangement; keep the bus HIGH above the DS18B20's ~2.96V minimum.
- For longer buses or larger fleets add an external P-MOSFET (or a dedicated
  strong pull-up IC) as the high-side switch and treat the MCU pin as its gate
  driver — the software interface stays exactly the same.

The example applications accept a compile-time flag to run over parasite
wiring out of the box:

```sh
make APP=demo EXT=-DPARASITE_POWER=1        # or demo2 / demo3 / demo4 / demo5
```

### Signal Statistics Module (`ow_stats`)

An optional compile-in module that collects per-sensor pulse-width statistics
and a global histogram across measurement cycles.  Enabled by defining
`OW_STATS_ENABLE` at build time.  When the macro is not defined, every inline
body compiles away to nothing — zero overhead in production builds.

```C
#include "ow_stats.h"

void ow_stats_init(void);
void ow_stats_capture_pulse(const volatile uint8_t *pulse, uint8_t n,
                            const uint8_t *rom);
void ow_stats_count_error(int16_t error, const uint8_t *rom);
void ow_stats_dump_start(void);
uint8_t ow_stats_dump_poll(void);
void ow_stats_reset(void);
uint32_t ow_stats_tick(void);
```

- `ow_stats_init()` — zero-initialise the statistics context.  Call once at
  startup.
- `ow_stats_capture_pulse()` — snapshot raw pulse widths before
  `decode_scratchpad()` overwrites the capture buffer via the union alias.
  Updates the 13-bucket logarithmic histogram (0–60+ µs; 13 of the 16
  `OW_STATS_HIST_BUCKETS` array slots are populated, indices 0–12) covering
  0–2, 3–4, 5–6, 7–9, 10–12, 13–14, 15–19, 20–24, 25–29, 30–39,
  40–49, 50–59, 60+ µs) and per-sensor min/max pulse counters.  Called
  automatically from `ds18b20.c` when `OW_STATS_ENABLE` is defined.
- `ow_stats_count_error()` — record a CRC mismatch, missing presence pulse
  or other error event.  Called automatically from `ds18b20.c`.
- `ow_stats_dump_start()` — begin a non-blocking UART dump.  Call from the
  main loop after the desired number of cycles (tracked via `ow_stats_tick()`).
- `ow_stats_dump_poll()` — advance the dump by one line (header, sensor line,
  histogram or total).  Each call blocks only for the UART TX register to
  accept one byte (~87 µs at 115200 baud), writing directly to TDR and
  bypassing the ring buffer entirely.  A 6-sensor report completes in ~22 ms.
  Returns 1 when the dump is complete.
- `ow_stats_reset()` — zero all counters and the histogram, keep the sensor
  ROM table.  Call after `ow_stats_dump_poll()` returns 1.
- `ow_stats_tick()` — increment the cycle counter; returns the new value.

RAM cost: ~290 bytes (8 sensors × 26 B + 16-entry `uint32_t` histogram [64 B] +
cycle/error counters; 13 of the 16 histogram buckets, indices 0–12, are
populated).

Example — dump every 100 cycles:

```C
#include "ow_stats.h"

static uint8_t dump_busy = 0;

void ds18b20_complete(int16_t temp) {
    // ... handle temperature reading ...
    uint32_t cycles = ow_stats_tick();
    if (cycles >= 100 && !dump_busy) {
        ow_stats_dump_start();
        dump_busy = 1;
    }
}

int main(void) {
    ow_stats_init();
    // ... ds18b20_init(), device search ...
    for (;;) {
        if (dump_busy) {
            if (ow_stats_dump_poll()) {
                dump_busy = 0;
                ow_stats_reset();
            }
        } else {
            ds18b20_poll();
        }
    }
}
```

Build with the statistics module:

```sh
make APP=demo5                                  # external power (OW_STATS_ENABLE auto-added)
make APP=demo5 EXT="-DPARASITE_POWER=1"            # parasite power
```

> Note: the `demo5` target already injects `-DOW_STATS_ENABLE` plus
> `-DSTATS_DUMP_INTERVAL=5000 -DDS18B20_CYCLE_PAUSE_US=10000`, so the shipped
> demo5 dumps every 5000 cycles with a 10 s inter-cycle pause. Override either
> macro via `EXT=` if you want the module defaults instead.

UART output format (compact, one sensor per line):

```
--- stats [100 c] ---
28 20 78 92 07 00 00 67:5-29 n17 e0
28 78 B8 AC 0B 00 00 2C:5-31 n17 e0
28 64 69 AB 0B 00 00 1F:5-29 n17 e0
28 FC AE AA 0B 00 00 F3:5-30 n17 e0
28 7E 63 AD 0B 00 00 3F:5-30 n16 e0
28 F1 39 AD 0B 00 00 D9:5-30 n16 e0
h:2=3304 8=2023 9=1873
t=100c 0e
```

Fields per sensor line: `ROM:min-max n=count e=errors` (errors = CRC + no
presence + other combined).  Histogram shows only non-empty buckets;
`h:B=count` where B is the bucket index.  Total line: `t=Nc Ee` where N =
cycle count, E = total error count.

### Resolution Change

```C
void ds18b20_set_resolution(uint8_t bits);
uint8_t ds18b20_set_resolution_poll(void);
uint8_t ds18b20_get_resolution(void);
```

Change the temperature conversion resolution between measurement cycles,
non-blocking and without interrupts, mirroring the device search state machine:

- `bits` is the new resolution in bits — `DS18B20_RES_MIN` (9) …
  `DS18B20_RES_MAX` (12). Out-of-range values are ignored. The change is only
  accepted while the measurement state machine is IDLE and no device search is
  running; it is ignored otherwise.
- The configuration is written to the volatile scratchpad with **Write
  Scratchpad (0x4E)** (TH/TL are reset to 0, disabling the alarm triggers) and
  takes effect immediately; it is **not** persisted to the EEPROM (Copy
  Scratchpad would need a strong pull-up under parasitic power).
- Poll `ds18b20_set_resolution_poll()` from the main loop until it returns 1,
  then resume `ds18b20_poll()`. The next measurement waits exactly as long as
  the new resolution requires (e.g. 93.75ms at 9-bit instead of 750ms).
- `ds18b20_get_resolution()` returns the current resolution. It is updated by a
  successful resolution change and auto-derived from every valid scratchpad
  read (byte 4, R1/R0), so it also tracks a resolution changed externally.

Example — drop to 9-bit to measure 8× faster:

```C
ds18b20_set_resolution(9);
while (!ds18b20_set_resolution_poll()) {
    /* keep calling from the main loop; never blocks */
}
/* ds18b20_get_resolution() == 9; ds18b20_poll() resumes with the fast wait */
```

### Weak Callbacks

```C
void ds18b20_busy(unsigned action);
```
Called to indicate busy/idle status — toggle an LED, for example. `action` is non-zero for busy (measurement in progress), 0 for idle.

```C
void ds18b20_complete(int16_t temp);
```
Called when a measurement cycle completes — provides temperature data in tenths of degrees Celsius, or an error code (`DS18B20_TEMP_ERROR_*`).

### Error Codes

- DS18B20_TEMP_ERROR_NO_SENSOR: No sensor detected on the bus.
- DS18B20_TEMP_ERROR_CRC_FAIL: Data corruption detected via CRC mismatch.
- DS18B20_TEMP_ERROR_GENERIC: Unspecified communication error.

## Performance

- Time to result (one measurement): 93.75ms @ 9-bit … ~0.76 s @ 12-bit
  (conversion + protocol overhead; the conversion wait follows the configured
  resolution, see `ds18b20_set_resolution()`)
- Inter-measurement pause: 5 s, configurable via `DS18B20_CYCLE_PAUSE_US` (default 5000000 µs; the demo5 build overrides it to 10000 µs)
- Precision: 0.1°C resolution at 12-bit (coarser steps at lower resolutions)
- Accuracy: ±0.5°C (typical)
- CPU Usage: Minimal; CPU is free to perform other tasks during waits.

## Configuration

### Timing Profiles

The 1-Wire slot timing is selected at runtime from four built-in profiles
(see also the [API Reference → Timing Profiles](#timing-profiles)). The default
profile is `ONEWIRE_TIMING_PROFILE_DEFAULT` (short alias `OW_TIMING_DEFAULT`,
`ONEWIRE_TIMING_STANDARD`); in the Makefile use `TIMING=SLOW` (`FAST`/`STANDARD`/
`SLOW`/`ROBUST`) instead of the long `EXT="-DONEWIRE_TIMING_PROFILE_DEFAULT=..."`.
The raw `-D` still works for direct compiler invocations.

The fixed `ONEWIRE_*` macros in `inc/onewire.h` are the **STANDARD** profile
values (and the initial runtime defaults); the reset-pulse bounds are defined in
`src/onewire.c`:

```C
/* inc/onewire.h — STANDARD profile defaults */
#define ONEWIRE_ONE_PULSE           5     // µs (short low = write-1)
#define ONEWIRE_ZERO_PULSE         60    // µs (long low = write-0)
#define ONEWIRE_GUARD_BAND          5     // µs (built into slot formula)
#define ONEWIRE_SHORT_PULSE_MAX    10    // µs (pulse <= this reads as bit '1')

/* src/onewire.c */
#define RESET_PULSE_MIN           480U    // µs
#define RESET_PULSE_MAX           540U    // µs
```

Slot formula (uniform across all profiles):

```
ARR = one_pulse + zero_pulse + guard_band
```

| Profile    | `one` | `zero` | `guard` | `parasite guard` | `short≤` | Slot  |
|------------|------:|-------:|--------:|-----------------:|---------:|------:|
| FAST       | 5µs  | 60µs  | 3µs    | 50µs            | 10µs    | 68µs |
| STANDARD   | 5µs  | 60µs  | 5µs    | 100µs           | 10µs    | 70µs |
| SLOW       | 8µs  | 90µs  | 20µs   | 200µs           | 15µs    | 118µs|
| ROBUST     | 10µs | 110µs | 30µs   | 250µs           | 18µs    | 150µs|

`parasite guard` is used in place of `guard` when parasite power is engaged
(via `ow_set_parasite_guard()`); it widens the window so the strong-pullup
release margin does not clip the sample. SLOW / ROBUST trade conversion
throughput for timing margin and are intended for long wiring, parasite buses
or electrically noisy setups.

## Troubleshooting

### Common Issues

1. "No sensor detected" or "CRC check failed" errors
   - Cause: The most common cause is electrical. The presence pulse captured by the DMA/timer did not meet the timing criteria, or noise corrupted the data during the 72-bit read.
   - Fix:
     - Check all wiring connections.
      - Ensure a 4.7kΩ pull-up resistor is between the 1-Wire data line
        (PA10; logical PA10 via PA12 remap on G0) and 3.3V.
     - Verify stable power is supplied to the DS18B20 sensor.
     - Keep data lines short to minimize noise and capacitance.

2. Temperature readings are infrequent
   - Cause: The ds18b20_poll() function is called slowly from the main loop. The driver operates correctly but advances through its states (e.g., the 750ms conversion wait) at a slower pace.
   - Fix: This is often not a problem if a slow update rate is acceptable. If faster updates are needed, ensure the main loop runs frequently and avoids other blocking code. The driver itself is non-blocking and will not cause this slowdown.

### Debugging Tips

- Use Debug Build: The release build (`-Os -flto -g0`) aggressively optimizes
  the driver, which may inline or eliminate static variables like `ctx`.
  Use `make debug` (`-Og -g3 -gdwarf`) for debugging.
- VSCode: Press F5 to build (debug) and launch a J-Link debug session.
  The SVD file provides peripheral register views.
- Monitor the State Variable: Check `ctx.current_state` in a debugger to
  see the current step in the communication sequence.
- Check the Update Flag: Read `TIM1->SR`. If the driver seems idle,
  a set UIF bit indicates a completed operation waiting to be processed
  by `ds18b20_poll()`.
- Inspect the GPIO: Use an oscilloscope on the data pin (PA10) to verify the 1-Wire
  waveforms. Look for:
  - A clean ~480µs reset pulse (MCU pulls low, then releases).
  - A presence pulse ~60-240µs after the reset pulse (sensor pulls low).
  - Precise write slots: a short ~5µs low for a '1', a long ~60µs low
    for a '0' (slot = 5 + 60 + 5 = 70µs).
- Inspect Captured Data: Examine the driver context's `ctx.edge[]` after a reset
   or `ctx.pulse[]` after a read (in `src/ds18b20.c`) to see the raw timing
   data.

## License

This project is released under the MIT License. See the LICENSE file for details.

## Contributing

1. Fork the repository
2. Create your feature branch (git checkout -b feature/AmazingFeature)
3. Commit your changes (git commit -m 'Add some AmazingFeature')
4. Push to the branch (git push origin feature/AmazingFeature)
5. Open a Pull Request

## Support

For issues and questions, please open an issue on GitHub.

## References

- DS18B20 Datasheet  
  https://datasheets.maximintegrated.com/en/ds/DS18B20.pdf

- STM32F103 Reference Manual  
  https://www.st.com/resource/en/reference_manual/cd00171190-stm32f101xx-stm32f102xx-stm32f103xx-advanced-arm-based-32-bit-mcus-stmicroelectronics.pdf

- 1-Wire Protocol Specification  
  https://www.maximintegrated.com/en/design/technical-documents/tutorials/1/1796.html
