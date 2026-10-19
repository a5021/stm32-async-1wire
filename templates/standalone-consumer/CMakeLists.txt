# =============================================================================
# Standalone-consumer template - a vendored use of stm32-async-1wire
# =============================================================================
#
# The smallest complete application that builds against the library as a
# *vendored* dependency (inc/ + src/ copied or referenced in place) rather than
# through FetchContent or an installed package. It uses the drop-in helper
# cmake/ow_add_library.cmake to wire the library up, and adds only what a
# bare-metal app must bring itself: CMSIS, the startup file, the linker script
# and a board layer (board.c).
#
# What it proves, and why it is checked in CI: the documented vendored path
# builds end to end - library sources, the helper's propagated flags, and a real
# consumer linking into a flashable ELF.
#
# Configure it like any other downstream project, with a bare-metal toolchain.
# Two examples (both assume the helper's `arm-none-eabi-gcc.cmake`):
#
#   # In-tree: reuse the checkout's own downloaded CMSIS (make OW_TARGET=h5 download-deps)
#   cmake -S templates/standalone-consumer -B build/standalone \
#       -G "Unix Makefiles" \
#       -DCMAKE_TOOLCHAIN_FILE=cmake/arm-none-eabi-gcc.cmake \
#       -DOW_CMSIS_CORE_INCLUDE="$PWD/CMSIS/core" \
#       -DOW_CMSIS_DEVICE_INCLUDE="$PWD/CMSIS/device"
#   cmake --build build/standalone
#
#   # Copied out of the tree: point OW_LIBRARY_DIR at the checkout and supply your
#   # own CMSIS (STM32Cube / PlatformIO / a manual checkout).
#   cmake -S . -B build \
#       -G "Unix Makefiles" \
#       -DCMAKE_TOOLCHAIN_FILE=/path/to/stm32-async-1wire/cmake/arm-none-eabi-gcc.cmake \
#       -DOW_LIBRARY_DIR=/path/to/stm32-async-1wire \
#       -DOW_CMSIS_CORE_INCLUDE=/path/to/CMSIS/Core/Include \
#       -DOW_CMSIS_DEVICE_INCLUDE=/path/to/cmsis_device_h5/Include \
#       -DOW_CMSIS_SYSTEM_SRC=/path/to/cmsis_device_h5/Source/Templates/system_stm32h5xx.c \
#       -DOW_CMSIS_STARTUP_ASM=/path/to/cmsis_device_h5/Source/Templates/gcc/startup_stm32h503xx.s \
#       -DOW_LINKER_SCRIPT=/path/to/stm32-async-1wire/port/stm32h5/STM32H503CB_FLASH.ld
#
# Generators: use the platform default on Linux/macOS ("Unix Makefiles"). On
# Windows use "MinGW Makefiles" (with mingw32-make in PATH) - the Windows-GNU
# platform file's PE-only link flags are already stripped above, and the
# "Unix Makefiles" generator interacts badly with a native make + Cygwin sh on
# Windows. The ELF-suffix and link-rule overrides make both work.
# =============================================================================

cmake_minimum_required(VERSION 3.20)
project(stm32_async_1wire_standalone_consumer C ASM)

# ---------------------------------------------------------------------------
# Bare-metal link rule (identical on Linux CI and Windows consumers)
# ---------------------------------------------------------------------------
# Two Windows-only defaults have to go before a firmware link can succeed:
#  * CMake's Windows-GNU platform file appends PE-only linker flags
#    (--out-implib, --major-image-version, ...) to executable links, which the
#    ARM ELF linker rejects. Spell the rule without them - an import library is
#    meaningless for firmware.
#  * It also adds the Windows system libraries (kernel32 et al.). Clear them and
#    pull in the runtime explicitly below instead.
# Setting the suffix keeps the MinGW Makefiles generator from treating the target
# as a .exe and appending the same PE flags. On Linux these three are no-ops.
set(CMAKE_C_LINK_EXECUTABLE "<CMAKE_C_COMPILER> <FLAGS> <LINK_FLAGS> <OBJECTS> -o <TARGET> <LINK_LIBRARIES>")
set(CMAKE_C_STANDARD_LIBRARIES "")
set(CMAKE_EXECUTABLE_SUFFIX ".elf")

# ---------------------------------------------------------------------------
# Knobs
# ---------------------------------------------------------------------------
set(OW_LIBRARY_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../.." CACHE PATH
    "Path to the stm32-async-1wire checkout (the directory holding inc/ and src/).")
set(OW_MCU "STM32H503xx" CACHE STRING
    "Device macro (STM32H503xx) or family macro (STM32H5)")
