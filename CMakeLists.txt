cmake_minimum_required(VERSION 3.14)

# Single source of truth for the library version: the VERSION file.
# include/version.h, library.json and library.properties must match;
# the version-sync CI job enforces it.
file(READ "${CMAKE_CURRENT_SOURCE_DIR}/VERSION" STM32_ASYNC_1WIRE_VERSION)
string(STRIP "${STM32_ASYNC_1WIRE_VERSION}" STM32_ASYNC_1WIRE_VERSION)

project(stm32_async_1wire VERSION ${STM32_ASYNC_1WIRE_VERSION} LANGUAGES C ASM)

# ---------------------------------------------------------------------------
# Top-level detection (PROJECT_IS_TOP_LEVEL needs CMake >= 3.21)
# ---------------------------------------------------------------------------
if(CMAKE_CURRENT_SOURCE_DIR STREQUAL CMAKE_SOURCE_DIR)
    set(OW_IS_TOP_LEVEL ON)
else()
    set(OW_IS_TOP_LEVEL OFF)
endif()

# ---------------------------------------------------------------------------
# Options
# ---------------------------------------------------------------------------
set(OW_TARGET "f1" CACHE STRING "MCU family: f1 (default), f0, g0")
set_property(CACHE OW_TARGET PROPERTY STRINGS f1 f0 g0)

option(OW_BUILD_EXAMPLES "Build example applications (top-level builds only)" OFF)

# When ON (default for standalone builds), missing CMSIS headers are fetched
# with FetchContent at the pinned tags below.  Consumers that already have
# CMSIS (STM32Cube, PlatformIO, another super-project) should either leave
# this OFF and set OW_CMSIS_*_INCLUDE, or rely on the find_path fallback.
option(OW_FETCH_CMSIS "Fetch ARM CMSIS / ST device headers via FetchContent" ${OW_IS_TOP_LEVEL})

# OW_CMSIS_CORE_INCLUDE / OW_CMSIS_DEVICE_INCLUDE are cache PATHs created by
# the find_path calls below (or set with -D before the first configure).

# Pinned dependency tags — never track a moving branch.
set(OW_CMSIS_CORE_GIT_TAG     "5.9.0"  CACHE STRING "ARM CMSIS_5 git tag")
set(OW_CMSIS_DEVICE_GIT_TAG_F1 "v4.3.5" CACHE STRING "cmsis_device_f1 git tag")
set(OW_CMSIS_DEVICE_GIT_TAG_F0 "v2.3.7" CACHE STRING "cmsis_device_f0 git tag")
set(OW_CMSIS_DEVICE_GIT_TAG_G0 "v1.4.5" CACHE STRING "cmsis_device_g0 git tag")

# Examples require startup/system/linker assets and only make sense as the
# root of a firmware build — never as a side effect of add_subdirectory().
if(OW_BUILD_EXAMPLES AND NOT OW_IS_TOP_LEVEL)
    message(STATUS "stm32-async-1wire: OW_BUILD_EXAMPLES ignored (not top-level)")
    set(OW_BUILD_EXAMPLES OFF)
endif()

# ---------------------------------------------------------------------------
# Device-family selection
# ---------------------------------------------------------------------------
if(OW_TARGET STREQUAL "f1")
    set(CMSIS_DEVICE_REPO  "https://github.com/STMicroelectronics/cmsis_device_f1.git")
    set(CMSIS_DEVICE_GIT_TAG "${OW_CMSIS_DEVICE_GIT_TAG_F1}")
    set(CMSIS_DEVICE_HEADER "stm32f1xx.h")
    set(CMSIS_DEVICE_DEF  "STM32F103xB")
    set(OW_PORT_DEF       "OW_PORT_TARGET_F1")
    set(MCU_FLAGS         -mcpu=cortex-m3 -mthumb)
    set(STARTUP_ASM       "startup_stm32f103xb.s")
    set(SYSTEM_SRC        "system_stm32f1xx.c")
    set(LINKER_SCRIPT     "${CMAKE_CURRENT_SOURCE_DIR}/port/stm32f1/STM32F103XB_FLASH.ld")
