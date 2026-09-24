# Integrating stm32-async-1wire into an external project

Five supported ways to pull the library into your firmware. Pick the row that
matches your build system — they all compile the same three sources
(`src/onewire.c`, `src/ds18b20.c`, `src/ow_stats.c`) and put `include/` +
`port/` on the include path.

| # | You build with… | Use |
|---|-----------------|-----|
| 1 | CMake (FetchContent / add_subdirectory / find_package) | [CMake](#1-cmake) |
| 2 | STM32CubeIDE (managed Makefile or existing Makefile) | [STM32CubeIDE](#2-stm32cubeide) |
| 3 | PlatformIO | [PlatformIO](#3-platformio) |
| 4 | Arduino STM32 core (Arduino_STM32 / STMDuino) | [Arduino STM32](#4-arduino-stm32) |
| 5 | Plain Makefile + git submodule | [Makefile + submodule](#5-makefile--git-submodule) |

Common requirements for every path:

- **CMSIS device headers** for your MCU family (F1 / F0 / G0). Cube firmware
  trees and PlatformIO already have them; bare Makefile projects usually run
  `make download-deps` once inside the library checkout.
- **A family define**: the library auto-detects `STM32F1` / `STM32F0` /
  `STM32G0` (defined by Cube/PlatformIO/Arduino), or you pass
  `-DOW_PORT_TARGET_F1` (or `_F0` / `_G0`) yourself.
- **Bus pin**: all three shipped backends use **PA10** (TIM1 CH3 out / CH4
  capture; G0 via the SYSCFG PA12 remap). Change the port header only if you
  fork the backend.

---

## 1. CMake

The library is a normal CMake package: `stm32_async_1wire` (target),
`find_package(stm32_async_1wire)`, install/export tree under
`lib/cmake/stm32_async_1wire/`.

### 1a. FetchContent (no local clone)

```cmake
cmake_minimum_required(VERSION 3.14)
project(my_firmware C ASM)

include(FetchContent)
FetchContent_Declare(stm32_1wire
    GIT_REPOSITORY https://github.com/a5021/stm32-async-1wire.git
    GIT_TAG        v2.0.0          # pin a tag, not a branch
)
FetchContent_MakeAvailable(stm32_1wire)

add_executable(my_firmware main.c)
target_link_libraries(my_firmware PRIVATE stm32_async_1wire)
```

CMSIS: the super-project is expected to provide device headers (typical for
STM32 firmware trees). If you want the library to fetch them itself:

```cmake
set(OW_FETCH_CMSIS ON CACHE BOOL "" FORCE)   # default ON only when top-level
# or point at an existing tree:
set(OW_CMSIS_CORE_INCLUDE   "${CUBE}/Drivers/CMSIS/Core/Include")
set(OW_CMSIS_DEVICE_INCLUDE "${CUBE}/Drivers/CMSIS/Device/ST/STM32F1xx/Include")
```

### 1b. add_subdirectory (git submodule / vendored copy)

```cmake
add_subdirectory(third_party/stm32-async-1wire)
target_link_libraries(my_firmware PRIVATE stm32_async_1wire)
```

`OW_BUILD_EXAMPLES` is ignored when the library is not the top-level project,
so a submodule will not try to build the seven demo apps inside your firmware
tree.

### 1c. find_package (installed package)

```bash
cmake -S . -B build -DCMAKE_TOOLCHAIN_FILE=cmake/arm-none-eabi-gcc.cmake \
      -DOW_TARGET=f1 -DOW_BUILD_EXAMPLES=OFF
cmake --build build
cmake --install build --prefix /opt/stm32-async-1wire
```

```cmake
list(APPEND CMAKE_PREFIX_PATH /opt/stm32-async-1wire)
find_package(stm32_async_1wire REQUIRED CONFIG)
target_link_libraries(my_firmware PRIVATE stm32_async_1wire::stm32_async_1wire)
```

The package config runs `find_path` for CMSIS as a courtesy; if your layout
is non-standard set `OW_CMSIS_CORE_INCLUDE` / `OW_CMSIS_DEVICE_INCLUDE`
before `find_package()`.

### Useful cache variables

| Variable | Default | Meaning |
|----------|---------|---------|
| `OW_TARGET` | `f1` | `f1` \| `f0` \| `g0` |
| `OW_BUILD_EXAMPLES` | `OFF` | Build demos (top-level only) |
| `OW_FETCH_CMSIS` | `ON` if top-level | FetchContent missing CMSIS |
| `OW_CMSIS_CORE_INCLUDE` | auto `find_path` | Dir with `cmsis_gcc.h` |
| `OW_CMSIS_DEVICE_INCLUDE` | auto `find_path` | Dir with `stm32*xx.h` |

`-Os -Wall -Wextra` are PRIVATE to the library target; only MCU arch flags
(`-mcpu… -mthumb`) and the family defines propagate to your code.

---

## 2. STM32CubeIDE

Two supported shapes:

### 2a. Existing CubeIDE C/C++ project (managed Makefile)

1. **File → Import → General → Existing Projects into Workspace** is *not*
   required — add the library as linked resources instead:
   - Right-click project → **New → Folder → Advanced → Link to alternative
     location** and point at the `stm32-async-1wire` checkout (or add it as a
     git submodule under `MiddlePacks/`).
2. **Project → Properties → C/C++ Build → Settings → MCU GCC Compiler →
   Includes**: add
   - `…/stm32-async-1wire/include`
   - `…/stm32-async-1wire/port`
3. **Source folders**: mark `src/` of the library as a source folder
   (right-click → **New → Source Folder** is not needed if you link the whole
   `src` directory; ensure `src/internal/*.c` is *not* compiled separately —
   only `onewire.c`, `ds18b20.c`, `ow_stats.c` belong in the build).
4. **Preprocessor symbols**: add `OW_PORT_TARGET_F1` (or rely on Cube’s
   `STM32F1` define, which the family-detection chain already accepts).
5. Build the Cube project as usual — the three library `.c` files compile
   with your existing MCU flags, linker script and CMSIS paths.

### 2b. Import as “Makefile project”

If you prefer the library’s own Makefile for demos:

1. **File → New → STM32 Project from an Existing Makefile**.
2. Point it at the `stm32-async-1wire` directory.
3. `make download-deps` first (shell/WSL on Windows).
4. Select the example (`APP=…`, `OW_TARGET=…`) and build.

For *your* application, prefer 2a (link sources into the Cube project) or
section 5 (plain Makefile + submodule).

---

## 3. PlatformIO

`library.json` is already wired for PlatformIO (`stm32cube` framework,
`ststm32` platform). Two ways to consume it:

### 3a. From the registry / git URL

```ini
[env:bluepill]
platform = ststm32
board = bluepill_f103c8
framework = stm32cube
lib_deps =
    https://github.com/a5021/stm32-async-1wire.git#v2.0.0
build_flags =
    -DOW_PORT_TARGET_F1
    ; optional: -DOW_STATS_ENABLE=1  -DOW_PARASITE_POWER=1
```

PlatformIO automatically:
- puts `include/` on the include path (`includeDir`),
- adds `-Iport` (`build.flags`),
- compiles only `onewire.c`, `ds18b20.c`, `ow_stats.c` (`srcFilter`;
  `src/internal/*` is excluded — those files are include-only parts of
  `ds18b20.c`).

### 3b. Local checkout / git submodule

```ini
lib_deps =
    symlink://../stm32-async-1wire
; or place the repo under lib/stm32-async-1wire/
```

CMSIS: the `stm32cube` framework already ships device headers — no extra
`OW_CMSIS_*` paths are needed.

**Note:** the library’s own `tests/` and `examples/` are not built by
PlatformIO; only the filtered `src/` files are.

---

## 4. Arduino STM32

`library.properties` targets Arduino Library Manager (`architectures=stm32`).

### 4a. Library Manager / manual install

1. Copy (or git-clone) the repository into your Arduino `libraries/` folder
   as `stm32-async-1wire`.
2. Restart the IDE. The library appears under **Sketch → Include Library**.
3. In the sketch:

```cpp
#include <stm32_async_1wire.h>   // umbrella header

void setup() {
    ds18b20_init();
}

void loop() {
    ds18b20_poll();
}
```

4. Implement the weak callbacks in the sketch (they override the library’s
   empty defaults):

```cpp
void ds18b20_complete(int16_t temp_tenths) { /* your handler */ }
void ds18b20_busy(unsigned action)         { /* optional LED */ }
```

### 4b. Family detection

The Arduino STM32 cores define `STM32F1` / `STM32F0` / `STM32G0`; the library
picks the matching `ow_port_*.h` automatically. If your core does not define
them, add to `build_flags` / platform.txt:

```
-DOW_PORT_TARGET_F1
```

### 4c. What gets compiled

Arduino builds every `.c`/`.cpp` under `src/`. The `src/internal/*.c` files
are include-only (guarded by `DS18B20_DRIVER_BUILD` and compiled solely via
`#include` from `ds18b20.c`), so they contribute no duplicate symbols.
`src/syscall.c` is only needed for pure Makefile builds (nosys specs); on
Arduino it is weak-stubbed by the core and can stay in the tree unused.

---

## 5. Makefile + git submodule

For projects that already use a hand-rolled Makefile (Blue Pill demos,
custom bare-metal apps).

### 5a. Add the submodule

```bash
git submodule add https://github.com/a5021/stm32-async-1wire.git third_party/stm32-async-1wire
git submodule update --init --recursive
```

### 5b. Pull in the integration fragment

```makefile
# --- top of your Makefile ----------------------------------------------------
OW_1WIRE_DIR := third_party/stm32-async-1wire
OW_TARGET     ?= f1              # f1 | f0 | g0

# your own flags / objects …
CFLAGS  += -mcpu=cortex-m3 -mthumb -Os -Wall -Wextra
# … CMSIS includes you already have …
CPPFLAGS += -Ipath/to/cmsis/core -Ipath/to/cmsis/device -DSTM32F103xB

include $(OW_1WIRE_DIR)/ow-integration.mk

# --- link --------------------------------------------------------------------
firmware.elf: $(APP_OBJ) $(OW_1WIRE_OBJ)
	$(CC) $^ $(LDFLAGS) -o $@
```

### 5c. What the fragment provides

| Variable | Contents |
|----------|----------|
| `OW_1WIRE_ROOT` | Absolute path to the checkout |
| `OW_1WIRE_SRC`  | `onewire.c`, `ds18b20.c`, `ow_stats.c` |
| `OW_1WIRE_INC`  | `-I…/include -I…/port` |
| `OW_1WIRE_DEFS` | `-DOW_PORT_TARGET_F?` (+ your `OW_1WIRE_DEFS`) |
| `OW_1WIRE_OBJ`  | Objects built into `$(OW_1WIRE_BUILD_DIR)` (default `$(BUILD_DIR)`) |
| `OW_1WIRE_CFLAGS` | Extra flags for library TUs only |

Optional features are plain macros, same as the library Makefile:

```makefile
OW_1WIRE_CFLAGS += -DOW_STATS_ENABLE=1 -DOW_PARASITE_POWER=1
```

### 5d. CMSIS one-shot download (optional)

If you do not already vendor CMSIS headers:

```bash
make -C third_party/stm32-async-1wire download-deps
# then add to CPPFLAGS:
#   -Ithird_party/stm32-async-1wire/CMSIS/core
#   -Ithird_party/stm32-async-1wire/CMSIS/device
```

---

## Quick checklist (any path)

- [ ] Three sources compiled: `onewire.c`, `ds18b20.c`, `ow_stats.c`
- [ ] `include/` and `port/` on the include path
- [ ] Exactly one family define: auto (`STM32F1`/`F0`/`G0`) **or**
      `-DOW_PORT_TARGET_F1` / `_F0` / `_G0`
- [ ] CMSIS Core + device headers visible to the compiler
- [ ] Bus wired on **PA10** (or forked port header)
- [ ] Call `ds18b20_init()` once, `ds18b20_poll()` from the main loop
- [ ] Implement `ds18b20_complete()` (and optionally `ds18b20_busy()`)

## Troubleshooting

| Symptom | Likely cause |
|---------|--------------|
| `ow_port: no family selected` | Missing family define — add `-DOW_PORT_TARGET_F1` (or let Cube/PlatformIO define `STM32F1`) |
| `stm32f1xx.h: No such file` | CMSIS device include path missing |
| Multiple definition of `ds18b20_*` | `src/internal/*.c` compiled standalone — only `ds18b20.c` should be a TU |
| Examples appear inside your super-project | You set `OW_BUILD_EXAMPLES=ON` while using `add_subdirectory` — the library ignores it when not top-level; reconfigure the super-project if you really want demos |
| Find_package cannot find CMSIS | Set `OW_CMSIS_CORE_INCLUDE` / `OW_CMSIS_DEVICE_INCLUDE` before `find_package()` |

See also: [README → Building](README.md#building),
[README → Configuration](README.md#configuration),
[README → API Reference](README.md#api-reference).