set(OW_SYSCLK_MHZ "64" CACHE STRING
    "System clock in MHz the firmware runs at (board.c implements 64)")
set(OW_PARASITE ON CACHE BOOL "Devices are powered over the data line (parasite)")

set(OW_CMSIS_CORE_INCLUDE "" CACHE PATH
    "Directory holding cmsis_gcc.h / core_cm33.h (your CMSIS Core/Include)")
set(OW_CMSIS_DEVICE_INCLUDE "" CACHE PATH
    "Directory holding stm32h5xx.h (the cmsis_device_h5 Include dir)")
# Defaults match the repository's flattened `make download-deps` layout, where
# the device headers, system source and startup file all sit in one directory.
# A stock STM32Cube layout keeps them apart; point these three at the real files.
set(OW_CMSIS_SYSTEM_SRC "${OW_CMSIS_DEVICE_INCLUDE}/system_stm32h5xx.c" CACHE FILEPATH
    "CMSIS system_stm32h5xx.c source")
set(OW_CMSIS_STARTUP_ASM "${OW_CMSIS_DEVICE_INCLUDE}/startup_stm32h503xx.s" CACHE FILEPATH
    "CMSIS GCC startup assembly (startup_stm32h503xx.s)")
set(OW_LINKER_SCRIPT "${OW_LIBRARY_DIR}/port/stm32h5/STM32H503CB_FLASH.ld" CACHE FILEPATH
    "Linker script for the target part")

# ---------------------------------------------------------------------------
# Validate - fail loudly, with the knob to fix, rather than deep in a compile
# ---------------------------------------------------------------------------
foreach(_ow_var OW_CMSIS_CORE_INCLUDE OW_CMSIS_DEVICE_INCLUDE)
    if(NOT ${_ow_var})
        message(FATAL_ERROR
            "${_ow_var} is not set. The library does not ship CMSIS; supply your "
            "own (STM32Cube, PlatformIO, or a manual checkout).")
    endif()
endforeach()
if(NOT EXISTS "${OW_LIBRARY_DIR}/inc/ow_board.h")
    message(FATAL_ERROR
        "OW_LIBRARY_DIR='${OW_LIBRARY_DIR}' does not look like the library "
        "checkout (no inc/ow_board.h). Point it at the stm32-async-1wire root.")
endif()
foreach(_ow_file OW_CMSIS_SYSTEM_SRC OW_CMSIS_STARTUP_ASM OW_LINKER_SCRIPT)
    if(NOT EXISTS "${${_ow_file}}")
        message(FATAL_ERROR "${_ow_file}='${${_ow_file}}' does not exist.")
    endif()
endforeach()

# ---------------------------------------------------------------------------
# Library, via the drop-in helper
# ---------------------------------------------------------------------------
include(${OW_LIBRARY_DIR}/cmake/ow_add_library.cmake)

set(_ow_args
    MCU ${OW_MCU}
    SYSCLK_MHZ ${OW_SYSCLK_MHZ}
    CMSIS_INCLUDE_DIRS ${OW_CMSIS_CORE_INCLUDE} ${OW_CMSIS_DEVICE_INCLUDE}
    SOURCE_DIR ${OW_LIBRARY_DIR}
)
if(OW_PARASITE)
    list(APPEND _ow_args PARASITE)
endif()
ow_add_library(owlib ${_ow_args})

# ---------------------------------------------------------------------------
# Application
# ---------------------------------------------------------------------------
add_executable(ow_consumer
    main.c
    board.c
    ${OW_CMSIS_SYSTEM_SRC}
    ${OW_CMSIS_STARTUP_ASM}
)
target_include_directories(ow_consumer PRIVATE
    ${OW_CMSIS_CORE_INCLUDE}
    ${OW_CMSIS_DEVICE_INCLUDE}
)
# owlib's flags are PUBLIC, so the app already inherits -mcpu/-mthumb, the device
# and clock defines, and -Wall/-Wextra; only the optimisation is app-local.
target_compile_options(ow_consumer PRIVATE -Os)

target_link_libraries(ow_consumer PRIVATE owlib)
target_link_options(ow_consumer PRIVATE
    -T${OW_LINKER_SCRIPT}
    -Wl,--gc-sections
    --specs=nano.specs
    # Standard libraries, linked explicitly because CMAKE_C_STANDARD_LIBRARIES
    # was cleared above: -lc/-lm for the newlib-nano runtime (nano.specs picks
    # the small variants), -lnosys for the weak syscall stubs.
    -lc -lm -lnosys
)