elseif(OW_TARGET STREQUAL "f0")
    set(CMSIS_DEVICE_REPO  "https://github.com/STMicroelectronics/cmsis_device_f0.git")
    set(CMSIS_DEVICE_GIT_TAG "${OW_CMSIS_DEVICE_GIT_TAG_F0}")
    set(CMSIS_DEVICE_HEADER "stm32f0xx.h")
    set(CMSIS_DEVICE_DEF  "STM32F030x6")
    set(OW_PORT_DEF       "OW_PORT_TARGET_F0")
    set(MCU_FLAGS         -mcpu=cortex-m0 -mthumb)
    set(STARTUP_ASM       "startup_stm32f030x6.s")
    set(SYSTEM_SRC        "system_stm32f0xx.c")
    set(LINKER_SCRIPT     "${CMAKE_CURRENT_SOURCE_DIR}/port/stm32f0/STM32F030X6_FLASH.ld")
elseif(OW_TARGET STREQUAL "g0")
    set(CMSIS_DEVICE_REPO  "https://github.com/STMicroelectronics/cmsis_device_g0.git")
    set(CMSIS_DEVICE_GIT_TAG "${OW_CMSIS_DEVICE_GIT_TAG_G0}")
    set(CMSIS_DEVICE_HEADER "stm32g0xx.h")
    set(CMSIS_DEVICE_DEF  "STM32G031xx")
    set(OW_PORT_DEF       "OW_PORT_TARGET_G0")
    set(MCU_FLAGS         -mcpu=cortex-m0plus -mthumb)
    set(STARTUP_ASM       "startup_stm32g031xx.s")
    set(SYSTEM_SRC        "system_stm32g0xx.c")
    set(LINKER_SCRIPT     "${CMAKE_CURRENT_SOURCE_DIR}/port/stm32g0/STM32G031X6_FLASH.ld")
else()
    message(FATAL_ERROR "OW_TARGET must be f1, f0, or g0 (got: ${OW_TARGET})")
endif()

# ---------------------------------------------------------------------------
# Resolve CMSIS: cache/find_path first, FetchContent only as fallback
# ---------------------------------------------------------------------------
if(NOT OW_CMSIS_CORE_INCLUDE)
    find_path(OW_CMSIS_CORE_INCLUDE
        NAMES cmsis_gcc.h
        PATHS
            ${CMAKE_CURRENT_SOURCE_DIR}/CMSIS/core   # `make download-deps` layout
            ${CMAKE_CURRENT_SOURCE_DIR}/CMSIS/Core/Include
        PATH_SUFFIXES CMSIS/Core/Include Include
        DOC "Directory containing cmsis_gcc.h (CMSIS Core headers)"
        # Cross toolchains set FIND_ROOT_PATH_MODE_INCLUDE=ONLY; these paths
        # live on the host / in the source tree, so bypass the root filter.
        NO_CMAKE_FIND_ROOT_PATH
    )
endif()

if(NOT OW_CMSIS_DEVICE_INCLUDE)
    find_path(OW_CMSIS_DEVICE_INCLUDE
        NAMES ${CMSIS_DEVICE_HEADER}
        PATHS
            ${CMAKE_CURRENT_SOURCE_DIR}/CMSIS/device # `make download-deps` layout
        PATH_SUFFIXES
            Include
            CMSIS/Device/ST/STM32F1xx/Include
            CMSIS/Device/ST/STM32F0xx/Include
            CMSIS/Device/ST/STM32G0xx/Include
            Drivers/CMSIS/Device/ST/STM32F1xx/Include
            Drivers/CMSIS/Device/ST/STM32F0xx/Include
            Drivers/CMSIS/Device/ST/STM32G0xx/Include
        DOC "Directory containing ${CMSIS_DEVICE_HEADER} (ST device headers)"
        NO_CMAKE_FIND_ROOT_PATH
    )
endif()

set(OW_FETCHED_DEVICE OFF)

