[![STM32 Build CI](https://github.com/a5021/stm32-async-1wire/actions/workflows/build.yml/badge.svg)](https://github.com/a5021/stm32-async-1wire/actions/workflows/build.yml)
[![Code Quality](https://github.com/a5021/stm32-async-1wire/actions/workflows/ci.yml/badge.svg)](https://github.com/a5021/stm32-async-1wire/actions/workflows/ci.yml)
[![Coverage](https://raw.githubusercontent.com/a5021/stm32-async-1wire/gh-pages/coverage-badge.svg)](https://a5021.github.io/stm32-async-1wire/) [![Awesome](https://awesome.re/badge.svg)](https://github.com/iDoka/awesome-embedded-software#sensors)
[![License: MIT](https://img.shields.io/badge/License-MIT-yellow.svg)](https://opensource.org/licenses/MIT)
# stm32-async-1wire

Non-blocking 1-Wire master for STM32, with a DS18B20 temperature driver built on top. A generic bus layer (`src/onewire.c`) owns the 1-Wire timing — a hybrid of a hardware timer (TIM1) and DMA automates every slot, so the CPU never performs timing-critical busy-waits inside a transaction and never enters an interrupt; operations advance by polling hardware completion flags. The first driver on that layer is `src/ds18b20.c`, and other 1-Wire slaves (DS2413, DS2431, ...) can ride it as-is.

The core (`src/onewire.c` + `src/ds18b20.c`) is MCU-independent and rides on a small port interface (`inc/ow_port.h`); per-MCU backends are header-only implementations under `port/`. Six backends ship today:

- `port/stm32f1/ow_port_f1.h` — STM32F103C8T6 (Blue Pill): bus on PA10, TIM1 CH3 output / CH4 capture, DMA1 channels 3/4.
- `port/stm32f0/ow_port_f0.h` — STM32F030x6 (e.g. TSSOP20 STM32F030F4P6): bus on PA10, TIM1 CH3 output / CH4 capture, DMA1 channels 3/4.
- `port/stm32f3/ow_port_f3.h` — STM32F303VC (F3-DISCOVERY / MB1035B): bus on PA10 (AF6), TIM1 CH3 output / CH4 capture, DMA1 channels 3/4 — the same fixed pair as F1/F0 (RM0316 Table 78; channel 4 also carries USART1_TX and channel 3 USART3_TX, harmless because no UART byte moves by DMA).
- `port/stm32g0/ow_port_g0.h` — STM32G031x6 (e.g. TSSOP20 STM32G031F6P6): bus on PA10 via the SYSCFG PA12 remap, TIM1 CH3 output / CH4 capture, DMA1 channels 3/4 through DMAMUX (requests 21/23).
- `port/stm32g4/ow_port_g4.h` — STM32G474CB (WeAct STM32G474CBT6 Long): bus on PA10 (AF6), TIM1 CH3 output / CH4 capture, DMA1 channels 3/4 through DMAMUX (requests 43/45), 8-bit feed tables. HSE+PLL to 170MHz (M=2, N=85, R=2, Range 1 Boost) or raw HSI16 at 16MHz. USART1 TX on PA9 (AF7).
- `port/stm32f4/ow_port_f4.h` — STM32F407VGT6 (STM32F4DISCOVERY), STM32F401CC (e.g. WeAct F401 Black Pill) and STM32F446RE (e.g. WeAct F446RET6): bus on PA10, TIM1 CH3 output / CH4 capture, DMA2 streams 2/4 (feed 16-bit, direct mode). Same header for all three parts — the chip selects the CMSIS device layer, linker script and clock default. The F446 additionally runs 180MHz, which needs the PWR over-drive sequence (see Clocking invariant below).

## Table of Contents

- [Features](#features)
- [Requirements](#requirements)
- [File Structure](#file-structure)
- [Examples](#examples)
- [Hardware Verified](#hardware-verified)
- [Hardware Connections](#hardware-connections)
- [Quick Start](#quick-start)
- [Building](#building)
- [VSCode Integration](#vscode-integration)
- [Comparison with Common 1-Wire Techniques](#comparison-with-common-1-wire-techniques)
- [Architecture](#architecture)
- [RTOS Integration](#rtos-integration)
- [API Reference](#api-reference)
- [Performance](#performance)
- [Configuration](#configuration)
- [Troubleshooting](#troubleshooting)
- [License](#license)
- [Contributing](#contributing)
- [Support](#support)
- [References](#references)

## Features

- Pure Bare-Metal: direct register manipulation, no HAL or LL libraries.
- Universal 1-Wire Layer: `inc/onewire.h` + `src/onewire.c` — a reusable,
  non-blocking 1-Wire master (bus primitives + Maxim Search ROM) scheduled on
  TIM1/DMA; the DS18B20 driver is built on it, and other 1-Wire slaves
  (DS2413, DS2431, ...) can reuse it as-is — see
  [1-Wire Layer (shared)](#1-wire-layer-shared).
  - Multi-MCU Backend: one MCU-independent core over a `ow_port_*` interface;
    header-only backends for STM32F1, STM32F0, STM32F3, STM32G0, STM32F4 and
    STM32G4, all on the shared CH3/CH4 scheme. Select at build time with
    `make OW_TARGET=f0` / `make OW_TARGET=f3` / `make OW_TARGET=g0` /
    `make OW_TARGET=f4` / `make OW_TARGET=g4` (F1 is the default).
- Zero NVIC Interrupts: no NVIC interrupts and no ISRs — fully polled
  operation. The optional `-DOW_PORT_LOW_POWER=1` mode enables the timer
  update **interrupt source** (UIE) only to generate a pending event that
  wakes `WFE()` via `SEVONPEND` — no NVIC interrupt is enabled and no ISR is
  installed.
- RTOS-Ready: the strict 1-Wire bit timing is generated entirely by TIM1+DMA,
  so `ds18b20_poll()` can be called at any rate from an RTOS task without
  corrupting the bus. Not thread-safe by itself — see
  [RTOS Integration](#rtos-integration).
- Hardware Automation: TIM1 output compare + input capture with DMA automate
  waveform generation and data capture — see [Architecture](#architecture).
- State Machine Architecture: event-driven operation controlled by hardware
  completion signals — see
  [State Machine Flow](#state-machine-flow-hardware-timed-polled-on-uif).
- Weak Function Callbacks (`ds18b20_busy()`, `ds18b20_complete()`) and CRC-8
  validation of every sensor reading — see [Weak Callbacks](#weak-callbacks)
  and [Error Codes](#error-codes).
- Resolution-Aware Conversion Wait: the driver waits exactly as long as the
  configured conversion resolution requires (93.75ms @ 9-bit … 750ms @
  12-bit), so lowering the resolution speeds up the measurement cycle.
- Non-blocking across the board — each operation advances one hardware
  operation per `*_poll()` call from the main loop:
  - Device search: `ds18b20_search_start()` / `_poll()` / `_count()` —
    [Device Search](#device-search).
  - Alarm search: `ds18b20_alarm_search_*()` reports only devices in alarm —
    [Alarm Search](#alarm-search).
  - Command transactions: Read ROM, TH/TL, scratchpad, Copy/Recall EEPROM,
    parasite detect — [Command Transactions](#command-transactions),
    `examples/5_commands`.
  - Per-device addressing: `ds18b20_select()` (Match ROM 0x55) —
    [Per-Device Addressing](#per-device-addressing).
  - Resolution change: `ds18b20_set_resolution()` / `_poll()` —
    [Resolution Change](#resolution-change).
  - Simultaneous multi-device conversion: one broadcast `Convert T`, read
    back via Match ROM —
    [Simultaneous Multi-Device Conversion](#simultaneous-multi-device-conversion).
- Optional Signal Statistics Module (`ow_stats`, `-DOW_STATS_ENABLE=1`):
  per-sensor pulse-width min/max, a global histogram, error counters and a
  non-blocking UART dump; all stubs inline to nothing in production builds —
  see [Signal Statistics Module](#signal-statistics-module-ow_stats).

## Requirements

- Microcontroller: any STM32 with a single advanced-control timer instance
  that satisfies the complete [Required Timer Capabilities](#required-timer-capabilities)
  and DMA topology (currently supported: STM32F103C8T6, STM32F030x6,
  STM32F303VC, STM32G031x6, STM32F407VGT6, STM32F401CC, STM32F401xE,
  STM32F446RE, STM32G474CB; see port backends in `port/`).
- Sensor: DS18B20 digital temperature sensor
- Toolchain: GCC ARM (arm-none-eabi)
- Clock Configuration: STM32F103 — 72MHz via HSE+PLL (default) or 8MHz via internal RC (`make SYSCLK_MHZ=8`); STM32F030 — 48MHz via HSI+PLL (default) or 8MHz via internal RC. STM32G031 — 64MHz via HSI16+PLL (default) or 16MHz via raw HSI16. STM32F303VC — 72MHz via HSE-bypass + PLL (default; the ST-LINK drives the board's 8MHz crystal onto OSC_IN), 64MHz via HSI/2 + PLL (this family has no HSI16 and PLLMUL tops out at 16, so 64MHz is all the internal RC can reach), or 8MHz on the raw HSI. STM32F407 — 168MHz via HSE+PLL (default), 16MHz via internal RC (`SYSCLK_MHZ=16`) or the crystal's own frequency (`SYSCLK_MHZ=<HSE_MHZ>`). STM32F401 — 84MHz via HSE+PLL (default, `OW_CHIP=f401xc`), 16MHz via internal RC, or the crystal's own frequency. STM32F446 — 180MHz via HSE+PLL with the over-drive sequence (default, `OW_CHIP=f446xx`), plus the same 16MHz / crystal-frequency options. STM32G474 — 170MHz via HSE+PLL with Range 1 Boost (default, `OW_CHIP=g474cb`), or 16MHz via raw HSI16. On F4 the crystal is a separate knob, `HSE_MHZ=N`, because it is the *board's* property while `SYSCLK_MHZ` is the application's: the PLL takes its M divider from it, so a 25MHz board reaches the F401's 84MHz cap with M=25/N=168, an 8MHz board the F407's 168MHz with M=8/N=336 and the F446's 180MHz with M=8/N=360. Part files carry a default for the board they are named after — `chips/f401xc.mk` says 25MHz for the WeAct F401 Black Pill, `chips/f401xe.mk` says 8MHz — and `HSE_MHZ=N` overrides both. The default is 8MHz everywhere else. A wrong value is not a compile error: the PLL simply never locks, so the HSE, PLL and (on the F446) over-drive waits are bounded and the application reports the failure over the console and stops rather than running on a clock its timings were not compiled for.

  The 180MHz mode is the one clock on F4 that is above 168MHz, and it is a
  different code path rather than a different number: it needs the PWR over-drive
  sequence (`ODEN`→`ODRDY`, `ODSWEN`→`ODSWRDY`), its own flash-latency and APB
  values, and PLLN=360. Only the F446 offers it, so a build that asks for 180MHz
  on any other F4 part is rejected at compile time rather than quietly run out of
  spec — each part's ceiling is `OW_PORT_F4_MAX_SYSCLK_MHZ` in
  `port/stm32f4/ow_port_f4.h`.
  APB2 is `/2` at 180MHz as it is at 168, so the timer clock still doubles to
  2 × 90 = 180MHz and the 1µs-tick invariant (`PSC = SYSCLK_MHZ - 1`) is
  unchanged.

## File Structure

```
├── inc/                    # Project header files
│   ├── ds18b20.h           # Driver interface (high-level API) and constants
│   ├── onewire.h           # Shared 1-Wire layer (bus primitives + Search ROM)
│   ├── ow_config.h         # Compile-time tunables (pulse widths, feature flags, limits)
│   ├── ow_stats.h          # Optional signal statistics module (histogram, per-sensor)
│   ├── ow_port.h           # 1-Wire port layer interface (+ backend select)
│   └── ow_bits.h           # STM32 register access macros (shared)
├── port/                   # Per-MCU backends for the ow_port_* interface
│   ├── common/            # TIM1+DMA1 core shared by all six backends
│   │   └── ow_port_tim_dma.h  # 17 ow_port_* functions: slot timing, capture, feed
│   ├── stm32f1/            # STM32F1: TIM1 + DMA1 + PA10 (header-only static inline)
│   │   ├── ow_port_f1.h    # F1 descriptor: clock gates + legacy-CRH pin setup
│   │   ├── STM32F103XB_FLASH.ld  # Linker script, STM32F103xB (with .noinit section)
│   │   ├── stm32f103cb.jflash    # J-Flash project file (make jprogram)
│   │   └── project.jdebug  # SEGGER Ozone project (STM32F103C8, SWD)
│   ├── stm32f0/            # STM32F0: TIM1 + DMA1 + PA10 (header-only static inline)
│   │   ├── ow_port_f0.h    # F0 descriptor: clock gates + MODER/AFR pin setup
│   │   ├── STM32F030X6_FLASH.ld  # Linker script, STM32F030x6 (16KB flash / 4KB RAM)
│   │   ├── stm32f030f4.jflash    # J-Flash project file
│   │   └── project.jdebug  # SEGGER Ozone project (STM32F030F4, SWD)
│   ├── stm32f3/            # STM32F3: TIM1 + DMA1 + PA10 (header-only static inline)
│   │   ├── ow_port_f3.h    # F3 descriptor: clock gates + MODER/AFR pin setup
│   │   ├── STM32F303XC_FLASH.ld  # Linker script, STM32F303xC (256KB flash / 32KB RAM)
│   │   ├── stm32f303vc.jflash    # J-Flash project file (STM32F303VC)
│   │   └── project-f303vc.jdebug # SEGGER Ozone project (STM32F303VC, SWD)
│   ├── stm32g0/            # STM32G0: TIM1 + DMA1 + DMAMUX + PA10 via PA12 remap (header-only static inline)
│   │   ├── ow_port_g0.h    # G0 descriptor: + SYSCFG pad remap and DMAMUX routing
│   │   ├── STM32G031X6_FLASH.ld  # Linker script, STM32G031x6 (32KB flash / 8KB RAM)
│   │   ├── stm32g031f6.jflash    # J-Flash project file
│   │   └── project.jdebug  # SEGGER Ozone project (STM32G031F6, SWD)
│   ├── stm32g4/            # STM32G4: TIM1 + DMA1 + DMAMUX + PA10 (header-only static inline)
│   │   ├── ow_port_g4.h    # G4 descriptor: clock gates + MODER/AFR pin setup + DMAMUX routing
│   │   ├── STM32G474CB_FLASH.ld  # Linker script, STM32G474CB (128KB flash / 96KB RAM)
│   │   ├── stm32g474cb.jflash    # J-Flash project file
│   │   └── project.jdebug  # SEGGER Ozone project (STM32G474CB, SWD)
│   └── stm32f4/            # STM32F4: TIM1 + DMA2 + PA10 (header-only static inline)
│   │   ├── ow_port_f4.h    # STM32F4: thin shim over the shared core (family facts, DMA2 stream + CHSEL/flag overrides, rearm, write_then_read carve-out)
│   │   ├── STM32F407VGT6_FLASH.ld  # Linker script, STM32F407VGT6 (1MB flash / 128KB RAM)
│   │   ├── STM32F401CC_FLASH.ld    # Linker script, STM32F401CC (256KB flash / 64KB RAM)
│   │   ├── STM32F401RE_FLASH.ld    # Linker script, STM32F401xE (512KB flash / 128KB RAM)
│   │   ├── STM32F446RE_FLASH.ld    # Linker script, STM32F446xE (512KB flash / 128KB RAM, no CCM)
│   │   ├── stm32f407vgt6.jflash    # J-Flash project file
│   │   ├── stm32f401cc.jflash      # J-Flash project file (STM32F401CC)
│   │   ├── stm32f401re.jflash      # J-Flash project file (STM32F401xE)
│   │   ├── stm32f446re.jflash      # J-Flash project file (STM32F446xE)
│   │   ├── project.jdebug          # SEGGER Ozone project (STM32F407VGT6, SWD)
│   │   ├── project-f401cc.jdebug   # SEGGER Ozone project (STM32F401CC, SWD)
│   │   ├── project-f401re.jdebug   # SEGGER Ozone project (STM32F401xE, SWD)
│   │   ├── project-f446re.jdebug   # SEGGER Ozone project (STM32F446xE, SWD)
│   │   └── HARDWARE-NOTES.md  # F4-specific DMA/timing notes
├── config/                # Build settings shared by both build systems
│   └── optim.mk            # Optimisation profiles + the no-LTO family list
├── chips/                  # Per-part build identity, one file per part
│   ├── f103xb.mk           # CMSIS macro, startup, linker script, debugger
│   ├── f030x6.mk           #   projects, SVD and default clock. Shared by
│   ├── g031xx.mk           #   the Makefile and the CMake build
│   ├── f407xx.mk           #   (OW_CHIP=<name> / -DOW_CHIP=<name>)
│   ├── f303xc.mk           # 256KB flash / 32KB RAM (F3-DISCOVERY, 72MHz default)
│   ├── f401xc.mk           # 256KB flash / 64KB RAM
│   ├── f401xe.mk           # 512KB flash / 128KB RAM
│   ├── f446xx.mk           # 512KB flash / 128KB RAM, 180MHz default
│   └── g474cb.mk           # 128KB flash / 96KB RAM, 170MHz default (WeAct G474 Long, 8MHz HSE)
├── src/                    # Project source files
│   ├── ow_stats.c          # Signal statistics implementation (histogram, UART dump)
│   ├── onewire.c           # 1-Wire layer: state machine + bus primitives
│   │                       #               + non-blocking Search ROM engine
│   ├── ds18b20.c           # Driver: DS18B20 command set on the 1-Wire layer.
│   │                       # Compiles as ONE translation unit: #includes its
│   │                       # four functional parts in dependency order.
│   ├── ds18b20_search.c    # (include-only part) ROM device table + search/alarm
│   ├── ds18b20_txn.c       # (include-only part) command transactions + parasite
│   ├── ds18b20_resolution.c # (include-only part) non-blocking resolution change
│   ├── ds18b20_measure.c   # (include-only part) DS18B20_ST_* measurement machine
│   └── syscall.c           # Minimal libc stubs (_write/_sbrk/...) for bare metal
├── examples/               # Example applications
│   ├── app/                # Shared application layer (UART, clock, init)
│   │   ├── app.c           # app_init(), UART TX ring buffer, busy LED
│   │   └── app.h           # Shared application layer interface
│   ├── 1_basic/main.c      # Single sensor, unconditional (Skip ROM)
│   ├── 2_device_search/main.c  # Device search + per-device poll (no broadcast convert)
│   ├── 3_round_robin/main.c    # Device search + sequential poll of all
│   ├── 4_scan_mode/main.c  # Device search + simultaneous conversion
│   ├── 5_commands/main.c   # Device search + command transactions
│   │                       # (ROM, power supply, TH/TL, Copy/Recall EEPROM)
│   ├── 6_statistics/main.c # Device search + stats dump every N cycles
│   └── 7_low_power/main.c  # Device search + WFE low-power sleep on long stages
├── tests/                  # Host test suite (no hardware required)
│   ├── mock/               # Behavioural TIM1/DMA model + register mocks
│   ├── fuzz/               # libFuzzer harnesses (ASAN/UBSAN, 10 harnesses)
│   ├── test/               # Unity-based test cases
│   ├── integration/        # Consumer fixtures: both CMake paths + PlatformIO
│   ├── check_chips.sh      # Part-matrix check (make test-chips)
│   ├── check_mock_headers.sh  # Mocks vs real CMSIS macros (make test-mocks)
│   ├── check_ram_budget.sh    # Library .bss ceilings (make test-ram)
│   ├── check_library_manifest.sh  # library.json vs the PIO consumer
│   ├── check_version.sh    # Version in every declaration site (make test-version)
│   └── check_elf_variant.sh    # Each build links its own objects
├── cmake/                  # CMake package
│   ├── arm-none-eabi-gcc.cmake  # Bare-metal cross-compilation toolchain file
│   └── stm32_async_1wireConfig.cmake.in  # find_package() config template
├── docs/                   # Documentation assets
│   └── api/                # Doxygen-generated API reference
├── .github/                # GitHub configuration
│   ├── workflows/          # CI (build.yml, ci.yml) and release (release.yml)
│   ├── ISSUE_TEMPLATE/     # Bug report / feature request templates
│   └── PULL_REQUEST_TEMPLATE.md
├── CMSIS/                  # Makefile's downloaded dependencies (gitignored)
│   ├── core/               # ARM CMSIS 5 core headers
│   └── device/             # STM32 device headers and startup (F1/F0/F3/G0/F4/G4) + SVD
├── _deps/                  # CMake FetchContent clones of the same CMSIS sources (gitignored)
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
├── Doxyfile                 # API docs config (CI api-docs job)
├── LICENSE                 # MIT
├── Makefile
├── README.md
├── SECURITY.md
└── .clang-format           # formatting rules (version pinned by CI)
```

## Examples

Seven ready-to-run example applications are provided; select one with `APP`:

| APP               | File                                  | Behaviour                                                        |
|-------------------|---------------------------------------|------------------------------------------------------------------|
| (default)         | `examples/1_basic/main.c`             | Unconditional polling of a single DS18B20 via Skip ROM (0xCC).   |
| `2_device_search` | `examples/2_device_search/main.c`     | Startup device search + per-device polling: each sensor is converted and read back individually via Match ROM (one `Convert T` per device, no broadcast conversion). |
| `3_round_robin`   | `examples/3_round_robin/main.c`       | Startup device search + sequential polling of every sensor found (up to `DS18B20_MAX_DEVICES`). |
| `4_scan_mode`     | `examples/4_scan_mode/main.c`         | Startup device search + simultaneous broadcast conversion: one `Convert T` (Skip ROM) converts all sensors in parallel, then each is read back via Match ROM. |
| `5_commands`      | `examples/5_commands/main.c`          | Startup device search + non-blocking command transactions on the first sensor: Read Power Supply (0xB4), raw Read Scratchpad (0xBE), Write Scratchpad TH/TL (0x4E), Copy Scratchpad (0x48) to the EEPROM, Recall EEPROM (0xB8), single-device Read ROM (0x33), forced alarm thresholds plus Alarm Search (0xEC) showing that only alarmed devices respond, then steady-state measurement of the selected device. |
| `6_statistics`    | `examples/6_statistics/main.c`        | Startup device search + sequential measurement with signal statistics. The `6_statistics` target auto-enables `-DOW_STATS_ENABLE=1`. Accumulates per-sensor pulse-width min/max, a global histogram and error counters over N sweeps - one sweep being one pass over every device - then streams the full report over UART as a non-blocking dump. The period is `STATS_DUMP_SWEEPS`, default 10, defined only in the example's source and overridable with `-DSTATS_DUMP_SWEEPS=N` in `EXT`. |
| `7_low_power`     | `examples/7_low_power/main.c`         | Low-power example (same search + sequential loop as `2_device_search`): with `-DOW_PORT_LOW_POWER=1` the **driver** enters `__WFE()` inside `ds18b20_poll()` while a long 1-Wire stage (> 1 ms: temperature conversion, scratchpad read, EEPROM hold-off) is running. The application loop is unchanged and still fully non-blocking, and the interval between cycles is a plain `app_millis()` deadline. Without the define, the example uses the standard polling loop. |

```bash
make                                          # build 1_basic -> build/ds18b20_1_basic.elf
make APP=2_device_search                      # build 2_device_search -> build/ds18b20_2_device_search.elf
make APP=3_round_robin                        # build 3_round_robin -> build/ds18b20_3_round_robin.elf
make APP=4_scan_mode                          # build 4_scan_mode -> build/ds18b20_4_scan_mode.elf
make APP=5_commands                           # build 5_commands -> build/ds18b20_5_commands.elf
make APP=6_statistics                         # build 6_statistics -> build/ds18b20_6_statistics.elf (OW_STATS_ENABLE auto-added)
make APP=7_low_power EXT="-DOW_PORT_LOW_POWER=1"   # build 7_low_power -> build/ds18b20_7_low_power.elf (WFE low-power)
make debug APP=3_round_robin                  # debug build of 3_round_robin (for J-Link/ST-Link)

# STM32F030 target (same examples, bus on PA10):
make OW_TARGET=f0 APP=4_scan_mode

# STM32G031 target (bus on PA12 pad via remap, console on PA11):
make OW_TARGET=g0 APP=4_scan_mode
# ... or on the raw internal HSI16 (no PLL):
make OW_TARGET=g0 SYSCLK_MHZ=16 APP=4_scan_mode

# STM32F303VC target (F3-DISCOVERY / MB1035B, bus on PA10, console on PA9):
# 72MHz is the default and needs the board's 8MHz external clock, which the
# ST-LINK drives onto OSC_IN and which is read in bypass mode. This family has
# no HSI16 and PLLMUL tops out at 16, so 64MHz is all the internal RC can reach.
make OW_TARGET=f3 APP=4_scan_mode
make OW_TARGET=f3 SYSCLK_MHZ=64 APP=4_scan_mode   # HSI/2 + PLL, no external clock
make OW_TARGET=f3 SYSCLK_MHZ=8  APP=4_scan_mode   # raw HSI, nothing configured

# STM32F407 target (same examples, bus on PA10):
make OW_TARGET=f4 APP=4_scan_mode

# STM32F401CC target (F401 Black Pill, 84MHz HSE+PLL default):
make OW_TARGET=f4 OW_CHIP=f401xc APP=4_scan_mode

# STM32F401xE target (512KB flash / 128KB RAM, same 84MHz default):
make OW_TARGET=f4 OW_CHIP=f401xe APP=4_scan_mode
# ... or on the raw internal RC (no HSE crystal needed):
make OW_TARGET=f4 OW_CHIP=f401xc SYSCLK_MHZ=16 APP=4_scan_mode

# STM32F446RE target (WeAct F446, 180MHz HSE+PLL + over-drive default):
make OW_TARGET=f4 OW_CHIP=f446xx APP=4_scan_mode
# ... 16MHz on the raw internal RC, or straight off the 8MHz crystal:
make OW_TARGET=f4 OW_CHIP=f446xx SYSCLK_MHZ=16 APP=4_scan_mode
make OW_TARGET=f4 OW_CHIP=f446xx HSE_MHZ=8 SYSCLK_MHZ=8 APP=4_scan_mode

# STM32G474CB target (WeAct G474 Long, 170MHz HSE+PLL + Range 1 Boost default):
make OW_TARGET=g4 APP=4_scan_mode
# ... 16MHz on the raw internal HSI16 (no HSE crystal needed):
make OW_TARGET=g4 SYSCLK_MHZ=16 APP=4_scan_mode
```

Notes:

- All examples use `app_init()` (from `examples/app/app.h`) to set up the system
  clock, USART1 TX and the busy LED in a single call.
- `1_basic` uses Skip ROM, so it is meant for a **single sensor** on the bus.
  With several sensors connected, all of them respond to the read command and
  the bus data collides (CRC failures are expected).
- `2_device_search` performs a startup Search ROM, then measures each discovered sensor
  **individually** via Match ROM (one `Convert T` per device, no broadcast
  conversion) in round-robin order. A separator `--------------------------------`
  is printed between full rounds. With one sensor it behaves like `1_basic` but
  with ROM addressing; with N sensors a round costs `N × conversion`. Supports
  `-DOW_PARASITE_POWER=1` (strong pull-up handled per conversion).
- `3_round_robin` measures the devices found at startup one at a time, in round-robin
  order. With exactly one sensor it behaves like `1_basic`.
- `4_scan_mode` (scan mode) converts every discovered sensor in parallel: a single
  conversion wait covers all devices, so N devices take `1 x conversion + N x
  read` instead of `N x conversion`. Before converting, it programs the
  conversion resolution to every sensor with one broadcast Write Scratchpad
  (`ds18b20_set_resolution()`), because scan mode assumes a uniform resolution:
  the single conversion wait must match the whole fleet. Skipping this can make
  a 12-bit sensor that follows a 9-bit one read back its 85.0 °C power-on-reset
  value. Each reading is reported through `ds18b20_complete()` in device-table
  order; `ds18b20_scan_index()` / `ds18b20_device_rom()` identify the sensor.
  Scan mode is mutually exclusive with `ds18b20_select()`.
  With several parasite-powered devices, a simultaneous broadcast conversion
  draws all their current from the one strong pull-up. A sagging supply (for
  example feeding the bus from a USB adapter) can brown out a marginal sensor
  (a stuck 127.9 °C reading with a valid CRC). Feed the fleet from a dedicated
  supply with an adequate pull-up — see the STM32F407VGT6 section in Hardware
  Verified — and the broadcast conversion stays reliable at the high PLL clock.
- `5_commands` targets the first sensor found by the search (Match ROM) and runs the
  non-blocking command sequence once at startup: power supply, raw scratchpad,
  TH/TL write with a Copy/Recall pair to demonstrate EEPROM persistence, and
  the single-device Read ROM. Each command advances by one hardware operation
  per `*_poll()` call; `ds18b20_last_command_ok()` verifies the result. The
  sequence closes with forced alarm thresholds (TH=-55C, TL=+125C on the
  selected device, so it alarms deterministically whatever the room
  temperature) plus an Alarm Search (0xEC) pass that reports only alarmed
  devices — the only example exercising
  `ds18b20_alarm_search_start()` / `ds18b20_alarm_search_poll()` /
  `ds18b20_alarm_search_count()`. Other fleet members may join from thresholds
  retained in their own RAM/EEPROM.
- `6_statistics` extends the `3_round_robin` sequential loop with signal statistics
  (`-DOW_STATS_ENABLE=1`, auto-enabled by `make APP=6_statistics`). After
  `STATS_DUMP_SWEEPS` sweeps - one sweep is one pass over every device, so a
  batch covers `STATS_DUMP_SWEEPS * devices` measurements and yields that
  many samples per sensor - the accumulated
  per-sensor pulse-width min/max,
  13-bucket histogram (0–60+ µs) and error counters are streamed over UART by
  `ow_stats_dump_poll()` (one line per call, non-blocking); the measurement
  loop is paused during the dump and resumed afterwards via `ow_stats_reset()`.
  Supports `-DOW_PARASITE_POWER=1`.
- Programming targets (`make jprogram` / `make program`) flash whichever
  example is currently selected by `APP`.
- Two build knobs have no dedicated example by design: `-DOW_DRIVE_ACTIVE=1`
  switches master-only write slots to push-pull (experimental; covered by the
  `test-active` host suite, same loop as every example so nothing demo-specific
  would show on the console), and `-DDS18B20_MAX_DEVICES=1` trims the scan
  table from 64 to 8 bytes for single-sensor builds (see CHANGELOG).

## Hardware Verified

The subsections below describe the boards the driver runs on (wiring, clocks,
console). Measurement logs are deliberately not kept in the repo.

### STM32F103C8T6 (Blue Pill)

Reference setup: STM32F103C8T6 (Blue Pill), 8 × DS18B20 on one 1-Wire bus
(PA10), flashed via ST-Link, USART1 TX at 115200 8N1 via a CP2102 USB-UART
adapter.

### STM32F030F4P6 (TSSOP20)

The same examples run on an STM32F030F4P6 minimum board: the
1-Wire bus on **PA10** (TIM1 CH3/CH4 pair — PA8 is not bonded out in this
package), USART1 TX on PA9, busy LED on PA4, flashed via ST-Link SWD. The bus
carries 8 × DS18B20.

### STM32F303VC (F3-DISCOVERY / MB1035B)

Validated on an **STM32F3-DISCOVERY** (MB1035B, STM32F303VC, 256KB flash /
32KB SRAM): 7 × DS18B20 in parasite power mode on one 1-Wire bus on **PA10**
(TIM1 CH3/CH4 pair, DMA1 channels 3/4 — the same fixed pair as F1/F0, see the
Supported-families table), console on USART1 TX / **PA9** (default mapping) at
115200 8N1, flashed via ST-Link SWD.

### STM32G031F6P6 (WeAct TSSOP20 board)

Validated on a WeAct STM32G031F6P6 minimum board: 6 × DS18B20 in parasite
power mode on one 1-Wire bus (logical PA10 on the physical PA12 pad), USART1
TX on logical PA9 (physical PA11), flashed via ST-Link SWD. Builds: the
default 64MHz (HSI16+PLL) and the raw-HSI16 `SYSCLK_MHZ=16` build. The bus pads
are reachable only through the SYSCFG remap described in Hardware Connections
below; the USB-C connector of this board is wired to PA11/PA12 and must stay
unplugged while the driver owns the bus.

### STM32G474CBT6 (WeAct STM32G474CBT6 Long)

Validated on a WeAct STM32G474CBT6 Long board: 7 × DS18B20 in parasite
power mode on one 1-Wire bus on **PA10** (TIM1 CH3/CH4 pair, DMA1 channels 3/4
through DMAMUX — requests 43/45, 8-bit feed tables), console on USART1 TX /
**PA9** (AF7) at 115200 8N1 via a CP2102 USB-UART adapter, flashed via ST-Link
SWD. Builds: the default 170MHz (HSE 8MHz + PLL M=2, N=85, R=2, Range 1 Boost)
and the raw-HSI16 `SYSCLK_MHZ=16` build. The board carries an 8 MHz crystal;
`HSE_MHZ=8` in `chips/g474cb.mk` derives the PLL dividers. The bus pads are
directly on PA10 (no remap needed). Build with `make OW_TARGET=g4`.

### STM32F407VGT6 (STM32F4DISCOVERY)

Validated on an **STM32F4DISCOVERY** (MB997C, STM32F407VGT6, 8 MHz HSE):
7 × DS18B20 in parasite power mode on one 1-Wire bus on **PA10** (TIM1 CH3/CH4
pair, DMA2 streams 2/4 — the feed stream runs 16-bit), with the console on
USART1 TX / **PB6** at 115200 8N1 (AF7; the F4DISCOVERY carries no signal on
the default PA9 USART1 pad, so the console rides the remapped PB6) through a
USB-TTL adapter; flashed via the on-board ST-Link SWD. **PB6 is an
F4DISCOVERY-specific choice, not an F4 family default** — a WeAct F446RET6
answers with its CP210x VCP on **PB6** too (measured, not the PA9 the silkscreen
implies), so the default build works on both boards with no flag. For an F4
board whose console really is on PA9, build with
`-DOW_UART_USART1_PA9` (`make OW_TARGET=f4 OW_CHIP=f446xx EXT=-DOW_UART_USART1_PA9`)
or attach the USB-TTL adapter to PB6. `SYSCLK_MHZ=16` selects raw HSI.
The F4-specific DMA topology and timing choices are documented in
`port/stm32f4/HARDWARE-NOTES.md`.

### STM32F401CC (F401 Black Pill)

The **same backend header** drives the 84 MHz-capped STM32F401 family: TIM1,
the DMA2 dual-stream topology and the `CHSEL=6` request map are identical to
the F407 (RM0368 §9.3.3 Table 29 via `port/stm32f4/HARDWARE-NOTES.md`), so the
port layer is chip-generic. The chip-specific parts (CMSIS device header
`stm32f401xc.h`, `startup_stm32f401xc.s`, the `STM32F401CC_FLASH.ld` linker
script, the `app.c` 84MHz HSE+PLL clock-config branch, and the library's 84MHz
clock default) are selected with `OW_CHIP=f401xc` — all of it in
`chips/f401xc.mk`.

**The crystal is the one thing the part cannot decide.** This board carries a
**25 MHz** crystal, not the 8 MHz the F407DISCOVERY does, and the F4 PLL divides
the crystal down before multiplying: `PLLM = HSE_MHZ` puts the PLL input at 1MHz
and `PLLN = 2 × SYSCLK` sets the VCO, so 25MHz gives M=25/N=168 for the same
84MHz that 8MHz reaches with M=8/N=168. `chips/f401xc.mk` therefore declares
`CHIP_HSE_MHZ = 25`; every other board passes `HSE_MHZ=<n>` to the build. This
matters more than it looks: with the divider hardcoded to 8, a 25MHz crystal
puts 3.125MHz into a PLL input specified for 1–2MHz, the PLL never locks, and
the old unbounded wait left the board silent with no way to tell a dead board
from a wrong one.

Expected wiring matches the F407: bus on **PA10** (TIM1 CH3/CH4, DMA2 streams
2/4), parasite mode on the same 2.2 kΩ pull-up, and the console on **PB6** —
not PA9, because the F4 UART path is pinned to PB6 for the F4DISCOVERY, which
carries no signal on the default PA9 USART1 pad.

The `xE` parts (F401CD/RD/VD/CE/RE/VE, 512KB flash / 128KB RAM) are the same
core with twice the memory, so they share everything above and differ only in
the memory map: `OW_CHIP=f401xe` selects `chips/f401xe.mk` with
`STM32F401RE_FLASH.ld` (512K/128K) and the `stm32f401xe.h` /
`startup_stm32f401xe.s` device layer. Like the xC variant it is unproven on
hardware — no F401xE part was available. Note that the CMSIS `STM32F401xE`
macro was already honoured by `onewire.h` before it had a linker script to go
with it, so a hand-rolled `-DSTM32F401xE` used to link against the xC script's
256K/64K; `make test-chips` now checks that every part's script exists.

### STM32F446RE (WeAct F446RET6)

Validated on a **WeAct STM32F446RET6** (ST-LINK V2J45S7, STM32F446 Rev A,
512KB flash): 7 × DS18B20 in parasite power mode on one 1-Wire bus on **PA10**
(TIM1 CH3/CH4 pair, same DMA2 streams 2/4 topology as the F407), console on
USART1 TX / **PB6** (the board's CP210x VCP answers there at 115200 8N1 — the
default build needs no UART flag), flashed via OpenOCD over the on-board
ST-Link.

**6_statistics — signal statistics** (`examples/6_statistics/main.c`): startup device search +
sequential measurement with the optional `ow_stats` module. By default the
module accumulates after every `STATS_DUMP_SWEEPS` sweeps (default 10, defined
only in the example's source) — per-sensor pulse-width min/max, a 13-bucket
logarithmic histogram (0–60+ µs) and error counters (CRC, presence, other),
then streams the full report over UART.

Build and run:

```sh
make OW_TARGET=g0 APP=6_statistics EXT="-DOW_STATS_ENABLE=1 -DOW_PARASITE_POWER=1"
```

**7_low_power — low power** (`examples/7_low_power/main.c`): the same search + sequential loop as
`2_device_search`, but built with `-DOW_PORT_LOW_POWER=1`.

> **What low-power mode does not change.** Low-power mode does not change
> 1-Wire execution. TIM+DMA continue to control all bus timing; `WFE` allows
> the CPU to sleep during sufficiently long hardware-controlled transaction
> stages.

The one-wire driver then enables the TIM1 update **interrupt source** (UIE)
and the `SEVONPEND` system-control bit. This is not a real interrupt: UIE is
enabled **only** to generate a pending event that wakes `WFE()` — no ISR is
ever installed and no `NVIC_EnableIRQ` call is made. With the define set, the
driver sleeps **itself** in `__WFE()` while a *long* 1-Wire stage is running and
is woken by the timer's update event. The sleep lives entirely inside
`ds18b20_poll()`, so the application code is byte-for-byte the same as in every
other example: it calls `ds18b20_poll()`, does its other work, and gets control
back when the stage is over. Stages treated as "long" (strictly more than 1 ms)
are the temperature conversion (up to 750 ms), the scratchpad read (~5 ms) and
an EEPROM hold-off (10 ms); the interval between measurement cycles is the
application's own (a plain `app_millis()` deadline, see below); short stages
(reset, commands, search reads) are still handled by standard polling.
Power is **not measured** yet — this example's goal is only to establish the
mechanism and measure the CPU-time saving.

> **No interrupt anywhere in the application.** Milliseconds come from the ARM
> SysTick counter running at 1 kHz with its interrupt *disabled*
> (`app_time_init()`, called from `app_init()`): no `SysTick_Handler` is
> installed and no NVIC bit is enabled. `app_millis()` reads the `COUNTFLAG`
> bit — set once per wrap — and folds it into a counter, so it costs one
> register read, never waits, and needs the main loop to run at least once per
> millisecond. Because a conversion takes hundreds of milliseconds, that rule
> only has to hold for the *pauses* between cycles, which is exactly where the
> application is doing its own work. Caveat: `app_millis()` counts *observed*
> SysTick wraps (one per read), so it under-counts whenever the loop is blocked
> for more than a millisecond — which is precisely what `OW_PORT_LOW_POWER=1`
> does inside `ds18b20_poll()` for up to 750 ms. In a low-power build the
> millisecond figure is therefore valid for the pauses between cycles (where the
> loop does spin) and deliberately excludes the conversion time.

Build and run:

```sh
make OW_TARGET=g0 APP=7_low_power EXT="-DOW_PORT_LOW_POWER=1"   # (append -DOW_PARASITE_POWER=1 on a parasite bus)
make OW_TARGET=g0 APP=7_low_power                              # same example, but standard polling (define omitted)
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
| PA4  | Busy LED (optional) | Active-low                         |
| PA13/PA14 | SWDIO/SWCLK    | ST-Link SWD programming            |

Note: the same 4.7kΩ pull-up is required between PA10 and 3.3V.

### STM32F303VC (F3-DISCOVERY / MB1035B)

| Pin | Function | Notes |
|-----|----------|-------|
| PA10 | 1-Wire Data | TIM1_CH3, open-drain AF6 after the default AF map |
| PA9  | USART1 TX (115200) | AF7; RX line of the USB-UART adapter (the on-board ST-LINK VCP is on PA2/PA3) |
| PE8  | Busy LED (optional) | **Active-high** (LED4 blue) — the opposite polarity of F1/F0/G0 |
| PA13/PA14 | SWDIO/SWCLK | ST-Link SWD programming |

Note: the board's 8 MHz clock is driven onto OSC_IN by the ST-LINK (bypass
mode, not a crystal), which is what the 72 MHz default is built against; the
64 MHz and 8 MHz builds need no external clock.

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
(`-DOW_DRIVE_ACTIVE=1`) the pin is temporarily switched to push-pull during
master-only write slots and restored to open-drain afterwards — see
[Bus Electrical Model](#bus-electrical-model).

### STM32G474CBT6 (WeAct STM32G474CBT6 Long)

| Pin  | Function            | Notes                              |
|------|---------------------|------------------------------------|
| PA10 | 1-Wire Data         | TIM1_CH3 AF6, open-drain (default topology) |
| PA9  | USART1 TX (115200)  | AF7; RX line of the USB-UART adapter |
| PA8  | Busy LED (optional) | **Active-high** (blue LED on WeAct board; CxT6 = PA8 per WeAct BSP) |
| PA13/PA14 | SWDIO/SWCLK    | ST-Link SWD programming            |

Note: the same 4.7kΩ pull-up is required between the bus pin and 3.3V.
The board carries an **8 MHz crystal** (`HSE_MHZ=8` in `chips/g474cb.mk`);
the 170 MHz PLL (M=2, N=85, R=2, Range 1 Boost) and the raw-HSI16
`SYSCLK_MHZ=16` build are supported. Build with `make OW_TARGET=g4`.
PC13 on this board is the user button (pull-up to VCC), not an LED.

### STM32F407VGT6 (STM32F4DISCOVERY)

The STM32F4 backend is hardware-validated (see Hardware Verified above). The
bus pin defaults to PA10 (the same wiring as F1/F0). The F4DISCOVERY board
carries no signal on the default PA9 USART1 pad, so the console rides USART1
TX remapped to PB6 (AF7); see the complete build line in Hardware Verified:

| Pin  | Function            | Notes                              |
|------|---------------------|------------------------------------|
| PA10 | 1-Wire Data         | TIM1_CH3, open-drain AF1; also OTG_FS_ID |
| PB6  | USART1 TX (115200)  | USART1 AF7 (remapped from PA9); RX line of the USB-UART adapter |
| PD12 | Busy LED (optional) | **Active-high** (F4DISCOVERY LD4: pin → LED → GND); the F4 console is on PB6, not the F1/F0/G0 PA4 pin |
| PA11 | Free (former LA marker) | Left untouched by the driver; logic-analyzer sync, if needed, goes through the opt-in OW_PORT_MARKER_TOGGLE() hook (no-op by default); also USB OTG FS D− |
| PA13/PA14 | SWDIO/SWCLK    | ST-Link SWD programming            |

Important: the board's USB OTG FS connector is wired to the pins above —
**do not plug a USB cable into it while the driver runs**. A standard A-cable
grounds OTG_FS_ID on **PA10** and holds the 1-Wire bus LOW; **PA11** is USB D−
and **PA12** is USB D+, so a connected cable also conflicts with any use of PA12.

Note: a pull-up is required between the bus pin and the supply — 4.7 kΩ for a
single device, 2.2 kΩ for a parasite-powered multi-drop fleet (fed from a
dedicated supply; see the `4_scan_mode` note). The board's 8 MHz HSE drives
the default 168 MHz PLL clock.

### STM32F446RE (WeAct F446RET6)

The F446 backend is hardware-validated over its full clock range (see the
F446 entry in Hardware Verified). Bench wiring on this board:

| Pin  | Function            | Notes                              |
|------|---------------------|------------------------------------|
| PA10 | 1-Wire Data         | TIM1_CH3, open-drain AF1 (same topology as the F407) |
| PB6  | USART1 TX (115200)  | AF7; the on-board CP210x VCP answers here on the default build |
| PB2  | Busy LED (optional) | Active high; silkscreen "B2". Select with `-DOW_F4_LED_PB2` (`make OW_TARGET=f4 OW_CHIP=f446xx EXT=-DOW_F4_LED_PB2`) |
| PA13/PA14 | SWDIO/SWCLK    | On-board ST-Link SWD programming            |

Important: the board's user button is on **PC13** (pull-up to VCC, not an LED),
and the bus pull-up for a parasite-powered fleet is 2.2 kΩ fed from a dedicated
supply, as on the F407. The F446 bench uses the same 8 MHz crystal and reaches
180MHz only through the PWR over-drive sequence (see Clocking invariant above).

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

Before `ds18b20_init()`, the driver must know the system-clock frequency: the
TIM1 prescaler that generates the 1-Wire timing derives from
 `OW_PORT_SYSCLK_MHZ`. Each backend has a built-in default (72 on STM32F1,
 48 on STM32F0, 72 on STM32F3, 64 on STM32G0, 170 on STM32G4, and on F4 the part's own
 default: 168 on F407, 84 on F401, 180 on F446), and the
examples enable that clock in their
`app_init()`. If your firmware runs the MCU at a different frequency, provide
it explicitly — e.g. `-DOW_PORT_SYSCLK_MHZ=8` for an 8 MHz HSI build.

**Single sensor (Skip ROM).** With one DS18B20 on the bus there is no need for
a ROM search — the driver uses Skip ROM (broadcast) addressing by default:

```C
#include "ds18b20.h"

int main(void) {
    ds18b20_init();  // One-time initialization

    ds18b20_start_measure();  // Request one measurement cycle

    while (1) {
        ds18b20_poll();  // Call repeatedly from main loop
        // ... report in ds18b20_complete(), decide the cadence here:
        //     when to call ds18b20_start_measure() again ...
    }
}
```

**Multiple sensors (bus search).** To find every device, run the non-blocking
Search ROM state machine, then address each sensor by ROM with
`ds18b20_select()`:

```C
#include "ds18b20.h"

static uint8_t found_roms[8][DS18B20_ROM_BYTES];
static uint8_t found_count = 0;

// Called for every DS18B20 the search finds; return 0 to keep searching.
static uint8_t on_device_found(const uint8_t* rom) {
    for (uint8_t i = 0; i < DS18B20_ROM_BYTES; i++) {
        found_roms[found_count][i] = rom[i];
    }
    found_count++;
    return 0;
}

int main(void) {
    ds18b20_init();  // One-time initialization

    // The search owns the bus until it finishes: do not call ds18b20_poll()
    // while it is running.
    ds18b20_search_start(on_device_found, 8);
    while (!ds18b20_search_poll()) {
        // Repeatedly advance the search; returns 1 when it is finished.
    }

    if (found_count > 0) {
        ds18b20_select(found_roms[0]);  // Measure the first sensor
        ds18b20_start_measure();        // Request one measurement cycle

        while (1) {
            ds18b20_poll();  // Call repeatedly from main loop
            // Other application code... decide here when to request the
            // next cycle with ds18b20_start_measure()
        }
    }
    return 0;
}
```

See `examples/1_basic/main.c` for a complete single-sensor setup and
`examples/2_device_search/main.c` for the search + round-robin loop.

### 3. Implement Callbacks (Optional)

Both callbacks are optional; the driver ships empty weak implementations. The
shared example layer `examples/app/app.c` supplies a strong `ds18b20_busy()`
that drives the onboard LED (F1: PC13, F0/G0: PA4, F3: PE8, F4: PD12 — or PB2
with `-DOW_F4_LED_PB2` on a WeAct F446RET6, G4: PA8 on a WeAct G474 Long).
There is no shared
`ds18b20_complete()`: each example implements its own, because the output
format differs.

> **The callback example below is STM32F1-specific** (Blue Pill onboard LED on
> PC13, active-low). Pin and polarity differ per MCU — see
> [Hardware Connections](#hardware-connections): F1 uses PC13 active-low;
> F0/G0 use PA4 active-low; F3 uses PE8 **active-high** (LED4 blue on the
> F3-DISCOVERY); F4 uses PD12 **active-high** (LD4 green on the F4DISCOVERY),
> or PB2 active-high with `-DOW_F4_LED_PB2` (the WeAct F446RET6's B2 LED —
> PC13 is that board's user button, not an LED); G4 uses PA8 active-high
> (WeAct G474 Long blue LED — PC13 there is the user button too). On F0/G0/G4
> the same logic targets `GPIOA` instead of `GPIOC`.

```C
// Busy indicator — e.g. LED toggling during measurement
// STM32F1 (Blue Pill): onboard LED on PC13, active-low.
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
-   **wget** (or **curl**): Required for downloading CMSIS build dependencies.
-   **Host toolchain** for the test and lint suites: **gcc** (`make test*`,
    `make test-mocks`, `make test-manifest`), **clang with libFuzzer**
    (`make fuzz-all`), **cppcheck** (CI static analysis), **doxygen** (API docs),
    **lcov** (only to turn the `.gcda` files from `COVERAGE=1` into a report — CI
    does this step, there is no Makefile target for it), and **python3**
    (`make test-manifest`).
-   **cmake** ≥ 3.14: only for the CMake build path (`find_package` /
    FetchContent); the Makefile path does not need it.
-   **clang-format**: use the exact version CI pins (see the `format` job in
    `.github/workflows/ci.yml`) — a different release can disagree about the same
    source.
-   **POSIX shell:** On Linux/macOS any shell works; on Windows the `Makefile`
    targets (including `make download-deps`) need a Linux-like environment —
    Git Bash, MSYS2 or WSL. `cmd`/PowerShell are **not** supported for builds.
-   **Programmer tools:**
    -   **ST-LINK:** `st-flash` (Linux/macOS) or `ST-LINK_CLI.exe` (Windows)
    -   **J-LINK:** `JFlashExe` / `JFlash.Exe` / `JLinkGDBServerCL.exe`

> **Building on Windows.** The `Makefile` uses POSIX shell constructs
> (`mkdir -p`, `rm -rf`, `awk`, `sleep`, `command -v`) and downloads
> dependencies via `wget`/`curl`, so the build itself cannot run from native
> `cmd`/PowerShell. Use Git Bash, MSYS2 or WSL and ensure `make`, `wget` (or
> `curl`) and `arm-none-eabi-gcc` are on `PATH`. The Windows-native flashers
> listed above are only invoked by `make program` / `make jprogram` — they do
> not replace the POSIX build environment.

### CMSIS Dependencies

ARM CMSIS core headers and STM32 device files (F1/F0/F3/G0/F4/G4) are not stored in the
repository. They are downloaded automatically at build time to
`CMSIS/core/` and `CMSIS/device/`:

```bash
make download-deps
```

To remove them (this also drops a root-level CMake FetchContent clone directory
`_deps/`; the `build/_deps/` tree that a normal `cmake -B build` creates is
removed by `make clean`):

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

Output goes to `build/` (`ds18b20_<app>.elf`, `.hex`, `.bin` — e.g. `ds18b20_1_basic.elf` for the default `APP=1_basic`).

### Common Targets

| Target | Description |
|--------|-------------|
| `make` / `make all` | Build release |
| `make debug` | Build with debug symbols |
| `make test` | Build and run host tests (PC toolchain) |
| `make clean` | Remove build artifacts, including any CMake install prefix outside `build/` |
| `make download-deps` | Download CMSIS dependencies |
| `make clean-deps` | Remove downloaded dependencies (`CMSIS/`, plus a root-level `_deps/`; `build/_deps/` goes with `make clean`) |
| `make program` | Flash via ST-LINK |
| `make jprogram` | Flash via J-LINK |
| `make test-f0` | Build and run host tests against the STM32F0 backend mock |
| `make test-f3` | Build and run host tests against the STM32F3 backend mock |
| `make test-g0` | Build and run host tests against the STM32G0 backend mock |
| `make test-f4` | Build and run host tests against the STM32F4 backend mock |
| `make test-g4` | Build and run host tests against the STM32G4 backend mock |
| `make test COVERAGE=1` | Host tests with gcov instrumentation (`.gcno`/`.gcda` land in the tree root; turning them into a report is a CI step — `lcov --capture` + `genhtml` — not a Makefile target) |
| `make test-active` | Build and run host tests for the active-drive write path (`-DOW_DRIVE_ACTIVE=1`) |
| `make test-active-f0` | Same as above against the STM32F0 backend mock |
| `make test-active-f3` | Same as above against the STM32F3 backend mock |
| `make test-active-g0` | Same as above against the STM32G0 backend mock |
| `make test-active-f4` | Same as above against the STM32F4 backend mock |
| `make test-active-g4` | Same as above against the STM32G4 backend mock |
| `make test-lowpower` | Same suite rebuilt with `-DOW_PORT_LOW_POWER=1` (WFE path, F1) |
| `make test-lowpower-f0` | Same as above against the STM32F0 backend mock |
| `make test-lowpower-f3` | Same as above against the STM32F3 backend mock |
| `make test-lowpower-g0` | Same as above against the STM32G0 backend mock |
| `make test-lowpower-f4` | Same as above against the STM32F4 backend mock |
| `make test-lowpower-g4` | Same as above against the STM32G4 backend mock |
| `make test-ndebug` | Same suite with `-DNDEBUG -DOW_TEST_PARAM_GUARD`: asserts compiled out, so the rejected-size returns (0) are observable; adds `test_param_guard` |
| `make test-ndebug-f0` | Same as above against the STM32F0 backend mock |
| `make test-ndebug-f3` | Same as above against the STM32F3 backend mock |
| `make test-ndebug-g0` | Same as above against the STM32G0 backend mock |
| `make test-ndebug-f4` | Same as above against the STM32F4 backend mock |
| `make test-ndebug-g4` | Same as above against the STM32G4 backend mock |
| `make test-chips` | Check the part matrix: every `chips/<part>.mk` names repository files that exist, its scalars are well formed, and an unknown family or part is rejected. No toolchain and no CMSIS download needed (`tests/check_chips.sh`) |
| `make test-clocks` | Compile-check the per-family clock defaults, including the F4 parts whose own default differs from the family one (`test-clocks-f1/f0/f3/g0/f4/g4`, where the F4 one covers `f401xc`, `f401xe` and `f446xx`) |
| `make fuzz-all` | Build and run all fuzz harnesses (requires host-side Clang; `FUZZ_TIME=N` for duration) |
| `make fuzz-crc8` | Fuzz `onewire_crc8` alone |
| `make download-licenses` | Download the CMSIS third-party license files into `CMSIS/` |
| `make gccversion` | Show the detected compiler and linker versions |
| `make help` | Show all targets |

Optional build flags (append via `EXT="..."` or `OW_DRIVE_ACTIVE=1`):

| Flag | Effect |
|------|--------|
| `OW_DRIVE_ACTIVE=1` | Enable the optional active-drive write path (`-DOW_DRIVE_ACTIVE=1`): during master-only write slots the bus pin is temporarily switched to push-pull (see [Bus Electrical Model](#bus-electrical-model)). The default remains open-drain. |
| `TIMING=SLOW` | Apply a compile-time timing preset (default `STANDARD`; also `FAST`/`SLOW`/`ROBUST`/`CUSTOM`). Expands into `-DONEWIRE_ONE_PULSE=... -DONEWIRE_ZERO_PULSE=... -DONEWIRE_GUARD_BAND=... -DONEWIRE_SHORT_PULSE_MAX=...` for that preset. Override any single value with `EXT="-DONEWIRE_GUARD_BAND=100"`. See [Configuration → Timing](#timing-1). |
| `EXT="-DOW_PARASITE_POWER=1"` | Parasite-powered bus: raises the default guard band from 5 µs to 100 µs and builds every example with `ds18b20_set_parasite(1)` — the strong-pull-up window is engaged at runtime per conversion. See 6_statistics. |
| `EXT="-DOW_PORT_LOW_POWER=1"` | Enable the opt-in low-power path: the TIM1 update **interrupt source** (UIE) is enabled only to generate a pending event that wakes `WFE()` via `SEVONPEND`, so the **driver** sleeps during long 1-Wire stages (> 1 ms) while the hardware completes the transaction. The sleep sits inside `ds18b20_poll()`, so no application call site changes. No ISR is installed and `NVIC_EnableIRQ` is never called. Without this define builds are byte-identical to the original. |

### Flash

-   **ST-LINK:** `make program` (uses `st-flash` / `ST-LINK_CLI.exe`)
-   **J-LINK:** `make jprogram` (uses `JFlashExe` / `JFlash.Exe`)

### Testing

The driver ships with a host test suite that runs entirely on the PC, no
hardware required:

```bash
make test        # host tests against the STM32F1 backend mock
make test-f0     # same suite against the STM32F0 backend mock
make test-f3     # same suite against the STM32F3 backend mock
make test-g0     # same suite against the STM32G0 backend mock
make test-f4     # same suite against the STM32F4 backend mock
make test-g4     # same suite against the STM32G4 backend mock
```

Both `src/onewire.c` and `src/ds18b20.c` are compiled as a single translation
unit (`tests/mock/ds18b20_test_access.c`) against a behavioural model of the
TIM1/DMA hardware (`tests/mock/hw_model.c`) and a register mock of the target
CMSIS header (`tests/mock/stm32f1xx.h` / `stm32f0xx.h` / `stm32f3xx.h` /
`stm32g0xx.h` / `stm32f4xx.h`) — each suite runs the full driver against its own
backend's channel/DMA wiring. (The driver itself is an amalgamated translation unit
    too: `src/ds18b20.c` `#include`s its four functional parts, so the whole
   driver shares the `ctx`/`txn_ctx`/`res_ctx`/`dev_roms` statics in one
   object file.) The base suite runs 309 tests against the F1/F0 mocks, 310
   against the F3 and F4 backends (the F4 target also builds and runs its
   180 MHz clock-default host variant `ds18b20_test_f4_180mhz.exe` as an
   extra, same count) and 311 against G0 (which adds two DMAMUX
   request-routing tests), 312 against G4 (same two DMAMUX tests plus the
   G4 console-baud test; the 16 MHz variant `ds18b20_test_g4_16mhz.exe`
   runs as an extra, same count).
    The suite covers:

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
      ownership guards and the merged search capture buffers
-   DMA buffer/transfer contracts (`test_dma`), DMAMUX request routing on G0
    (`test_dmamux`)
-   Scratchpad decode and temperature conversion (incl. negative values)
-   Timing configuration and register setup
-   Bus release behaviour between slots
-   Channel broadcast (`test_broadcast`), UART app driver (`test_app_uart`) and
    test-scaffold accessors/harness edge-case branches (`test_harness_api`)
-   `ow_stats` capture and error counters plus the non-blocking dump protocol
    (`test_ow_stats`)

Separate opt-in builds extend the suite: `make test-active` (active-drive, 8
tests, every family), `make test-lowpower` (full suite + low-power WFE path,
321 tests against F1/F0, 322 against F3/F4, 323 against G0, 324 against G4) and
`make test-ndebug` (322 / 323 / 324 / 325, asserts off).

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

A ready-made consumer lives in `tests/integration/platformio/` — copy that
directory as a starting point. CI does not run PlatformIO itself (it would mean
downloading the `ststm32` platform and the STM32Cube framework for every run);
what CI checks instead is that `library.json` is self-consistent and that the
same consumer compiles with the include set the manifest itself declares, via
`make test-manifest`. See the comments in `platformio.ini` for exactly what that
does and does not cover.

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

Keep the build directory and any install prefix inside `build/`
(`cmake --install build --prefix build/prefix`), as the `ci.yml` `cmake` job
does. `build/` is ignored and `make clean` removes it; a `--prefix` at the
repository root works too but leaves a directory that `make clean` has to glob
for.

Add `-DOW_BUILD_EXAMPLES=ON` to build the seven example applications. They are
built with the same per-example `UART_TX_BUF_SIZE` and, for `6_statistics`, the
same `OW_STATS_ENABLE` as `make APP=<ex>`, and
each one also gets a `.hex` and a `.bin` next to the executable in
`build/examples/`.

`-DOW_EXTRA_DEFINES` is the CMake counterpart of the Makefile's `EXT="..."`: it
applies extra definitions to every library and example, so the same firmware
variants can be selected without a dedicated option each.

```bash
cmake -DCMAKE_TOOLCHAIN_FILE=cmake/arm-none-eabi-gcc.cmake \
      -DOW_TARGET=f4 -DOW_BUILD_EXAMPLES=ON \
      -DOW_EXTRA_DEFINES="-DOW_PARASITE_POWER=1;-DOW_PORT_LOW_POWER=1" \
      -B build .
cmake --build build
```

`CMAKE_BUILD_TYPE` defaults to `Release` when a single-config generator leaves
it empty, and the profiles are the Makefile's own rather than CMake's defaults:
`Release` is `-Os -flto -g0`, `Debug` is `-Og -g3 -gdwarf`. LTO is on for every
family: the old Cortex-M0/M0+ exclusion (GCC 14's thin-LTO partitioner failed
there) was removed after the F0/G0 firmware linked cleanly under Arm GNU
Toolchain 15.2. When LTO is on
the build points the archiver at `arm-none-eabi-gcc-ar`, because a plain `ar`
leaves the LTO symbols out of the archive index and the link then fails with
undefined references to the driver API. CMake's own `Release` (`-O3 -DNDEBUG`)
is deliberately not used — `-O3` is not what the hardware numbers were taken
with, and `NDEBUG` is a separate mode in this project.

Select the MCU family with `-DOW_TARGET=f1` (default), `f0`, `f3`, `g0`, `f4` or `g4`, and
the part within it with `-DOW_CHIP=<part>` (e.g. `-DOW_TARGET=f4
-DOW_CHIP=f401xc`, default per family, `-DOW_SYSCLK_MHZ=N` to override the
part's clock). CMake reads the same `chips/<part>.mk` the Makefile build does,
so the two cannot disagree about the device macro, startup file, linker script
or clock.

In a downstream project:

```cmake
FetchContent_Declare(stm32_1wire
    GIT_REPOSITORY https://github.com/a5021/stm32-async-1wire.git
    GIT_TAG        v2.2.0
)
FetchContent_MakeAvailable(stm32_1wire)
target_link_libraries(your_app PRIVATE stm32_async_1wire)
```

This is the path to prefer: the library brings its own CMSIS through
FetchContent, so nothing has to be downloaded or configured first. Build the
library as a subproject, the target name is un-namespaced
(`stm32_async_1wire`), not the `stm32_async_1wire::` form below.

### CMake (`find_package`)

The alternative, when you would rather consume a fixed install tree than vendor
the sources. Install once with the same toolchain, then point `find_package` at
the prefix:

```bash
cmake -DCMAKE_TOOLCHAIN_FILE=cmake/arm-none-eabi-gcc.cmake \
      -DOW_TARGET=f4 -DCMAKE_BUILD_TYPE=Release -B build .
cmake --build build
cmake --install build --prefix build/prefix
```

```cmake
find_package(stm32_async_1wire REQUIRED)
target_link_libraries(your_app PRIVATE stm32_async_1wire::stm32_async_1wire)
```

Two things to know, both consequences of how a package can be built:

- **The consumer provides CMSIS.** The include paths the library compiled
  against were FetchContent checkouts inside its build tree; baking them into
  the install export would tie the package to the machine that built it. So
  add your own CMSIS `core` and device include directories on top of the
  imported target — from STM32Cube, PlatformIO, or a manual checkout.
- **The package carries its part selection.** `OW_CHIP` and `OW_TARGET` are
  baked into the installed target as public definitions (`STM32F407xx`,
  `OW_PORT_TARGET_F4`, `OW_PORT_SYSCLK_MHZ=168`), because `ow_port.h` picks its
  backend from them and `#error`s without them. Build the package for the part
  your firmware is for; there is no way to change it afterwards.

Both integration paths are built by CI against this repository on every change
for all six families (`tests/integration/cmake/`), so neither can rot the way
the install-tree file checks alone would not catch.

### STM32CubeIDE

1.  Run `make download-deps` once to fetch CMSIS headers.
2.  In STM32CubeIDE: **File → New → STM32 Project from an Existing Makefile**.
3.  Point to the repository root directory.
4.  The IDE auto-generates the CDT project; select your MCU target
    (e.g. STM32F103C8).
5.  Build and flash as usual.

### Configuration Notes

-   **Target Name:** The firmware target name is `ds18b20_$(APP)` (e.g. `ds18b20_1_basic`).

-   **Build Directory:** Default is `build/`.

-   **Optimization Level:**

    -   **Release:** `-Os -flto -g0` (default).
    -   **Debug:** `-Og -g3 -gdwarf`.

-   **MCU Flags:** `STM32F103xB` (Cortex-M3) by default; `STM32F030x6`
    (Cortex-M0) with `OW_TARGET=f0`; `STM32G031xx` (Cortex-M0+) with
    `OW_TARGET=g0`; `STM32F303xC` (Cortex-M4) with `OW_TARGET=f3`;
    `STM32F407xx` (Cortex-M4) with `OW_TARGET=f4`,
    `STM32F401xC` with `OW_TARGET=f4 OW_CHIP=f401xc`, `STM32F401xE` with
    `OW_TARGET=f4 OW_CHIP=f401xe` or `STM32F446xx` with `OW_TARGET=f4
    OW_CHIP=f446xx`.

-   **Target Selection:** `make OW_TARGET=f0` builds for the STM32F0 backend
    (48MHz default clock, `port/stm32f0/STM32F030X6_FLASH.ld`),
    `make OW_TARGET=f3` for the STM32F3 backend (72MHz default clock,
    `port/stm32f3/STM32F303XC_FLASH.ld`),
    `make OW_TARGET=g0` for the STM32G0 backend (64MHz default clock,
    `port/stm32g0/STM32G031X6_FLASH.ld`), `make OW_TARGET=f4` for the STM32F4
    backend (168MHz default clock, `port/stm32f4/STM32F407VGT6_FLASH.ld`),
    `make OW_TARGET=f4 OW_CHIP=f401xc` for the F401CC variant of the same backend
    (84MHz default clock, `port/stm32f4/STM32F401CC_FLASH.ld`) and
    `make OW_TARGET=f4 OW_CHIP=f401xe` for the 512KB/128KB xE parts
    (`port/stm32f4/STM32F401RE_FLASH.ld`, same 84MHz default), and
    `make OW_TARGET=f4 OW_CHIP=f446xx` for the STM32F446
    (180MHz default clock with the over-drive sequence,
    `port/stm32f4/STM32F446RE_FLASH.ld`). Each part's
    identity lives in its own `chips/<part>.mk`. The default
    target is STM32F103 (bus on PA10 for F1/F0/F3/F4, logical PA10 via PA12 remap
    for G0).

-   **8MHz RC Build:** On the STM32F103 target the firmware runs on HSE 8MHz
    + PLL ×9 = 72MHz by default. Pass `SYSCLK_MHZ=8` to use the internal RC
    oscillator (HSI) at 8MHz without an external crystal or PLL:

    ```bash
    make SYSCLK_MHZ=8
    make debug SYSCLK_MHZ=8
    ```

    On the STM32F030 target the default clock is already HSI+PLL (48MHz);
    there `SYSCLK_MHZ=8` selects the raw 8MHz HSI instead. On the STM32F303
    the default is the 8MHz HSE-bypass signal ×9 = 72MHz, and `SYSCLK_MHZ=8`
    selects the raw 8MHz HSI (64MHz is the HSI ceiling there). On F4 the
    default is HSE+PLL (168/84/180MHz per part), and `SYSCLK_MHZ=16` selects
    the raw 16MHz HSI.

    The knob maps to the portable `OW_PORT_SYSCLK_MHZ` define — a single
    value in MHz that every clock-dependent setting derives from: the
    timer prescaler, the input-capture filter and the USART baud rate
    adjust automatically. Useful for testing on bare minimum hardware
    (no HSE crystal).

### Compile-time Tunables (`inc/ow_config.h`)

All genuinely tunable build constants live in `inc/ow_config.h`.  Every
macro carries a `#ifndef` guard so that a `-D` on the command line (Makefile
EXT, PlatformIO `build_flags`) overrides the default without editing the
header.  (These are preprocessor macros, not CMake options: a `cmake
-DOW_STATS_ENABLE=1` variable would not reach the compiler.  The CMake
example build enables stats automatically for `6_statistics` — see below.)
Protocol-inherent values (`ONEWIRE_MAX_SLOTS`,
`DS18B20_RES_MIN/MAX/DEFAULT`) and the per-family system clock default
(`OW_PORT_SYSCLK_MHZ`) remain in their respective headers and are NOT
listed here.

The four feature flags use **value style**: define to **1** to enable,
omit or set to 0 to disable.  Old presence-only style
(`-DOW_PORT_LOW_POWER` without `=1`) no longer compiles correctly.

| Macro | Default | Notes |
|-------|---------|-------|
| `ONEWIRE_ONE_PULSE` | 5 | '1'-bit duration in µs — HW-validated on every clock |
| `ONEWIRE_ZERO_PULSE` | 60 | '0'-bit duration in µs |
| `ONEWIRE_GUARD_BAND` | 5 (100 when `OW_PARASITE_POWER=1`) | Slot release margin in µs |
| `ONEWIRE_SHORT_PULSE_MAX` | 10 | Short-pulse detection window in µs |
| `OW_PARASITE_POWER` | 0 | 1 = set parasite timing defaults (guard band) |
| `OW_PORT_LOW_POWER` | 0 | 1 = enable opt-in WFE sleep path |
| `OW_DRIVE_ACTIVE` | 0 | 1 = enable push-pull write path |
| `OW_STATS_ENABLE` | 0 | 1 = compile in per-sensor pulse statistics |
| `OW_BUS_DRIVE` | 3 (`OW_BUS_DRIVE_MAX`) | Drive strength of the bus pin: 0 = `WEAK`, 1 = `MEDIUM`, 2 = `STRONG`, 3 = `MAX`. `MAX` is the default because the parasite strong pull-up sources the whole fleet from this pad; F1 has no `OSPEEDR` register, so `MAX` clamps to `STRONG` there. |
| `DS18B20_MAX_DEVICES` | 8 | Max devices in the device table (8 B each) — single-sensor builds can set 1 and save 56 B of `.bss` |

**Override examples**

```bash
make                              # defaults, all flags =0
make EXT="-DOW_PORT_LOW_POWER=1"  # enable WFE sleep path
make EXT="-DOW_PARASITE_POWER=1"  # parasite guard-band default
make EXT="-DONEWIRE_SHORT_PULSE_MAX=15 -DDS18B20_MAX_DEVICES=16"
```

CMake: the `OW_STATS_ENABLE` flag is not a CMake option.  With
`-DOW_BUILD_EXAMPLES=ON` it is applied automatically to
the `6_statistics` example only (its dedicated library variant), mirroring
`make APP=6_statistics`:

```bash
cmake -DOW_TARGET=f0 -DOW_BUILD_EXAMPLES=ON -B build .
cmake --build build --target 6_statistics
```

Other CMake feature flags can be forwarded to the compiler via a standard
`CMAKE_C_FLAGS` (or a toolchain-file edit) — the `#ifndef` guard in
`ow_config.h` picks them up:

PlatformIO (`platformio.ini`):

```ini
build_flags = -DOW_PORT_LOW_POWER=1 -DOW_STATS_ENABLE=1
```

Feature flags can also be set via a Makefile knob (no EXT needed):

```bash
make OW_DRIVE_ACTIVE=1        # → -DOW_DRIVE_ACTIVE=1
```

## VSCode Integration

The repository includes `.vscode/` workspace configuration for a
convenient development workflow.

### Building

Press **Ctrl+Shift+B** to run the default build task (`make`). Other
tasks are available via **Ctrl+Shift+P** → "Tasks: Run Task":

- `Build (release)` — `make` (default)
- `Build (debug)` — `make debug`
- `Build F0 (debug)` — `make OW_TARGET=f0 debug` (debug build for the STM32F030 target)
- `Build F3 (debug)` — `make OW_TARGET=f3 debug` (debug build for the STM32F3 target)
- `Build G0 (debug)` — `make OW_TARGET=g0 debug` (debug build for the STM32G031 target)
- `Build F4 (debug)` — `make OW_TARGET=f4 debug` (debug build for the STM32F4 target)
- `Clean` — `make clean`
- `Program (J-Link)` / `Program (ST-Link)` — flash the device
- `Download dependencies` — `make download-deps`

### Debugging

1. In the **Run and Debug** panel (`Ctrl+Shift+D`), select the debug
   configuration: **"Debug F1 (J-Link)"** / **"Debug F1 (ST-Link)"** for
   the STM32F103 target, **"Debug F0 (J-Link)"** / **"Debug F0 (ST-Link)"**
   for the STM32F030 target, **"Debug F3 (J-Link)"** / **"Debug F3
   (ST-Link)"** for the STM32F303 target, **"Debug G0 (J-Link)"** / **"Debug
   G0 (ST-Link)"** for the STM32G031 target, or **"Debug F4 (J-Link)"** /
   **"Debug F4 (ST-Link)"** for the STM32F4 target. The F0
   configurations build with `OW_TARGET=f0` automatically, the F3 ones with
   `OW_TARGET=f3`, the G0 ones with `OW_TARGET=g0`, the F4 ones with
   `OW_TARGET=f4`.
2. Open `examples/1_basic/main.c` and set a breakpoint in `main()`.
3. Press **F5** — Cortex-Debug will build the firmware in debug mode,
   flash it, run to `main()`, and halt.

The SVD file for the selected family is downloaded by `make download-deps`
and loaded automatically for peripheral register views in the debug
sidebar. Standalone SEGGER Ozone users can open `port/<mcu>/project.jdebug`
from any backend directory; the project resolves its SVD and ELF paths
relative to its own location. The F3 directory holds
`project-f303vc.jdebug` for the F303VC; the F4 directory holds one project per
part — `project.jdebug` for the F407VGT6 default, `project-f401cc.jdebug` for
the F401CC, `project-f401re.jdebug` for the xE parts and
`project-f446re.jdebug` for the F446, since each part has its own device name
and SVD.

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
| **Bit-banging + timer ISR** | Timer interrupt drives GPIO transitions | Semi-blocking | Medium | RTOS-based firmware |
| **UART bit-banging** | UART at 9600/115200 baud emulates 1-Wire timings | Depends | Medium | Systems with spare UARTs |
| **Hardware 1-Wire master** | Dedicated IC (DS2482) or kernel subsystem (Linux w1-gpio) | No | High | Linux SBCs, complex systems |
| **Timer + DMA + One-Pulse Mode** (this driver) | DMA feeds CCR values to the timer, which generates bus timing without CPU-driven edges | No | Hardware-timed, 1 µs timer resolution; no CPU-induced edge jitter | STM32 firmware with available timer and DMA resources |

### Trade-offs

**Cost.** This driver consumes dedicated hardware resources — TIM1 and two DMA
channels (the capture drain from CCR4 and the marker feed into CCR3: DMA1
channels 3/4 on F0/F1/F3/G0/G4 (via DMAMUX on G0/G4), DMA2 streams 2/4 on F4) — that
cannot be used for other purposes. The 1-Wire data line itself occupies one GPIO (PA10), but any approach
needs a GPIO pin for the bus, so that is not an extra cost. Bit-banging
approaches, by contrast, need only that one pin and no DMA, making them more
portable across MCUs with limited peripherals.

**Precision vs. portability.** Timer+DMA keeps the CPU out of the
timing-critical path. The timer schedules bus edges at a nominal 1 µs
resolution, so CPU activity does not introduce edge jitter. Actual timing
accuracy depends on the timer clock and the electrical characteristics of the
bus. Software delays degrade under interrupt load, and even timer-ISR approaches
incur jitter from preemption. The trade-off is complexity: this driver's
hardware configuration is ~150 lines of register-level code versus ~20 lines
for a typical bit-bang implementation.

## Architecture

### Hybrid Hardware Automation

The 1-Wire layer uses a hybrid of several hardware features:

1. Timer-Driven Sequences: TIM1 is configured in One-Pulse Mode (OPM). Each state machine step configures the timer for a specific operation (reset, write byte, read byte, wait) and starts it.
2. DMA for Data Transfer: DMA is used in two key ways:
   - Transmit: Feeds a pre-calculated sequence of Compare Register (CCR) values to TIM1->CCR3 to automatically generate the precise waveform for writing commands or bits. The feed request comes from the CH2 slot-end marker compare and rides DMA1 channel 3 on F0/F1/F3/G0 (DMAMUX request 21 on G0), or DMA2 stream 2 at `CHSEL=6` on F4.
   - Capture: Automatically stores values from the TIM1->CCR4 capture register into memory to record pulse timings during read operations or presence detection; the capture drain rides DMA1 channel 4 on F0/F1/F3/G0 (DMAMUX request 23 on G0), or DMA2 stream 4 on F4.
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
> consistent with the non-blocking measurement path. See `examples/3_round_robin/main.c` for a
> complete Search ROM example.

### Shared 1-Wire Layer

All bus-level protocol lives in `src/onewire.c` (interface in `inc/onewire.h`),
a reusable 1-Wire master that the DS18B20 driver builds on:

- `onewire_init()`, `onewire_reset()`, `onewire_present()`,
   `onewire_write_slots()`, `onewire_encode_byte()`,
   `onewire_write_bit()`, `onewire_read_pair()`, `onewire_write_then_read()`,
   `onewire_pair_bits()`, `onewire_read_data()`, `onewire_decode_pulses()`,
   `onewire_bit_from_pulse()`, `onewire_start_timer()`, `onewire_kick()`,
   `onewire_strong_pullup()`, `onewire_bus_done()` — the
   TIM1/DMA bus primitives.
- `onewire_search_start()`, `onewire_search_poll()`,
  `onewire_search_count()`, `onewire_search_active()` — the generic Maxim
  Search ROM engine, shared by `ds18b20_search_*` and
  `ds18b20_alarm_search_*`.
- `onewire_crc8()` — the Dallas/Maxim CRC-8 utility.

Each bus transaction is timed and executed by TIM1 and DMA, without CPU-driven
timing. The application calls `onewire_bus_done()` / `onewire_search_poll()` to
let the non-blocking state machine check hardware completion and advance to the
next step; no busy-wait is used. Helper functions such as CRC calculation and
pulse encoding/decoding process data in software and do not start bus
transactions. The layer owns its own capture
buffers, keeps the line released to idle HIGH after every transaction, and
is fully covered by the host test suite. See the API Reference below for the
complete `onewire_*` surface.

### Layer Contracts

Dependency direction is app → `ds18b20` → `onewire` → `ow_port` → backend:
each layer uses only the one directly below it. Two past leaks were closed
along this rule: the driver no longer calls `ow_port_*` directly (it goes
through `onewire_bus_done()` / `onewire_long_wait_pending()` /
`onewire_sleep_until_done()`), and the low-power "long stage" flag is gone —
"long" is derived from the scheduled timer
(`CEN && !UIF && ARR*(RCR+1) > OW_PORT_LONG_STAGE_US`, one rule for capture,
feed and timer waits), so multi-slot writes sleep instead of busy-polling.
`bus_done()` retires the schedule explicitly.

The following five look like leaks but are deliberate, reviewed decisions —
do not "fix" them without re-reading the reasoning:

- **Backend selection lives in `onewire.h`.** One chain, one `#include`
  branch, `#error` on no family — so the backend and its defaults cannot
  drift (`8dba9d2`). Header-side selection is the integration contract:
  consumers define `OW_PORT_TARGET_Fx` (or CubeMX defines `STM32F*` itself)
  and the headers pick the backend. Moving selection into the build system
  would break the CubeIDE path.
- **`ow_pulse_t` is family-sized.** F4 feeds CCR3 in DMA direct mode
  (halfword, `uint16_t`); the other families use `uint8_t`. An
  always-`uint16_t` type would double the pulse buffers on 4 KB-RAM parts;
  casts would hide the F4 halfword requirement the backends document.
- **The `onewire.h` ↔ `ow_port.h` include cycle is intentional.** It is
  guard-protected and order-independent (`ow_port.h` stays self-contained
  whatever the TU includes first). Breaking it means either duplicating the
  selection chain or forcing an include order on users.
- **`onewire_strong_pullup()` stays in the generic layer.** It is a thin
  wrapper like every other `onewire_*` primitive, and the strong pull-up is
  a bus-line mode owned by the layer that owns the hardware operations —
  not a sensor concept. Removing it would push the driver back to
  `ow_port_*`.
- **`OW_PORT_MARKER_TOGGLE` is an opt-in hook.** Default no-op, used once
  (merged-pass start marker for a logic-analyzer decoder, see
  `port/stm32f4/HARDWARE-NOTES.md`). Zero cost unless a backend defines it.

Two more closures, for the record: stats emission stays in the driver
because pulse widths, ROM addresses and `DS18B20_TEMP_ERROR_*` codes are
driver-owned concepts the onewire layer must not know (see the note at the
`ow_stats.h` include in `src/ds18b20.c`); the reset-geometry macros stay in
`inc/ow_port.h` because they are the port's own schedule parameters
published as port contract (see the comment there).

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
(`port/stm32*/ow_port_*.h`) pins each backend to one specific timer rather
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
(default AFIO map); on STM32F0, PA10 (AF2); on STM32F3, PA10 (AF6); on
STM32G0, the PA12 pad remapped to logical PA10 via `SYSCFG_CFGR1.PA12_RMP`;
on STM32G4, PA10 (AF6); on STM32F4, PA10 (AF1).

#### 3. DMA

Two DMA channels are required, each carrying a specific peripheral request:

| Channel | Direction | Request | Purpose |
|---|---|---|---|
| DMA channel A | Memory → Peripheral | TIM1_CC2 (CH2 compare) | Feeds CCR3 with the next pulse width on each slot boundary (the "feed" path). |
| DMA channel B | Peripheral → Memory | TIM1_CH4 (capture) | Drains CCR4 capture values into a memory buffer (the "capture" path). |

F0/F1/F3 have a fixed request map (no `DMA_CSELR` mux) — channel 3 = CC2,
channel 4 = CC4, confirmed empirically.  G0 uses a DMAMUX: TIM1_CC2 = request
21, TIM1_CH4 = request 23; G4 the same way: TIM1_CC2 = request 43, TIM1_CH4 =
request 45.  F4 uses DMA2 with the per-stream `CHSEL` field
set to 6 (`TIM1_CH2`/`TIM1_CH4`): stream 2 = feed, stream 4 = capture.  A new
backend must verify the DMA request numbers for its target; the channel roles
are identical across all families.

#### 4. Clocking invariant

  The invariant is not "APB prescaler = /1" but **"the TIM1 kernel clock equals
  SYSCLK"**, which is what makes `PSC = SYSCLK_MHZ - 1` a 1 µs tick.  STM32
  timers double their clock when the APB prescaler feeding them is >1
  (`timer clock = 2 × PCLK`), so each family reaches SYSCLK its own way.  All
  five supported families satisfy the invariant by construction: F1 keeps
  PPRE2=/1 (TIM1 is on APB2, so TIM1 = PCLK2 = SYSCLK); F0 and G0 never touch
  the APB prescaler, so it stays at its reset /1; F3 puts APB2 at /1 while
  taking APB1 down to /2 for the 36 MHz limit; F4 runs PPRE2=/2 at 168, 84
  and 180 MHz and *relies* on the doubling (84→168, 42→84, 90→180), with
  PPRE2=/1 only on the raw-HSI and raw-HSE paths.  See `examples/app/app.c`.
  
  #### 5. GPIO
  
  The bus pin must support alternate-function open-drain (for normal bus
  operation) and runtime switching to alternate-function push-pull (for
  parasite-power strong pull-up and the optional active-drive write path).
  The pin never leaves alternate function — only the output-stage topology
  changes.
  
  #### Supported families (same scheme, different prescaler and pin config)
  
  | Family | Timer | Bus pin | DMA routing | Notes |
  |---|---|---|---|---|
  | STM32F1 | TIM1 | PA10 (default AFIO) | Fixed: CC2→DMA1 ch3 (feeds CCR3), CH4→DMA1 ch4 | APB2=/1 by default |
  | STM32F0 | TIM1 | PA10 (AF2) | Fixed: same mapping | TSSOP20: PA8 not bonded out, CH3/CH4 is the only viable pair |
  | STM32F3 | TIM1 | PA10 (AF6) | Fixed, same pair as F0/F1: CC2→DMA1 **ch3**, CH4→DMA1 **ch4** | RM0316 Table 78; channel 4 also carries USART1_TX and channel 3 USART3_TX, harmless because no UART byte moves by DMA |
  | STM32G0 | TIM1 | PA10 via PA12 remap | DMAMUX: CC2=#21, CH4=#23 | SYSCFG `PA12_RMP`; PA11/PA12 cannot be used as GPIO while driver is active |
| STM32G4 | TIM1 | PA10 (AF6) | DMAMUX: CC2=#43, CH4=#45 | APB2=/1, no x2 doubling; 8-bit feed tables |
  | STM32F4 | TIM1 | PA10 (AF1) | DMA2, CHSEL=6: CC2→stream2 (feeds CCR3), CH4→stream4 | Feed runs 16-bit in direct mode; see `port/stm32f4/HARDWARE-NOTES.md` |
  
  The DMA column is the one thing here that is **not** interchangeable between
  families, and getting it wrong is silent: on a family whose CC2/CH4 requests sit
  on different channels, the feed DMA never fires and captures read back empty
  rather than misreporting. Each backend's channel pair is read out of its own
  reference manual and recorded in the assignment's comment.
  
All six backends share one TIM1/DMA core in `port/common/` (`ow_port_f0.h`,
`ow_port_f1.h`, `ow_port_f3.h`, `ow_port_g0.h`, `ow_port_g4.h`
with DMA1 defaults, `ow_port_f4.h`
as a thin shim overriding the DMA spelling): DMA2's per-stream `CHSEL` mux
travels inside the control-register word, and the merged write+read pass is
shared too, with no per-family arm order left.
  
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
  where the DS18B20 is silent and cannot respond on the bus — and also around the
  Convert T and Read Scratchpad command writes. Deliberately, it **stays engaged
  while the driver is parked at IDLE** so the sensor's capacitors keep charging
  for the next cycle; it is released only where the bus has to be free (at the
  reset pulse, at the scratchpad read and at the second reset).

- **Active-drive write mode (optional).** During master-only write slots the bus
  pin may be placed in alternate-function push-pull, actively driving both HIGH
  and LOW levels instead of relying on the pull-up for HIGH. The driver
  automatically retains open-drain operation for reset, presence, read slots and
  write/read transactions where the slave may drive the bus. This is what makes
  push-pull acceptable on a 1-Wire bus at all: it is confined to phases where no
  slave can answer. Enabled with `-DOW_DRIVE_ACTIVE=1`; the published default
  remains open-drain.

### State Machine Flow (hardware-timed; polled on UIF)

Explicit-start behavior
- The driver measures **only when asked**: after `ds18b20_init()` the bus stays idle, and one `ds18b20_start_measure()` call requests exactly one conversion + scratchpad-read cycle. Afterwards the driver parks at IDLE again — it never restarts a cycle on its own, so the measurement cadence, the retry policy and any idle interval are the application's decision. Nothing else starts a conversion either: finishing a device search, a resolution change or a command transaction leaves the bus idle.

- IDLE (state 0)
  - The driver is parked here. A pending UIF (raised by `ds18b20_start_measure()` via `onewire_kick()`) falls through into START, which turns the busy indicator **on** and arms the bus reset. The data union is not touched here: `capture[]`, `pulse[]` and `scratchpad[]` are three views of one union, and decoding must consume `pulse[]` before writing `scratchpad[]`.
  - Set state=1.

- START (state 1)
  - LED on. Run reset_bus():
    - CH3 issues active-low reset pulse (~480µs within ~960µs slot).
    - CH4 (indirect input) captures presence timing into ctx.capture[0..1] via DMA from CCR4.
  - Set state=2.

- CONVERT (state 2)
  - On UIF, check_presence() with ctx.capture[].
    - If present: send convert command via CH3+DMA. With no selected device,
      this is "Skip ROM 0xCC + Convert T 0x44" (16 slots, RCR=15). With a
      device selected via ds18b20_select(), it is "Match ROM 0x55 + 8-byte ROM
      + Convert T 0x44" (80 slots, RCR=79). Set state=3.
    - Else: report NO_SENSOR; set state=0 (parked until the next ds18b20_start_measure()).

- WAIT (state 3)
  - On UIF: wait_conversion() schedules exactly the conversion time of the
    configured resolution `ctx.resolution` (9-bit: 10×9.375ms; 10-bit:
    10×18.75ms; 11-bit: 20×18.75ms; 12-bit: 12×62.5ms = 750ms). The
    resolution is auto-derived from each valid scratchpad read and updated
    by `ds18b20_set_resolution()`; set state=4.

- CONTINUE (state 4)
  - On UIF: run reset_bus() again; set state=5.

- REQUEST (state 5)
  - On UIF, check_presence().
    - If present: send read command via CH3+DMA. With no selected device,
      this is "Skip ROM 0xCC + Read Scratchpad 0xBE" (16 slots). With a device
      selected, it is "Match ROM 0x55 + 8-byte ROM + Read Scratchpad 0xBE"
      (80 slots). Set state=6.
    - Else: report NO_SENSOR; set state=0 (parked until the next ds18b20_start_measure()).

- READ (state 6)
  - On UIF: read_data() schedules 72 slots (RCR=71; ARR=70µs). CH3 emits ~5µs active-low kick at each slot start and then releases; CH4 captures sensor pulse timing; DMA fills ctx.pulse[72]. Set state=7.

- DECODE (state 7)
  - On UIF: decode_scratchpad() from ctx.pulse[] into ctx.scratchpad[], LED off; reject an all-0xFF frame from an absent addressed device (`NO_SENSOR`), verify the reserved scratchpad bytes (byte 5 = 0xFF, byte 7 = 0x10), then verify CRC; report temperature or CRC_FAIL; set state=0 (parked until the next ds18b20_start_measure()).

## RTOS Integration

### Bus Idle Behaviour

Relevant if `ds18b20_search_poll()` / `ds18b20_poll()` are called
from an RTOS task and scheduling delays can land between 1-Wire slots: what the
DS18B20 datasheet allows, what the bench measured, and what the suite covers.

- **Idle-HIGH is harmless.** The datasheet states the 1-Wire bus must be left
  in the inactive (high) state when suspending a transaction and that
  *"infinite recovery time can occur between bits so long as the bus is in the
  inactive (high) state during the recovery period."* The DS18B20 re-synchronises
  to the next falling edge; it has no internal timeout that expires during an
  idle-high pause.
- **Measured on a real bus:** injecting idle-high gaps of 10 µs … 5 ms between
  Search ROM slots finds all 5 devices in 100/100 runs at every gap size (bench
  experiment, quoted from the earlier hardware campaign).
- **Idle-LOW > 480 µs resets all devices** (datasheet: *"if the bus is left low
  for more than 480 µs, all components on the bus will be reset"*). This is the
  only real hazard.

Practical consequences for RTOS use:

1. A delayed poll is safe **because the line is released to HIGH in hardware**,
   not by software. Every bus operation now ends with the line idle-HIGH
   automatically: the CCR3-fed writes (`ow_port_feed`, including the merged
   search op) append a trailing 0 to the DMA feed, and the direct-write/capture
   operations (reset, read, single-slot write) use an OC3PE preload of 0 — both
   applied at the instant the one-pulse timer stops. Those `T1.CCR3 = 0`
   preloads are the only software CCR3 writes on the direct paths, latched at
   the stopping update event; the bus cannot be left LOW by a stale compare
   value, no matter how long the RTOS delays the next poll.
2. The usable scheduling latency budget is ~480 µs of LOW, not a tight
   microsecond window. Any RTOS that resumes the poll within hundreds of µs is
   fine; longer delays only require that the bus idles HIGH, which the hardware
   release guarantees by construction.

### Multithreading and Concurrency

The driver is safe to call from an RTOS task, but it is **not re-entrant and
not thread-safe by itself**. All driver state is global and shared: the DS18B20
state machine, the 1-Wire search context (`search_ctx`, in `onewire.c`), and
the TIM1/DMA peripherals (DMA1 on F0/F1/F3/G0, DMA2 on F4). There is no internal lock. The ownership guards
(`txn_can_start`, the `ds18b20_search_*` checks) only prevent *logical*
conflicts within a single-threaded model — they are plain flag checks, not
atomic across tasks.

Rules for correct RTOS use:

1. **Confine every `ds18b20_*` call to a single task, or serialise with a
   lock.** Either drive the whole driver from one task (the same one that calls
   `ds18b20_poll()`), or wrap every entry point — `ds18b20_start_measure()`,
   each `*_start`, each `*_poll`, `ds18b20_select()`, `ds18b20_scan_start()`,
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

2. **TIM1, the driver's DMA feed/capture pair and the bus GPIO pin are owned
   exclusively by the driver from initialisation until reset.** No other task,
   ISR or peripheral may configure or use them:

   - **TIM1** — the prescaler, pulse-generation/CCR3-feed logic and input
     capture on CCR4 are all driver-managed.
    - **The DMA pair** — DMA1 channels 3 (feeds CCR3) and 4 (drains CCR4) on
      F0/F1/F3, the same two channels via DMAMUX requests 21 (TIM1_CC2) and 23
      (TIM1_CH4) on G0, requests 43 and 45 on G4, and **DMA2 streams 2 and 4
      at `CHSEL=6`** on F4.
    - **The 1-Wire data pin** — PA10 as alternate-function open-drain, or
      the physical PA12 pad remapped to logical PA10 on G0 (those pads must
      not be used as plain GPIO while the driver is active).

   The library has no deinit or release API — the exclusivity begins at
   `onewire_init()` / `ds18b20_init()` and lasts until reset. Configure any of
   these resources before initialising the driver, never after.

3. **Poll cadence vs latency.** Because the 1-Wire bit timing is generated
   entirely by hardware, `ds18b20_poll()` may be called at any rate — slow
   polling only increases latency, never causes errors (see *Bus Idle
   Behaviour*). The one exception is a build with `OW_PORT_LOW_POWER=1`, where
   that same call blocks in `WFE()` for the duration of a long stage — see
   [Examples](#examples) (`7_low_power`, and the low-power block there). Two
   practical patterns:
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
Initialize the DS18B20 driver. Enables the peripherals the backend needs (the bus
GPIO port, TIM1 and the DMA controller — DMA1 on F0/F1/F3/G0, DMA2 on F4) and sets
up the timer prescaler for 1µs resolution. System clock configuration is handled separately in the application (see `app.c`). This function does NOT start a measurement.

```C
void ds18b20_start_measure(void);
```
Request exactly one measurement cycle (broadcast `Convert T` + scratchpad read) on the currently selected device, or on every discovered device in scan mode. The result is reported through `ds18b20_complete()`. When the cycle finishes the driver parks at IDLE and waits for the next request — it never starts a cycle on its own, so the measurement cadence, retries and idle intervals are entirely the application's decision. Ignored (no-op) while a measurement cycle, a device search, a resolution change or a command transaction owns the bus.

```C
void ds18b20_poll(void);
```
The Core Driver Function: Must be called from the main loop. It checks the Timer Update Flag (UIF). If the flag is set, it means the hardware has finished the previous operation (e.g., sending a command, waiting for conversion). The function then clears the flag and advances the internal state machine to the next step. The driver's state is persistent, so this function can be called at any rate without risk of getting stuck. At IDLE it does nothing until `ds18b20_start_measure()` requests a cycle.

### 1-Wire Layer (shared)

The 1-Wire bus primitives and the Search ROM engine are **not** part of the
driver — they live in the shared 1-Wire layer (`inc/onewire.h` +
`src/onewire.c`) that `src/ds18b20.c` is built on. Full interface:

```C
void        onewire_init(void);
uint8_t     onewire_bus_done(void);
void        onewire_reset(volatile uint16_t *reset_pulses);
uint8_t     onewire_present(const volatile uint16_t *pulses);
uint8_t     onewire_write_slots(const ow_pulse_t *pulses, uint16_t slots);
uint8_t     onewire_write_bit(uint8_t bit);
void        onewire_read_pair(volatile uint16_t *pair_pulses);
void        onewire_write_then_read(uint8_t bit);
void        onewire_pair_bits(const volatile uint16_t *pair_pulses,
                              uint8_t *id_bit, uint8_t *cmp_bit);
uint8_t     onewire_read_data(volatile uint8_t *dst, uint8_t bytes);
void        onewire_decode_pulses(uint8_t *dst, const volatile uint8_t *pulse,
                                 uint8_t nbytes);
void        onewire_encode_byte(ow_pulse_t *out, uint8_t byte);
static inline uint8_t onewire_bit_from_pulse(uint16_t dur);
void        onewire_start_timer(uint16_t arr, uint8_t rcr);
void        onewire_kick(void);
void        onewire_strong_pullup(uint8_t on);
uint8_t     onewire_crc8(const uint8_t *data, uint8_t len);
void        onewire_search_start(onewire_search_sink_t sink,
                                 uint8_t max_devices, uint8_t command,
                                 uint8_t family);
uint8_t     onewire_search_poll(void);
uint8_t     onewire_search_count(void);
uint8_t     onewire_search_active(void);
```

`onewire_write_slots()` and `onewire_read_data()` report whether the operation
was scheduled: they return **1** on success and **0** when the size argument is
out of range and nothing was started (`slots` must be 1..256, `bytes` 1..32).
Debug builds additionally trap the reject path with `assert`; with `NDEBUG` the
caller observes the 0 instead of a silent no-op, so an invalid size can never
turn into an undiscovered `onewire_bus_done()` hang. `onewire_write_bit()` has
no size argument and always returns 1.

There is no byte-oriented command helper: a command is encoded by the caller
with `onewire_encode_byte()` into a slot buffer and then handed to
`onewire_write_slots()`, so the caller owns the buffer geometry. The DS18B20
driver builds its command slots the same way (`src/ds18b20_measure.c`,
`src/ds18b20_txn.c`, `src/ds18b20_resolution.c`). A multi-slot write buffer must
hold `slots + 1` entries, the last one equal to `ONEWIRE_RELEASE_PULSE` — the
hardware bus-release sentinel, not an extra slot (a single-slot write uses a
separate path and reads only entry 0). `onewire_encode_byte()` and the
`ow_pulse_t` type are declared in `inc/onewire.h` alongside the rest of the
layer; `ow_pulse_t` is 8 bits wide on F0/F1/F3/G0 and 16 bits on F4.

#### Timing

Slot timing is fixed at **compile time** (`inc/ow_config.h`): four `ONEWIRE_*`
macros define the bit-slot geometry, and every bus operation is scheduled with
those exact durations. There is no runtime profile switching and no timing
state — the values fold into the TIM1/DMA register arithmetic and can be
overridden individually via `-D`.

```C
#define ONEWIRE_ONE_PULSE       5   /* µs: short low = write-1 / read pulse       */
#define ONEWIRE_ZERO_PULSE     60   /* µs: long low = write-0                     */
#define ONEWIRE_GUARD_BAND      5   /* µs: slot tail (external-power default)     */
#define ONEWIRE_SHORT_PULSE_MAX 10  /* µs: pulse <= this decodes as bit '1'       */
void onewire_strong_pullup(uint8_t on);  /* parasite power: drive bus HIGH */
```

On a parasite-powered bus the release margin must be wider: define
`OW_PARASITE_POWER` as 1 (`-DOW_PARASITE_POWER=1`) to select the 100µs guard
band default, or pass `-DONEWIRE_GUARD_BAND=...` explicitly. The Makefile
presets in [Configuration → Timing](#timing-1) select whole value sets.

Any other 1-Wire slave driver can use the same layer. The DS18B20 driver calls
`onewire_init()` from `ds18b20_init()` and keeps the layer's Search ROM engine
for its own `ds18b20_search_*` / `ds18b20_alarm_search_*` wrappers.

### CRC Utility

```C
uint8_t ds18b20_crc8(const uint8_t *data, uint8_t len);
```
Calculates the Dallas/Maxim CRC-8 used by the driver to validate ROM codes and
scratchpad data. Exposed publicly as a small utility (e.g., for host tools).
It is a thin DS18B20-namespaced wrapper around the shared `onewire_crc8()`
(see [1-Wire Layer](#1-wire-layer-shared)); both compute the same checksum.

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
`ds18b20_search_poll()` from the main loop until it returns 1. The measurement
state machine is never left IDLE, so `ds18b20_start_measure()` works immediately
afterwards; the timer stays idle in the meantime. `ds18b20_search_count()` returns how many
devices were found. Only devices with family code `DS18B20_FAMILY_CODE` (0x28)
are reported. See `examples/3_round_robin/main.c`.

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

`ds18b20_select()` targets one device and does not automatically cycle through
the devices found by a search. For round-robin measurements, the application
stores the ROM addresses returned by the search and selects the next device
between measurement cycles; see `examples/3_round_robin/main.c`.

To convert all discovered devices in parallel, use the separate scan mode:
`ds18b20_scan_start()` broadcasts one Convert T command, then reads each device
by ROM address and reports results in device-table order. See
`examples/4_scan_mode/main.c`. Scan mode and single-device selection are
mutually exclusive.

### Command Transactions

The remaining DS18B20 commands run with the same non-blocking discipline as the
device search and the resolution change: each transaction owns TIM1/DMA while
it runs (reset → presence → write → read | timed wait) and leaves the bus idle
when it finishes.

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
  the matching `*_poll()` from the main loop until it returns 1, then call
  `ds18b20_start_measure()` when you want a measurement again. Commands are
  ignored mid-cycle, while a device search, an
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
   before the next conversion (see `examples/5_commands/main.c`, which chains recall → scratchpad
   read for this reason).
- `ds18b20_detect_parasite()` runs a Read Power Supply query and stores the
   answer straight into the driver state — after
  `ds18b20_detect_parasite_poll()` returns 1 (check `ds18b20_last_command_ok()`)
  the wiring is configured and `ds18b20_parasite_mode()` reports it. On a mixed
  bus the open-drain answer is a wired-AND: any externally powered device masks
  the parasite report, so detect per-device in Match ROM addressing mode for
  heterogeneous wiring.
- `ds18b20_set_parasite(1)` enables parasite-power support (default 0 —
  external VDD wiring, no pin mode changes). The strong pull-up is harmless
  for externally powered devices, so a mixed bus works with the flag set —
  mechanism and wiring budget in [Parasite Power](#parasite-power).
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

Convert every discovered device in parallel. `ds18b20_scan_start()` switches the
driver into scan mode, then `ds18b20_start_measure()` runs one round: a
broadcast `Convert T` (Skip ROM 0xCC) so all sensors convert simultaneously,
followed by reading each one back via Match ROM in device-table order, with
every result reported through `ds18b20_complete()`. N devices take one
conversion wait plus N reads instead of N conversion waits. A missing device
reports `DS18B20_TEMP_ERROR_NO_SENSOR` and the scan continues. See
`examples/4_scan_mode/main.c`.

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
ds18b20_scan_start();     // enter scan mode (once)
ds18b20_start_measure();   // request one simultaneous-conversion round
while (1) {
    ds18b20_poll();     // scan reports each device via ds18b20_complete()
    // ... when the next round is due, call ds18b20_start_measure() again ...
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
hold-off window (`ds18b20_set_parasite(1)`). It releases the pin back to the
passive pull-up only where the bus has to be free — the reset pulses and the
scratchpad read — and deliberately keeps it engaged while parked at IDLE.

```C
ds18b20_init();
ds18b20_detect_parasite();          // query the wiring (0xCC + Read Power Supply)
while (!ds18b20_detect_parasite_poll()) {
    /* the transaction owns TIM1/DMA: ds18b20_poll() returns immediately by
       design, so only ds18b20_detect_parasite_poll() advances it */
}
/* ds18b20_parasite_mode() now reflects the detected wiring */
```

Wiring guidance, from bench validation on an STM32F103 with a 2.2kΩ pull-up —
deliberately stronger than the 4.7kΩ recommended for externally powered buses
(see [Hardware Connections](#hardware-connections)), because on a parasite bus
the pull-up must also source the conversion current:

- A handful of sensors on short wires (<30cm) converts reliably on the MCU pin
  alone: a six-device broadcast cycle completed without a single CRC error.
- Budget ~1.5mA per simultaneously converting sensor against the pin's drive
  capability (~25mA source on F1/F0/F3/G0/F4/G4) and the VOH droop across your pull-up
  arrangement; keep the bus HIGH above the DS18B20's ~2.96V minimum.
- For longer buses or larger fleets add an external P-MOSFET (or a dedicated
  strong pull-up IC) as the high-side switch and treat the MCU pin as its gate
  driver — the software interface stays exactly the same.

The example applications accept a compile-time flag to run over parasite
wiring out of the box:

```sh
make APP=1_basic EXT="-DOW_PARASITE_POWER=1"        # all seven examples gate ds18b20_set_parasite(1) on it
```

### Signal Statistics Module (`ow_stats`)

An optional compile-in module that collects per-sensor pulse-width statistics
and a global histogram across measurement cycles.  Enabled by defining
`OW_STATS_ENABLE=1` at build time.  When the macro is 0, every inline
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
  `OW_STATS_HIST_BUCKETS` array slots are populated, indices 0–12;
  buckets 0–2, 3–4, 5–6, 7–9, 10–12, 13–14, 15–19, 20–24, 25–29,
  30–39, 40–49, 50–59, 60+ µs) and per-sensor min/max pulse counters.  Called
  automatically from `ds18b20.c` when `OW_STATS_ENABLE=1` is set.
- `ow_stats_count_error()` — record an error event: a CRC or reserved-byte
  mismatch, or an addressed device that answered with an all-0xFF frame
  (reported as `NO_SENSOR`).  A device that fails the presence check after a
  reset is reported through `ds18b20_complete()` but is **not** counted here.
  Called automatically from the DECODE state of `ds18b20.c`.
- `ow_stats_dump_start()` — begin a non-blocking UART dump.  Call from the
  main loop after the desired number of cycles (tracked via `ow_stats_tick()`).
- `ow_stats_dump_poll()` — advance the dump by one line (header, sensor line,
  histogram or total).  Non-blocking: each call enqueues one line into the
  UART TX ring buffer; the main loop drains it via `uart_poll_tx()`.  Returns
  1 when the dump is complete.
- `ow_stats_reset()` — zero all counters and the histogram, keep the sensor
  ROM table.  Call after `ow_stats_dump_poll()` returns 1.
- `ow_stats_tick()` — increment the cycle counter; returns the new value.  The
  module never increments it by itself: `t=` in a dump line is whatever the
  application counts with this call (the `6_statistics` demo ticks once per
  reported measurement, so there it is a count of measurements, not of dumps).

The dump also needs five weak output hooks the application provides as strong
definitions: `ow_stats_putchar()`, `ow_stats_puts()`, `ow_stats_print_int()`,
`ow_stats_print_hex()` and `ow_stats_tx_enqueue()`.  Defaults in `ow_stats.c`
are no-ops, so without them the dump stays silent; the shared example layer
`examples/app/app.c` (linked into every example) implements them on top of the
UART TX ring buffer.

RAM cost: ~300 bytes (8 sensors × 28 B + 16-entry `uint32_t` histogram [64 B] +
cycle/error counters + 8 B dump state; 13 of the 16 histogram buckets, indices
0–12, are populated).

Example — dump every `STATS_DUMP_SWEEPS` sweeps (default 10). A sweep is
one pass over every sensor, so the period is a count of passes rather than
of individual measurements, and it reads the same on a one-sensor bus as on
a seven-sensor one:

```C
#include "ow_stats.h"

#ifndef STATS_DUMP_SWEEPS
#define STATS_DUMP_SWEEPS 10u
#endif

static uint8_t dump_busy = 0;
static uint32_t sweep_count = 0;

void ds18b20_complete(int16_t temp) {
    // ... handle temperature reading ...
    // ... advance to the next sensor, wrapping back to index 0 ...
    if (select_index == 0) { /* one sweep completed */
        sweep_count++;
    }
    (void)ow_stats_tick(); /* keeps total_cycles a count of measurements */
    if (sweep_count >= STATS_DUMP_SWEEPS && !dump_busy) {
        ow_stats_dump_start();
        dump_busy = 1;
    }
}

int main(void) {
    ow_stats_init();
    // ... ds18b20_init(), device search, ds18b20_start_measure() ...
    for (;;) {
        if (dump_busy) {
            if (ow_stats_dump_poll()) {
                dump_busy = 0;
                ow_stats_reset();
                ds18b20_start_measure(); // resume measuring with a clean window
            }
        } else {
            ds18b20_poll();
        }
    }
}
```

Build with the statistics module:

```sh
make APP=6_statistics                                  # external power (OW_STATS_ENABLE auto-added)
make APP=6_statistics EXT="-DOW_PARASITE_POWER=1"            # parasite power
```

> Note: the `6_statistics` target injects `-DOW_STATS_ENABLE=1` and nothing
> else. The period is the example's own `STATS_DUMP_SWEEPS`, defined once in
> `examples/6_statistics/main.c`, so it is overridable with
> `EXT="-DSTATS_DUMP_SWEEPS=N"`. The demo needs no inter-measurement pause: it
> requests the next cycle as soon as the dump is done, so the conversion time
> itself is the cadence and how long a batch takes depends on how many sensors
> are on the bus. A `---` separator marks each sweep, so the wait is visible
> rather than silent.

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

Fields per sensor line: `ROM:min-max n=count e=errors` (errors = CRC +
no-sensor + other combined).  Histogram shows only non-empty buckets;
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
  then call `ds18b20_start_measure()` for the next cycle. That measurement waits
  exactly as long as
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
/* ds18b20_get_resolution() == 9; the next ds18b20_start_measure() uses the
   fast wait */
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

- `DS18B20_TEMP_ERROR_NO_SENSOR`: No sensor detected on the bus.
- `DS18B20_TEMP_ERROR_CRC_FAIL`: Data corruption detected via CRC mismatch.
- `DS18B20_TEMP_ERROR_GENERIC`: Unspecified communication error.

## Performance

- Time to result (one measurement): 93.75ms @ 9-bit … ~0.76 s @ 12-bit
  (conversion + protocol overhead; the conversion wait follows the configured
  resolution, see `ds18b20_set_resolution()`)
- Measurement cadence: **application-defined**. The driver measures one cycle per
  `ds18b20_start_measure()` and then parks, so it has no inter-measurement
  interval of its own; the examples pace themselves (5 s, see
  `MEASURE_PERIOD_MS` in `examples/*/main.c` — except `6_statistics`, which runs
  with no time base at all and paces itself with the conversion time — and
  `app_time_init()` in the
  shared app layer)
- Precision: 0.1°C reported (API tenths; the sensor step at 12-bit is
  0.0625°C — coarser steps at lower resolutions)
- Accuracy: ±0.5°C (typical)
- CPU Usage: Minimal; CPU is free to perform other tasks during waits.

## Configuration

### Part selection: families and parts

Two build knobs, and the split between them is the whole point:

- `OW_TARGET` picks the **family** (`f1`, `f0`, `f3`, `g0`, `f4`). This is where the
  driver code differs: the port header, the timer and DMA resources, the bus
  pin. One `port/<family>/ow_port_<family>.h` per family.
- `OW_CHIP` picks the **part** within the family, by naming a file in
  `chips/`. The parts of one family share a port header — `ow_port_f4.h` has no
  per-part conditional at all, because TIM1/DMA2 and `CHSEL=6` map identically
  across the F4 parts. What actually differs is the CMSIS device layer, the
  linker script (memory sizes), the debugger projects and the default clock,
  so that is exactly what `chips/<part>.mk` holds:

  | Variable | Meaning |
  |---|---|
  | `CHIP_DEV_DEF` | CMSIS device macro (`-DSTM32F401xE`) |
  | `CHIP_DEVICE_HDR` | device header fetched by `make download-deps` |
  | `CHIP_STARTUP` | startup assembly fetched by `make download-deps` |
  | `CHIP_LINKER` | linker script — the memory map is the part's own |
  | `CHIP_JFLASH` / `CHIP_JDEBUG` | J-Flash and SEGGER Ozone projects |
  | `CHIP_SVD` | register view for debuggers |
  | `CHIP_HSE_MHZ` | the board crystal the part file is named after (F3/F4 only), overridable via `HSE_MHZ` |
  | `CHIP_SYSCLK_MHZ` | the part's default clock, overridable via `SYSCLK_MHZ` |

Both build systems read the same file, so they cannot drift on the device macro,
startup file, linker script or clock:

```sh
make OW_TARGET=f4 OW_CHIP=f401xc APP=4_scan_mode   # Makefile
cmake -S . -B build -DOW_TARGET=f4 -DOW_CHIP=f401xc # CMake (same chips/*.mk)
```

Parts are named after the CMSIS device macro, lowercased — `STM32F401xC`
becomes `f401xc`, `STM32F103xB` becomes `f103xb` — so the name says which
flash/pin-density code it is. `make test-chips` (part of `make test-clocks`)
checks that every part's repository-side files exist and that the two scalars
are well formed, and that an unknown family or part is **rejected** rather than
silently falling back to the F1 default. That last part matters: an
unrecognised `OW_TARGET` used to fall through to the F1 branch and produce a
perfectly valid Blue Pill binary.

Two rules when editing a part file: keep it to plain `NAME = value` assignments
(no includes or conditionals — the CMake build parses it), and do not put a
comment at the end of an assignment line, because make keeps the whitespace in
front of `#` as part of the value and every path in it silently stops existing.

### Timing

Slot timing is fixed at **compile time** via the `ONEWIRE_*` macros in
`inc/ow_config.h` (see also the [API Reference → Timing](#timing)). Each macro is
overridable with `-D`, so a specific board or bus length pins its values without
any runtime state. The reset-pulse bounds are defined in `src/onewire.c`:

```C
/* inc/ow_config.h — defaults */
#define ONEWIRE_ONE_PULSE           5     // µs (short low = write-1)
#define ONEWIRE_ZERO_PULSE         60    // µs (long low = write-0)
#define ONEWIRE_GUARD_BAND          5     // µs (built into slot formula)
#define ONEWIRE_SHORT_PULSE_MAX    10    // µs (pulse <= this reads as bit '1')

/* src/onewire.c */
#define RESET_PULSE_MIN           480U    // µs
#define RESET_PULSE_MAX           540U    // µs
```

Slot formula:

```
ARR = one_pulse + zero_pulse + guard_band
```

The Makefile presets set all four values at once (`make TIMING=SLOW`):

| Preset     | `one` | `zero` | `guard` | `short≤` | Slot  |
|------------|------:|-------:|--------:|---------:|------:|
| FAST       | 5µs  | 60µs  | 3µs    | 10µs    | 68µs |
| STANDARD   | 5µs  | 60µs  | 5µs    | 10µs    | 70µs |
| SLOW       | 8µs  | 90µs  | 20µs   | 15µs    | 118µs|
| ROBUST     | 10µs | 110µs | 30µs   | 18µs    | 150µs|
| CUSTOM     | 1µs  | 60µs  | 1µs    | 15µs    | 62µs |

SLOW / ROBUST widen the bit slot (118 µs / 150 µs instead of 70 µs), which trades
**protocol throughput** — every command and every bit read costs proportionally
more time — for timing margin. They do **not** change the conversion wait: that
is fixed by the resolution alone (93.75 ms … 750 ms). They are intended for
long wiring, parasite buses or electrically noisy setups.

On a parasite-powered bus the strong-pullup release must not clip the sensor's
slot sampling, so the guard band needs to be wider than the 5µs external-power
default. Compile with `-DOW_PARASITE_POWER=1`: when `ONEWIRE_GUARD_BAND` is not
defined by a preset or an explicit `-D`, it defaults to 100µs. Presets always
pin the guard explicitly, so on long or noisy parasite wiring pass the value
directly instead — e.g. `make TIMING=ROBUST EXT="-DONEWIRE_GUARD_BAND=250"`.

CUSTOM uses the minimum slot timing allowed by the 1-Wire standard
(t_LOW1 = 1µs, t_LOW0 = 60µs, t_REC = 1µs). It is an experimental setting:
a 1µs read/write pulse is **below the values validated on hardware** (a 2µs
pulse already broke slot decoding on an F030 at 8MHz — see also the note in
`inc/ow_config.h`). Use it only for experiments or electrically ideal setups.

**OC3 output-compare preload (`OC3PE`).** The write/capture paths that must
leave the bus idle-HIGH at the terminal update (capture, read-pair, single-slot
write) gate the `TIM_CCMR2` preload bit behind `-DOW_PORT_OC3PE=1` (default) /
`-DOW_PORT_OC3PE=0`. The DMA-fed paths leave it off unconditionally, because
their reload must act immediately — preload would break it. The knob exists so
a bench can sweep the bit without editing code; see `inc/ow_port.h` for the
rationale.

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
- Inspect Captured Data: Examine the driver context's `ctx.capture[]` after a reset
  or `ctx.pulse[]` after a read (in `src/ds18b20.c`) to see the raw timing
  data.

## License

This project is released under the MIT License. See the LICENSE file for details.

## Contributing

See [CONTRIBUTING.md](CONTRIBUTING.md) for the full guide (bug reports via
the issue templates, feature requests, Code of Conduct, PR workflow). Quick
version:

1. Fork and create a feature branch (`git checkout -b fix/my-change`).
2. Make your changes and keep the host tests green: `make test`, plus
   `make test-f0` / `make test-f3` / `make test-g0` / `make test-f4` /
   `make test-g4` for the other backends.
3. Format touched sources with the repo's `.clang-format`
   (`clang-format --dry-run --Werror <files>` must pass), using the
   `clang-format` version CI pins — see the `format` job in
   `.github/workflows/ci.yml`, which is the source of truth; a different
   release can disagree about the same source.
4. Open a Pull Request — CI builds every target and runs the test suite.

## Support

For issues and questions, please open an issue on GitHub.

## References

- DS18B20 Datasheet  
  https://datasheets.maximintegrated.com/en/ds/DS18B20.pdf

- STM32F103 Reference Manual  
  https://www.st.com/resource/en/reference_manual/cd00171190-stm32f101xx-stm32f102xx-stm32f103xx-advanced-arm-based-32-bit-mcus-stmicroelectronics.pdf

- STM32F030 Reference Manual  
  https://www.st.com/resource/en/reference_manual/rm0091-stm32f0x1stm32f0x2stm32f0x8-advanced-armbased-32bit-mcus-stmicroelectronics.pdf

- STM32G031 Reference Manual  
  https://www.st.com/resource/en/reference_manual/rm0444-stm32g0x1stm32g0x2-advanced-armbased-32bit-mcus-stmicroelectronics.pdf

- STM32F405/F407 Reference Manual (RM0090)
- STM32F401 Reference Manual (RM0368)
- STM32F446 Reference Manual (RM0390)
- STM32F303 Reference Manual (RM0316)

- 1-Wire Protocol Specification  
  https://www.analog.com/en/resources/technical-articles/1wire-communication.html
