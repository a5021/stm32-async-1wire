# Repository file structure

[Back to the README](../README.md)

## File Structure

```text
├── inc/                    # Public headers: the whole consumer surface
│   ├── ds18b20.h           # Driver interface (high-level API) and constants
│   ├── ds2431.h            # DS2431 EEPROM driver interface (commands + async API)
│   ├── ds2413.h            # DS2413 dual-channel switch driver interface
│   ├── onewire.h           # Shared 1-Wire layer (bus primitives + Search ROM)
│   ├── ow_board.h          # Board facts: family select, ow_pulse_t, clock defaults
│   ├── ow_config.h         # Compile-time tunables (pulse widths, feature flags, limits)
│   ├── ow_stats.h          # Optional signal statistics module (histogram, per-sensor)
│   └── ow_version.h        # Library version (single source, read by check_version.sh)
├── port/                   # Per-MCU build artifacts for the examples (linker scripts, debug projects)
│   ├── stm32f1/            # STM32F1: TIM1 + DMA1 + PA10
│   │   ├── STM32F103XB_FLASH.ld  # Linker script, STM32F103xB (with .noinit section)
│   │   ├── stm32f103cb.jflash    # J-Flash project file (make jprogram)
│   │   ├── HARDWARE-NOTES.md     # F1-specific bring-up and bench notes
│   │   └── project.jdebug  # SEGGER Ozone project (STM32F103C8, SWD)
│   ├── stm32f0/            # STM32F0: TIM1 + DMA1 + PA10
│   │   ├── STM32F030X6_FLASH.ld  # Linker script, STM32F030x6 (16KB flash / 4KB RAM)
│   │   ├── stm32f030f4.jflash    # J-Flash project file
│   │   └── project.jdebug  # SEGGER Ozone project (STM32F030F4, SWD)
│   ├── stm32f3/            # STM32F3: TIM1 + DMA1 + PA10
│   │   ├── STM32F303XC_FLASH.ld  # Linker script, STM32F303xC (256KB flash / 32KB RAM)
│   │   ├── stm32f303vc.jflash    # J-Flash project file (STM32F303VC)
│   │   └── project-f303vc.jdebug # SEGGER Ozone project (STM32F303VC, SWD)
│   ├── stm32g0/            # STM32G0: TIM1 + DMA1 + DMAMUX + PA10 via PA12 remap
│   │   ├── STM32G031X6_FLASH.ld  # Linker script, STM32G031x6 (32KB flash / 8KB RAM)
│   │   ├── stm32g031f6.jflash    # J-Flash project file
│   │   └── project.jdebug  # SEGGER Ozone project (STM32G031F6, SWD)
│   ├── stm32g4/            # STM32G4: TIM1 + DMA1 + DMAMUX + PA10
│   │   ├── STM32G474CB_FLASH.ld  # Linker script, STM32G474CB (128KB flash / 96KB RAM)
│   │   ├── stm32g474cb.jflash    # J-Flash project file
│   │   └── project.jdebug  # SEGGER Ozone project (STM32G474CB, SWD)
│   ├── stm32l4/            # STM32L4: TIM1 + DMA1 + DMA1_CSELR + PA10
│   │   ├── STM32L476RGTX_FLASH.ld  # Linker script, STM32L476RG (1MB flash / 96KB SRAM1)
│   │   └── project-l476rg.jdebug   # SEGGER Ozone project (STM32L476RG, SWD)
│   ├── stm32c5/            # STM32C5: TIM1 + LPDMA1 + PA10
│   │   ├── STM32C552CE_FLASH.ld   # Linker script, STM32C552CE (512KB flash / 128KB SRAM)
│   │   ├── project-c552ce.jdebug  # SEGGER Ozone project (STM32C552CE, SWD)
│   │   └── HARDWARE-NOTES.md      # C5-specific DMA/timing notes
│   ├── stm32f4/            # STM32F4: TIM1 + DMA2 + PA10
│   │   ├── STM32F407VGT6_FLASH.ld  # Linker script, STM32F407VGT6 (1MB flash / 128KB RAM)
│   │   ├── STM32F401CC_FLASH.ld    # Linker script, STM32F401CC (256KB flash / 64KB RAM)
│   │   ├── STM32F446RE_FLASH.ld    # Linker script, STM32F446xE (512KB flash / 128KB RAM, no CCM)
│   │   ├── stm32f407vgt6.jflash    # J-Flash project file
│   │   ├── stm32f401cc.jflash      # J-Flash project file (STM32F401CC)
│   │   ├── stm32f446re.jflash      # J-Flash project file (STM32F446xE)
│   │   ├── project.jdebug          # SEGGER Ozone project (STM32F407VGT6, SWD)
│   │   ├── project-f401cc.jdebug   # SEGGER Ozone project (STM32F401CC, SWD)
│   │   ├── project-f446re.jdebug   # SEGGER Ozone project (STM32F446xE, SWD)
│   │   └── HARDWARE-NOTES.md  # F4-specific DMA/timing notes
│   ├── stm32h5/            # STM32H5: TIM1 + GPDMA1 + PA10
│   │   ├── STM32H503CB_FLASH.ld  # Linker script, STM32H503CB (128KB flash / 32KB RAM)
│   │   └── project-h503cb.jdebug  # SEGGER Ozone project (STM32H503CB, SWD; no J-Flash: CoreID unverified)
│   └── stm32h7/            # STM32H7: TIM1 + DMA1/DMAMUX1 + PA10
│       ├── STM32H743VI_FLASH.ld  # Linker script, STM32H743VI (2MB flash / 512KB AXI SRAM1)
│       └── project-h743vi.jdebug  # SEGGER Ozone project (STM32H743VI, SWD; no J-Flash: ST-Link flow)
├── config/                # Build settings shared by both build systems
│   └── optim.mk            # Optimisation profiles + the no-LTO family list
├── chips/                  # Per-part build identity, one file per part
│   ├── f103xb.mk           # CMSIS macro, startup, linker script, debugger
│   ├── f030x6.mk           #   projects, SVD and default clock. Shared by
│   ├── g031xx.mk           #   the Makefile and the CMake build
│   ├── f407xx.mk           #   (selected by OW_MCU=<device macro>; the legacy
│   ├── f303xc.mk           #   OW_TARGET/OW_CHIP pair selects them too)
│   ├── f401xc.mk           # 256KB flash / 64KB RAM
│   ├── f446xx.mk           # 512KB flash / 128KB RAM, 180MHz default
│   ├── g474cb.mk           # 128KB flash / 96KB RAM, 170MHz default (WeAct G474 Long, 8MHz HSE)
│   ├── l476rg.mk           # 1MB flash / 96KB SRAM1, 80MHz HSI16 default (NUCLEO-L476RG, no HSE)
│   ├── c552ce.mk           # 512KB flash / 128KB SRAM, 144MHz HSE->PSI default (WeAct C5 48-pin, 8MHz HSE)
│   ├── h503cb.mk           # 128KB flash / 32KB RAM, 64MHz HSI default (WeAct H503 Core, 8MHz HSE for later steps)
│   └── h743vi.mk           # 2MB flash / 512KB AXI SRAM1, 240MHz HSE+PLL default (WeAct Studio STM32H7XX, 25MHz HSE)
├── src/                    # Library sources (the engine headers are private to the library)
│   ├── ow_port.h           # 1-Wire port layer: TIM1+DMA engine (private; only src/onewire.c reaches it)
│   ├── ow_mcu.h            # Per-family register/DMA facts for the engine (private; 10 family sections)
│   ├── ow_compiler.h       # Private compiler-attribute shim for the driver sources
│   ├── ow_glue.h           # Internal bridge: timed waits and state-machine kicks
│   ├── ow_stats.c          # Signal statistics implementation (histogram, UART dump)
│   ├── onewire.c           # 1-Wire layer: state machine + bus primitives
│   │                       #               + non-blocking Search ROM engine
│   ├── ds18b20.c           # Driver: DS18B20 command set on the 1-Wire layer.
│   │                       # Compiles as ONE translation unit: #includes its
│   │                       # four functional parts in dependency order.
│   ├── ds18b20_internal.h  # Private split header shared by the include-only parts
│   ├── ds18b20_search.c    # (include-only part) ROM device table + search/alarm
│   ├── ds18b20_txn.c       # (include-only part) command transactions + parasite
│   ├── ds18b20_resolution.c # (include-only part) non-blocking resolution change
│   ├── ds18b20_measure.c   # (include-only part) DS18B20_ST_* measurement machine
│   ├── ds2431.c            # Driver: DS2431 EEPROM command set on the 1-Wire
│   │                       # layer. Owns the context and the public start/poll
│   │                       # API (core TU).
│   ├── ds2431_internal.h   # Private DS2431 interface: context layout + engine
│   │                       # entry points shared by the core and engine TUs.
│   ├── ds2431_txn.c        # Engine TU: reset -> write -> read/wait on the
│   │                       # caller-owned context (no globals).
│   └── ds2413.c            # Driver: DS2413 PIO Access Read/Write, one TU
├── examples/               # Example applications
│   ├── app/                # Shared application layer (UART, clock, init)
│   │   ├── app.c           # app_init(), UART TX ring buffer, busy LED
│   │   ├── app.h           # Shared application layer interface
│   │   └── syscall.c       # Minimal libc stubs (_write/_sbrk/...) for bare metal (example support, not library code)
│   ├── 0_dma_pressure/main.c   # Diagnostic: feed margin under memory load
│   ├── 1_basic/main.c      # Single sensor, unconditional (Skip ROM)
│   ├── 2_device_search/main.c  # Device search + per-device poll (no broadcast convert)
│   ├── 3_round_robin/main.c    # Device search + sequential poll of all
│   ├── 4_scan_mode/main.c  # Device search + simultaneous conversion
│   ├── 5_commands/main.c   # Device search + command transactions
│   │                       # (ROM, power supply, TH/TL, Copy/Recall EEPROM)
│   ├── 6_statistics/main.c # Device search + stats dump every N cycles
│   ├── 7_low_power/main.c  # Device search + WFE low-power sleep on long stages
│   ├── 8_ds2431/main.c     # DS2431: one EEPROM write cycle, then a repeating search
│   └── 9_multi/main.c      # Peak/low logger: DS18B20 + DS2431 + DS2413 blink
├── tests/                  # Host test suite (no hardware required)
│   ├── mock/               # Behavioural TIM1/DMA model + register mocks
│   │                       # (+ optional WFE sleep hook, -DMOCK_WFE_HOOK)
│   ├── fuzz/               # libFuzzer harnesses (ASAN/UBSAN, 14 harnesses)
│   ├── test/               # Unity-based test cases
│   ├── integration/        # Consumer fixtures: both CMake paths + PlatformIO
│   ├── api/                # Public-API declaration snapshot + mutation canary
│   ├── hil/                # Hardware-in-the-loop harness (needs a bench)
│   ├── ci/                 # CI helper scripts (apt/pip installers)
│   ├── check_chips.sh      # Part-matrix check (make test-chips)
│   ├── check_mock_headers.sh  # Mocks vs real CMSIS macros (make test-mocks)
│   ├── check_ram_budget.sh    # Library .bss ceilings (make test-ram)
│   ├── check_library_manifest.sh  # library.json vs the PIO consumer
│   ├── check_version.sh    # Version in every declaration site (make test-version)
│   ├── check_header_hygiene.sh  # Public headers stay free of engine internals
│   ├── check_mcu_dispatch.sh  # Makefile and CMake dispatch agree on flags
│   ├── check_elf_variant.sh    # Each build links its own objects
│   ├── check_removed_macros.sh # Removed names are hard #errors naming their replacement
│   └── mutation_check.py       # Deliberate defects the host suite must catch
├── cmake/                  # CMake package
│   ├── arm-none-eabi-gcc.cmake  # Bare-metal cross-compilation toolchain file
│   ├── ow_add_library.cmake      # Drop-in helper for vendored inc/ + src/ copies
│   ├── ow_flags.cmake            # MCU -> family/core/-mcpu/CMSIS table shared by both paths
│   └── stm32_async_1wireConfig.cmake.in  # find_package() config template
├── tools/                  # Standalone host tools (not part of the library build)
│   ├── 1wire_vis/          # Browser 1-Wire capture viewer + bundled 9_multi capture
│   ├── stress_report.py    # Generates docs/stress/index.html
│   ├── la_capture.py       # Logic-analyzer capture helper
│   ├── la_decode.py        # Logic-analyzer transport decoder
│   ├── uart_capture.py     # UART capture helper
│   └── check_records.py    # Bench-record checker
├── templates/              # Copyable consumer projects
│   └── standalone-consumer/  # Vendored inc/ + src/ CMake template (H5 board.c + main.c)
├── docs/                   # Topic guides, stress report and bench records
│   ├── *.md                # The topic guides linked from the README
│   ├── api/                # Doxygen-generated API reference (generated, gitignored)
│   └── stress/             # Published DMA-pressure report and captures
├── .github/                # GitHub configuration
│   ├── workflows/          # CI (build.yml, ci.yml) and release (release.yml)
│   ├── ISSUE_TEMPLATE/     # Bug report / feature request templates
│   └── PULL_REQUEST_TEMPLATE.md
├── CMSIS/                  # Makefile's downloaded dependencies (gitignored)
│   ├── core/               # ARM CMSIS 5 core headers
│   └── device/             # STM32 headers/startup (F0/F1/F3/F4/G0/G4/L4/C5/H5/H7); SVDs where available (none for H503/H743)
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