if(NOT OW_CMSIS_CORE_INCLUDE OR NOT OW_CMSIS_DEVICE_INCLUDE)
    if(NOT OW_FETCH_CMSIS)
        message(FATAL_ERROR
            "stm32-async-1wire: CMSIS headers not found.\n"
            "  Set -DOW_CMSIS_CORE_INCLUDE=<dir-with-cmsis_gcc.h> and\n"
            "      -DOW_CMSIS_DEVICE_INCLUDE=<dir-with-${CMSIS_DEVICE_HEADER}>,\n"
            "  or enable -DOW_FETCH_CMSIS=ON to download them.")
    endif()

    include(FetchContent)

    if(NOT OW_CMSIS_CORE_INCLUDE)
        FetchContent_Declare(cmsis_core
            GIT_REPOSITORY https://github.com/ARM-software/CMSIS_5.git
            GIT_TAG        ${OW_CMSIS_CORE_GIT_TAG}
            GIT_SHALLOW    TRUE
            GIT_PROGRESS   TRUE
        )
        FetchContent_MakeAvailable(cmsis_core)
        set(OW_CMSIS_CORE_INCLUDE "${cmsis_core_SOURCE_DIR}/CMSIS/Core/Include")
    endif()

    if(NOT OW_CMSIS_DEVICE_INCLUDE)
        FetchContent_Declare(cmsis_device
            GIT_REPOSITORY ${CMSIS_DEVICE_REPO}
            GIT_TAG        ${CMSIS_DEVICE_GIT_TAG}
            GIT_SHALLOW    TRUE
            GIT_PROGRESS   TRUE
        )
        FetchContent_MakeAvailable(cmsis_device)
        set(OW_CMSIS_DEVICE_INCLUDE "${cmsis_device_SOURCE_DIR}/Include")
        set(OW_FETCHED_DEVICE ON)
    endif()
elseif(OW_FETCH_CMSIS AND OW_BUILD_EXAMPLES)
    # Headers already provided, but examples need startup/system templates:
    # fetch only the device package (not the core) when its tree is absent.
    # The `make download-deps` layout keeps startup/system flat next to the
    # headers — that needs no fetch at all.
    get_filename_component(_ow_dev_parent "${OW_CMSIS_DEVICE_INCLUDE}" DIRECTORY)
    if(EXISTS "${OW_CMSIS_DEVICE_INCLUDE}/${STARTUP_ASM}"
       OR EXISTS "${OW_CMSIS_DEVICE_INCLUDE}/Source/Templates/gcc/${STARTUP_ASM}"
       OR EXISTS "${_ow_dev_parent}/Source/Templates/gcc/${STARTUP_ASM}")
        # already have templates locally
    else()
        include(FetchContent)
        FetchContent_Declare(cmsis_device
            GIT_REPOSITORY ${CMSIS_DEVICE_REPO}
            GIT_TAG        ${CMSIS_DEVICE_GIT_TAG}
            GIT_SHALLOW    TRUE
            GIT_PROGRESS   TRUE
        )
        FetchContent_MakeAvailable(cmsis_device)
        set(OW_FETCHED_DEVICE ON)
        set(_ow_dev_parent "${cmsis_device_SOURCE_DIR}")
    endif()
endif()

# Examples also need startup + system sources from the device package.
if(OW_BUILD_EXAMPLES)
    if(EXISTS "${OW_CMSIS_DEVICE_INCLUDE}/${STARTUP_ASM}")
        # Flat layout: make download-deps (startup_*.s + system_*.c in CMSIS/device)
        set(CMSIS_STARTUP_DIR "${OW_CMSIS_DEVICE_INCLUDE}")
        set(CMSIS_SYSTEM_DIR  "${OW_CMSIS_DEVICE_INCLUDE}")
    elseif(OW_FETCHED_DEVICE)
        set(CMSIS_STARTUP_DIR "${cmsis_device_SOURCE_DIR}/Source/Templates/gcc")
        set(CMSIS_SYSTEM_DIR  "${cmsis_device_SOURCE_DIR}/Source/Templates")
    elseif(EXISTS "${_ow_dev_parent}/Source/Templates/gcc/${STARTUP_ASM}")
        set(CMSIS_STARTUP_DIR "${_ow_dev_parent}/Source/Templates/gcc")
        set(CMSIS_SYSTEM_DIR  "${_ow_dev_parent}/Source/Templates")
    else()
        message(FATAL_ERROR
            "stm32-async-1wire: startup/system sources not found for examples.\n"
            "  Need ${STARTUP_ASM} under the CMSIS device tree, or run\n"
            "  `make download-deps`, or configure with -DOW_FETCH_CMSIS=ON.")
    endif()
endif()

# ---------------------------------------------------------------------------
# Library targets (code only — no startup/system, no syscall.c)
# ---------------------------------------------------------------------------

