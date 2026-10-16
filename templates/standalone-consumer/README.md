# Standalone consumer template

The smallest complete application that uses `stm32-async-1wire` as a **vendored**
dependency: `inc/` and `src/` are referenced in place (copied or submodule'd),
not pulled through FetchContent or an installed package. It reads one DS18B20
and prints the temperature over USART1.

Reach for this template when you want to drop the driver into an existing
bare-metal project and wire it up yourself. If you build with CMake and can add
the library as a subdirectory or `find_package()` target, prefer those paths -
they manage CMSIS for you and this template's glue is unnecessary.

## Files

| File | Role |
| --- | --- |
| `CMakeLists.txt` | Wires the library through `cmake/ow_add_library.cmake` and links a firmware ELF. |
| `main.c` | The application: `board_init()`, `ds18b20_init()`, non-blocking poll loop. |
| `board.c` / `board.h` | Board bring-up the library does **not** provide: 64MHz clock, USART1 console, polled 1ms tick. Replace with your own. |

The library owns the whole 1-Wire port (`inc/ow_port.h` is a header of static
inline register writes), so there is no port glue to write - only the clock, a
console and a tick, which any board already has.

## Requirements

- `arm-none-eabi-gcc` (the template is tested with 15.2.rel1), `cmake` (>= 3.20),
  and a make (or Ninja).
- CMSIS: the ARM Core headers (`cmsis_gcc.h`, `core_cm33.h`) and the ST device
  headers/sources for STM32H5 (`stm32h5xx.h`, `system_stm32h5xx.c`,
  `startup_stm32h503xx.s`). The library never ships these - supply your own from
  STM32Cube, PlatformIO, or a checkout.

## Build

**In-tree** (reuses the checkout's own downloaded CMSIS):

```sh
make OW_TARGET=h5 download-deps        # fetches CMSIS/ into the repo
cmake -S templates/standalone-consumer -B build/standalone \
    -DCMAKE_TOOLCHAIN_FILE=cmake/arm-none-eabi-gcc.cmake \
    -DOW_CMSIS_CORE_INCLUDE="$PWD/CMSIS/core" \
    -DOW_CMSIS_DEVICE_INCLUDE="$PWD/CMSIS/device"
cmake --build build/standalone
```

The result is `build/standalone/ow_consumer.elf`.

**Copied out of the tree** - point `OW_LIBRARY_DIR` at the checkout and give the
three CMSIS file paths explicitly (a stock STM32Cube layout keeps the headers,
the system source and the startup file in different directories):

```sh
cmake -S . -B build \
    -DCMAKE_TOOLCHAIN_FILE=/path/to/stm32-async-1wire/cmake/arm-none-eabi-gcc.cmake \
    -DOW_LIBRARY_DIR=/path/to/stm32-async-1wire \
    -DOW_CMSIS_CORE_INCLUDE=/path/to/CMSIS/Core/Include \
    -DOW_CMSIS_DEVICE_INCLUDE=/path/to/cmsis_device_h5/Include \
    -DOW_CMSIS_SYSTEM_SRC=/path/to/cmsis_device_h5/Source/Templates/system_stm32h5xx.c \
    -DOW_CMSIS_STARTUP_ASM=/path/to/cmsis_device_h5/Source/Templates/gcc/startup_stm32h503xx.s \
    -DOW_LINKER_SCRIPT=/path/to/stm32-async-1wire/port/stm32h5/STM32H503CB_FLASH.ld
cmake --build build
```

### Knobs

| Variable | Default | Meaning |
| --- | --- | --- |
| `OW_LIBRARY_DIR` | `../..` | Path to the library checkout (`inc/`, `src/`, `cmake/`). |
| `OW_MCU` | `STM32H503xx` | Device macro (`STM32H503xx`) or family macro (`STM32H5`). |
| `OW_SYSCLK_MHZ` | `64` | System clock the firmware runs at. `board.c` implements 64. |
| `OW_PARASITE` | `ON` | Devices are powered over the data line (`-DOW_PARASITE_POWER=1`). |
| `OW_CMSIS_CORE_INCLUDE` | - | Directory holding `cmsis_gcc.h`. **Required.** |
| `OW_CMSIS_DEVICE_INCLUDE` | - | Directory holding `stm32h5xx.h`. **Required.** |
| `OW_CMSIS_SYSTEM_SRC` | `<device>/system_stm32h5xx.c` | CMSIS system source. |
| `OW_CMSIS_STARTUP_ASM` | `<device>/startup_stm32h503xx.s` | CMSIS GCC startup file. |
| `OW_LINKER_SCRIPT` | `<lib>/port/stm32h5/STM32H503CB_FLASH.ld` | Linker script for the part. |

## Generators

Use the platform default on Linux/macOS (`Unix Makefiles`). On Windows use
`-G "MinGW Makefiles"` with `mingw32-make` in `PATH`: the project already strips
the Windows-GNU platform file's PE-only link flags (which the ARM ELF linker
rejects) and forces a `.elf` suffix, but the `Unix Makefiles` generator does not
pair well with a native `make` plus a Cygwin `sh`.

## Adapting

- **Other H5 clocks (250MHz HSE+PLL).** `board_clock_init()` implements only the
  64MHz raw-HSI bring-up clock. For 250MHz, follow the H5 branch of
  `examples/app/app.c`, pass `-DOW_SYSCLK_MHZ=250 -DOW_HSE_MHZ=<crystal>`; the
  port is already timing-correct for both.
- **Other families.** `board.c` is H5-only. `ow_add_library()` accepts every
  supported family (`MCU STM32F1 ...`); supply that family's CMSIS, startup and
  linker script and a matching `board.c`. The application (`main.c`) is
  family-agnostic.
- **SystemCoreClock instead of `SYSCLK_MHZ`.** Drop `SYSCLK_MHZ` from the
  `ow_add_library()` call and `board.c`'s check; the port then reads the CMSIS
  `SystemCoreClock` variable instead (call `SystemCoreClockUpdate()` after any
  clock change).

## Gotchas

- **H5 reset clock.** The H5 boots at 32MHz (HSIDIV=/2), not 64MHz. Clearing
  `RCC_CR.HSIDIV` and setting the flash latency first is not optional - a wrong
  core clock scales every 1-Wire timing.
- **Parasite power.** With `OW_PARASITE=ON` the driver holds the bus high through
  the conversion; a normal (3-wire) bus should build with `-DOW_PARASITE=OFF`.
- **LTO.** The helper points the archiver at `arm-none-eabi-gcc-ar` when it is
  available, so `-flto` works; the template itself builds with `-Os` only.