# src/ds18b20.c is an amalgamated translation unit: the search / txn /
# resolution / measurement code lives in these include-only parts (guarded by
# DS18B20_DRIVER_BUILD). Mark them as header-only-sources so the IDE shows the
# split while only ds18b20.c itself is compiled into the library.
set_source_files_properties(
    src/internal/ds18b20_resolution.c
    src/internal/ds18b20_txn.c
    src/internal/ds18b20_search.c
    src/internal/ds18b20_measure.c
    PROPERTIES HEADER_FILE_ONLY TRUE
)

# Build a core library target <tgt> from the supplied driver sources.
#
# Visibility rules:
#   PUBLIC  — include dirs, device/port defines: consumers of the static lib
#             compile headers that need them.
#   PUBLIC  — MCU arch flags: required again at link time (multilib select).
#   PRIVATE — -Os / -Wall / -Wextra / section-gc flags: build hygiene for this
#             target only; never forced onto the consumer's translation units.
function(ow_add_core_library tgt)
    add_library(${tgt} STATIC ${ARGN})
    target_include_directories(${tgt} PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/port>
        $<INSTALL_INTERFACE:include/stm32-async-1wire>
        # Device/CMSIS headers are required to compile the library (ow_port.h
        # pulls in stm32f1xx.h).  BUILD_INTERFACE: the _deps / cache paths must
        # not leak into install exports — consumers of the installed lib provide
        # their own CMSIS/device headers (see stm32_async_1wireConfig.cmake).
        $<BUILD_INTERFACE:${OW_CMSIS_CORE_INCLUDE}>
        $<BUILD_INTERFACE:${OW_CMSIS_DEVICE_INCLUDE}>
    )
    target_compile_definitions(${tgt} PUBLIC
        ${CMSIS_DEVICE_DEF}
        ${OW_PORT_DEF}
    )
    target_compile_options(${tgt} PUBLIC
        ${MCU_FLAGS}
    )
    target_compile_options(${tgt} PRIVATE
        -Os
        -Wall
        -Wextra
        -ffunction-sections
        -fdata-sections
    )
endfunction()

# The plain library is always built without OW_STATS_ENABLE: the stats module
# is inert (its body and the inline ow_stats_* hooks compile away).  The
# example build below adds a dedicated stats-enabled variant for 6_statistics
# only.
ow_add_core_library(stm32_async_1wire
    src/onewire.c
    src/ds18b20.c
    src/ow_stats.c
)

# ---------------------------------------------------------------------------
# Install targets
# ---------------------------------------------------------------------------
include(GNUInstallDirs)

install(TARGETS stm32_async_1wire
    EXPORT stm32_async_1wireTargets
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
)

install(FILES
    include/stm32_async_1wire.h
    include/version.h
    include/onewire.h
    include/ds18b20.h
    include/ow_port.h
    include/ow_stats.h
    include/ow_config.h
    include/ow_bits.h  # included by every ow_port_<family>.h backend
    # ow_port.h selects its backend with sibling quote-includes
    # (#include "ow_port_f1.h"), so the backends must ship alongside it.
    port/ow_port_f1.h
    port/ow_port_f0.h
    port/ow_port_g0.h
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/stm32-async-1wire
)

install(EXPORT stm32_async_1wireTargets
    FILE stm32_async_1wireTargets.cmake
    NAMESPACE stm32_async_1wire::
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/stm32_async_1wire
)

# --- Config file for find_package() ---
include(CMakePackageConfigHelpers)

configure_package_config_file(
    ${CMAKE_CURRENT_SOURCE_DIR}/cmake/stm32_async_1wireConfig.cmake.in
    ${CMAKE_CURRENT_BINARY_DIR}/stm32_async_1wireConfig.cmake
    INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/stm32_async_1wire
)

write_basic_package_version_file(
    ${CMAKE_CURRENT_BINARY_DIR}/stm32_async_1wireConfigVersion.cmake
    VERSION ${PROJECT_VERSION}
    COMPATIBILITY SameMajorVersion
)

install(FILES
    ${CMAKE_CURRENT_BINARY_DIR}/stm32_async_1wireConfig.cmake
    ${CMAKE_CURRENT_BINARY_DIR}/stm32_async_1wireConfigVersion.cmake
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/stm32_async_1wire
)

# ---------------------------------------------------------------------------
# Convenience: print selected target
# ---------------------------------------------------------------------------
message(STATUS "stm32-async-1wire: OW_TARGET=${OW_TARGET}  "
               "CMSIS_DEVICE_DEF=${CMSIS_DEVICE_DEF}  "
               "OW_PORT_DEF=${OW_PORT_DEF}  "
               "OW_CMSIS_CORE_INCLUDE=${OW_CMSIS_CORE_INCLUDE}  "
               "OW_CMSIS_DEVICE_INCLUDE=${OW_CMSIS_DEVICE_INCLUDE}")

# ---------------------------------------------------------------------------
# Examples (opt-in with -DOW_BUILD_EXAMPLES=ON; top-level builds only)
# ---------------------------------------------------------------------------
if(OW_BUILD_EXAMPLES)
    # Complete platform: startup, system, linker script, syscall.c
    add_library(stm32_async_1wire_platform STATIC
        src/syscall.c
        ${CMSIS_STARTUP_DIR}/${STARTUP_ASM}
        ${CMSIS_SYSTEM_DIR}/${SYSTEM_SRC}
    )
    target_include_directories(stm32_async_1wire_platform PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${CMAKE_CURRENT_SOURCE_DIR}/port
        ${OW_CMSIS_CORE_INCLUDE}
        ${OW_CMSIS_DEVICE_INCLUDE}
    )
    target_compile_definitions(stm32_async_1wire_platform PUBLIC
        ${CMSIS_DEVICE_DEF}
        ${OW_PORT_DEF}
    )
    target_compile_options(stm32_async_1wire_platform PUBLIC
        ${MCU_FLAGS}
    )
    target_compile_options(stm32_async_1wire_platform PRIVATE
        -Os
        -Wall
        -Wextra
        -ffunction-sections
        -fdata-sections
    )
    target_link_options(stm32_async_1wire_platform PUBLIC
        ${MCU_FLAGS}
        -T${LINKER_SCRIPT}
        -Wl,--gc-sections
        --specs=nosys.specs
        --specs=nano.specs
    )

    # 6_statistics is the only example that uses the stats module.  Rebuild
    # only the DS18B20 driver and stats implementation with stats enabled;
    # it reuses onewire.c from the plain library when linked below.
    ow_add_core_library(stm32_async_1wire_stats
        src/ds18b20.c
        src/ow_stats.c
    )
    target_compile_definitions(stm32_async_1wire_stats PUBLIC
        OW_STATS_ENABLE=1
        DS18B20_CYCLE_PAUSE_US=10000
    )

    # Example applications — mirror the make APP=<ex> targets: every example
    # links the shared examples/app/app.c platform layer and gets the same
    # per-example compile definitions as the Makefile.
    set(EXAMPLES 1_basic 2_device_search 3_round_robin 4_scan_mode
                 5_commands 6_statistics 7_low_power)
    set(UART_TX_SIZE_1_basic         128)
    set(UART_TX_SIZE_2_device_search 256)
    set(UART_TX_SIZE_3_round_robin   256)
    set(UART_TX_SIZE_4_scan_mode     256)
    set(UART_TX_SIZE_5_commands      256)
    set(UART_TX_SIZE_6_statistics    1024)
    set(UART_TX_SIZE_7_low_power     256)
    foreach(ex ${EXAMPLES})
        add_executable(${ex}
            examples/${ex}/main.c
            examples/app/app.c
        )
        if(ex STREQUAL "6_statistics")
            target_link_libraries(${ex} PRIVATE
                stm32_async_1wire_stats
                stm32_async_1wire
                stm32_async_1wire_platform
            )
            # Mirror `make APP=6_statistics`.
            target_compile_definitions(${ex} PRIVATE
                STATS_DUMP_INTERVAL=5000
            )
        else()
            target_link_libraries(${ex} PRIVATE
                stm32_async_1wire
                stm32_async_1wire_platform
            )
        endif()
        target_include_directories(${ex} PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}/examples/app
        )
        target_compile_definitions(${ex} PRIVATE
            UART_TX_BUF_SIZE=${UART_TX_SIZE_${ex}}
        )
    endforeach()
endif()
