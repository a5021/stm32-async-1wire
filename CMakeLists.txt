cmake_minimum_required(VERSION 3.14)
project(stm32_async_1wire VERSION 1.8.1 LANGUAGES C ASM)

# ---------------------------------------------------------------------------
# Options
# ---------------------------------------------------------------------------
set(OW_TARGET "f1" CACHE STRING "MCU family: f1 (default), f0, g0, f4")
set_property(CACHE OW_TARGET PROPERTY STRINGS f1 f0 g0 f4)
# Part within the family, named after the chips/<part>.mk file the Makefile
# build uses. Both build systems read the same part file, so they cannot drift
# on the device macro, startup file, linker script or clock default.
set(OW_CHIP "" CACHE STRING "Part within OW_TARGET (e.g. f103xb, f030x6, g031xx, f407xx, f401xc, f401xe); empty = the family's default")
set(OW_SYSCLK_MHZ "" CACHE STRING "System clock in MHz; empty = the part's own default from chips/<part>.mk (mirrors the Makefile's SYSCLK_MHZ)")
# The CMake counterpart of the Makefile's EXT="...": extra -D definitions applied
# to every library and example, so a CMake build can select the same firmware
# variants without CMakeLists.txt growing one option per feature.
#   cmake -DOW_EXTRA_DEFINES="-DOW_PARASITE_POWER=1;-DOW_PORT_LOW_POWER=1" ...
set(OW_EXTRA_DEFINES "" CACHE STRING "Extra definitions applied to all targets, e.g. \"-DOW_PARASITE_POWER=1;-DOW_PORT_LOW_POWER=1\"")

option(OW_BUILD_EXAMPLES "Build example applications" OFF)

# ---------------------------------------------------------------------------
# FetchContent: ARM CMSIS Core + STM32 device headers
# ---------------------------------------------------------------------------
include(FetchContent)

FetchContent_Declare(cmsis_core
    GIT_REPOSITORY https://github.com/ARM-software/CMSIS_5.git
    GIT_TAG        master
    GIT_SHALLOW    TRUE
    GIT_PROGRESS   TRUE
)
FetchContent_MakeAvailable(cmsis_core)

# CMSIS Core headers live under CMSIS/Core/Include in the repo
set(CMSIS_CORE_INCLUDE "${cmsis_core_SOURCE_DIR}/CMSIS/Core/Include")

# Device-specific repo and defines
if(OW_TARGET STREQUAL "f1")
    set(CMSIS_DEVICE_REPO "https://github.com/STMicroelectronics/cmsis_device_f1.git")
    set(OW_PORT_DEF       "OW_PORT_TARGET_F1")
    set(MCU_FLAGS          -mcpu=cortex-m3 -mthumb)
    set(SYSTEM_SRC         "system_stm32f1xx.c")
elseif(OW_TARGET STREQUAL "f0")
    set(CMSIS_DEVICE_REPO "https://github.com/STMicroelectronics/cmsis_device_f0.git")
    set(OW_PORT_DEF       "OW_PORT_TARGET_F0")
    set(MCU_FLAGS          -mcpu=cortex-m0 -mthumb)
    set(SYSTEM_SRC         "system_stm32f0xx.c")
elseif(OW_TARGET STREQUAL "g0")
    set(CMSIS_DEVICE_REPO "https://github.com/STMicroelectronics/cmsis_device_g0.git")
    set(OW_PORT_DEF       "OW_PORT_TARGET_G0")
    set(MCU_FLAGS          -mcpu=cortex-m0plus -mthumb)
    set(SYSTEM_SRC         "system_stm32g0xx.c")
elseif(OW_TARGET STREQUAL "f4")
    set(CMSIS_DEVICE_REPO "https://github.com/STMicroelectronics/cmsis_device_f4.git")
    set(OW_PORT_DEF       "OW_PORT_TARGET_F4")
    set(MCU_FLAGS          -mcpu=cortex-m4 -mthumb)
    set(SYSTEM_SRC         "system_stm32f4xx.c")
else()
    message(FATAL_ERROR "OW_TARGET must be f1, f0, g0 or f4 (got: ${OW_TARGET})")
endif()

# ---------------------------------------------------------------------------
# Part selection: read chips/<part>.mk, the same file the Makefile build uses
# ---------------------------------------------------------------------------
# The part carries what the *family* does not: the CMSIS device macro, the
# startup file, the linker script and the default system clock. Reading one
# file for both build systems is what keeps them from drifting apart — an
# unknown part is a hard error here exactly as it is in the Makefile.
set(OW_CHIP_DIR "${CMAKE_CURRENT_SOURCE_DIR}/chips")
file(GLOB _ow_chip_files "${OW_CHIP_DIR}/*.mk")
set(OW_KNOWN_CHIPS "")
foreach(_f IN LISTS _ow_chip_files)
    get_filename_component(_n "${_f}" NAME_WE)
    list(APPEND OW_KNOWN_CHIPS "${_n}")
endforeach()

if(OW_TARGET STREQUAL "f0")
    set(_ow_default_chip f030x6)
elseif(OW_TARGET STREQUAL "g0")
    set(_ow_default_chip g031xx)
elseif(OW_TARGET STREQUAL "f4")
    set(_ow_default_chip f407xx)
else()
    set(_ow_default_chip f103xb)
endif()
if(NOT OW_CHIP)
    set(OW_CHIP "${_ow_default_chip}")
endif()
if(NOT OW_CHIP IN_LIST OW_KNOWN_CHIPS)
    message(FATAL_ERROR
        "OW_CHIP='${OW_CHIP}' has no chips/${OW_CHIP}.mk. "
        "Known parts: ${OW_KNOWN_CHIPS}")
endif()

# Parse the flat "NAME = value" assignments. CMake cannot include() a makefile,
# which is why the part files are kept to plain assignments with no includes,
# conditionals or multi-line values.
set(CHIP_DEV_DEF "")
set(CHIP_DEVICE_HDR "")
set(CHIP_STARTUP "")
set(CHIP_LINKER "")
set(CHIP_JFLASH "")
set(CHIP_JDEBUG "")
set(CHIP_SVD "")
set(CHIP_SYSCLK_MHZ "")
file(STRINGS "${OW_CHIP_DIR}/${OW_CHIP}.mk" _ow_chip_lines)
foreach(_line IN LISTS _ow_chip_lines)
    if(_line MATCHES "^[ \t]*(CHIP_[A-Z_]+)[ \t]*=[ \t]*(.*)$")
        set(_k "${CMAKE_MATCH_1}")
        string(REGEX REPLACE "#.*$" "" _v "${CMAKE_MATCH_2}")
        string(STRIP "${_v}" _v)
        set(${_k} "${_v}")
    endif()
endforeach()
foreach(_k CHIP_DEV_DEF CHIP_DEVICE_HDR CHIP_STARTUP CHIP_LINKER
           CHIP_JFLASH CHIP_JDEBUG CHIP_SVD CHIP_SYSCLK_MHZ)
    if(NOT ${_k})
        message(FATAL_ERROR
            "chips/${OW_CHIP}.mk does not define ${_k}; the part file and this "
            "CMakeLists.txt have drifted apart")
    endif()
endforeach()
if(NOT EXISTS "${OW_CHIP_DIR}/${OW_CHIP}.mk")
    message(FATAL_ERROR "chips/${OW_CHIP}.mk does not exist")
endif()
foreach(_p "${CHIP_LINKER}" "${CHIP_JFLASH}" "${CHIP_JDEBUG}")
    if(NOT EXISTS "${_p}")
        message(FATAL_ERROR
            "chips/${OW_CHIP}.mk points at a file that does not exist: ${_p}")
    endif()
endforeach()

# Fold the part into the family-level values the rest of this file consumes.
string(REGEX REPLACE "^-D" "" _ow_dev "${CHIP_DEV_DEF}")
set(CMSIS_DEVICE_DEF "${_ow_dev}")
get_filename_component(STARTUP_ASM "${CHIP_STARTUP}" NAME)
# The part files spell repo paths relative to the repository root, which is what
# the Makefile build wants (it runs from there). The linker resolves -T against
# its own working directory - the build tree - so hand it an absolute path.
get_filename_component(LINKER_SCRIPT "${CHIP_LINKER}" ABSOLUTE
                       BASE_DIR "${CMAKE_CURRENT_SOURCE_DIR}")
if(OW_SYSCLK_MHZ)
    set(OW_PORT_SYSCLK_MHZ "${OW_SYSCLK_MHZ}")
else()
    set(OW_PORT_SYSCLK_MHZ "${CHIP_SYSCLK_MHZ}")
endif()

FetchContent_Declare(cmsis_device
    GIT_REPOSITORY ${CMSIS_DEVICE_REPO}
    GIT_TAG        master
    GIT_SHALLOW    TRUE
    GIT_PROGRESS   TRUE
)
FetchContent_MakeAvailable(cmsis_device)

# Device headers and sources live under Include/ and Source/Templates/ in the ST repos
set(CMSIS_DEVICE_INCLUDE "${cmsis_device_SOURCE_DIR}/Include")
set(CMSIS_DEVICE_DIR     "${cmsis_device_SOURCE_DIR}")
set(CMSIS_STARTUP_DIR    "${cmsis_device_SOURCE_DIR}/Source/Templates/gcc")
set(CMSIS_SYSTEM_DIR     "${cmsis_device_SOURCE_DIR}/Source/Templates")

# ---------------------------------------------------------------------------
# Optimization profiles
# ---------------------------------------------------------------------------
# Read from config/optim.mk, which the Makefile includes directly, so there is
# exactly one copy of the flags in the repository. CMake cannot include() a
# makefile, so the file is parsed with the same flat "NAME = value" reader the
# chips/<part>.mk files use below: plain assignments, no includes, no
# conditionals, no multi-line values, no trailing comments on a value line.
set(OW_OPTIM_MK "${CMAKE_CURRENT_SOURCE_DIR}/config/optim.mk")
if(NOT EXISTS "${OW_OPTIM_MK}")
    message(FATAL_ERROR "optimization profiles not found: ${OW_OPTIM_MK}")
endif()
file(STRINGS "${OW_OPTIM_MK}" _ow_optim_lines)
set(OPT_RELEASE "")
set(OPT_DEBUG "")
set(OPT_NO_LTO "")
foreach(_line IN LISTS _ow_optim_lines)
    if(_line MATCHES "^[ \t]*(OPT_[A-Z_]+)[ \t]*=[ \t]*(.*)$")
        # Copy both captures out before any string() call: string(REGEX REPLACE)
        # overwrites CMAKE_MATCH_*, so reading CMAKE_MATCH_1 afterwards would
        # return the replace operation's (empty) match instead of the key.
        set(_k "${CMAKE_MATCH_1}")
        set(_v "${CMAKE_MATCH_2}")
        string(REGEX REPLACE "#.*$" "" _v "${_v}")
        string(STRIP "${_v}" _v)
        set(${_k} "${_v}")
    endif()
endforeach()
foreach(_k OPT_RELEASE OPT_DEBUG OPT_NO_LTO)
    if(NOT ${_k})
        message(FATAL_ERROR "${OW_OPTIM_MK} does not define ${_k}")
    endif()
endforeach()
string(REPLACE " " ";" OPT_NO_LTO "${OPT_NO_LTO}")

# LTO is not restated here: it is part of the profile, and the exclusion below
# filters it back out with the same list the Makefile uses. Keyed by OW_TARGET
# in both build systems; the OW_TARGET -> -mcpu mapping is 1:1 (f0 cortex-m0,
# g0 cortex-m0plus, f1 cortex-m3, f4 cortex-m4), so keying on the family cannot
# disagree with the architecture.
if(OW_TARGET IN_LIST OPT_NO_LTO)
    foreach(_p OPT_RELEASE OPT_DEBUG)
        # Drop the token, then collapse the whitespace it leaves behind. Matching
        # the surrounding separators as part of the pattern would eat both of
        # them and glue the neighbouring flags together ("-Os -flto -g0" would
        # become "-Os-g0", which the compiler reads as one unknown option).
        string(REGEX REPLACE "-flto" "" ${_p} "${${_p}}")
        string(REGEX REPLACE "[ \t]+" " " ${_p} "${${_p}}")
        string(STRIP "${${_p}}" ${_p})
    endforeach()
    set(OW_LTO_FLAGS "")
else()
    set(OW_LTO_FLAGS "-flto")
endif()

# LTO archives need the GCC-aware archiver: with a plain `ar` the archive index
# has no LTO symbols, so the link fails with "undefined reference to
# ds18b20_init" even though the archive contains it. CMake normally guesses
# `arm-none-eabi-ar`, so point it at `arm-none-eabi-gcc-ar` when that is what is
# available next to the compiler. Only done when LTO is actually on.
#
# The parentheses are required: in if(), the unary NOT binds tighter than
# MATCHES, so `NOT CMAKE_AR MATCHES "gcc-ar"` parses as
# `(NOT CMAKE_AR) MATCHES "gcc-ar"` and is always false.
#
# The comparison is on the file NAME, not the whole path: toolchains are often
# installed under a directory like .../gcc-arm-none-eabi/current/bin/, and the
# substring "gcc-ar" occurs in that directory name - matching the full path
# would decide the plain archiver is already the GCC-aware one.
get_filename_component(_ow_ar_name "${CMAKE_AR}" NAME)
if(OW_LTO_FLAGS AND NOT (_ow_ar_name MATCHES "gcc-ar"))
    get_filename_component(_ow_bin_dir "${CMAKE_C_COMPILER}" DIRECTORY)
    find_program(_ow_gcc_ar NAMES arm-none-eabi-gcc-ar HINTS "${_ow_bin_dir}")
    if(_ow_gcc_ar)
        message(STATUS "stm32-async-1wire: using ${_ow_gcc_ar} for LTO archives (was ${CMAKE_AR})")
        set(CMAKE_AR "${_ow_gcc_ar}")
        find_program(_ow_gcc_ranlib NAMES arm-none-eabi-gcc-ranlib HINTS "${_ow_bin_dir}")
        if(_ow_gcc_ranlib)
            set(CMAKE_RANLIB "${_ow_gcc_ranlib}")
        endif()
    else()
        message(WARNING
            "LTO is enabled but arm-none-eabi-gcc-ar was not found next to "
            "${CMAKE_C_COMPILER}; linking a static library will fail with "
            "undefined references. Pass -DCMAKE_AR=/path/to/arm-none-eabi-gcc-ar.")
    endif()
endif()
# CMake's own defaults are not used: Release would be "-O3 -DNDEBUG", and
# neither half is wanted here. -O3 is not what the hardware numbers were taken
# with, and NDEBUG is a deliberate separate mode in this project (it changes
# assert() behaviour, which tests/test/test_param_guard.c exists to cover).
set(CMAKE_C_FLAGS_RELEASE         "${OPT_RELEASE}")
set(CMAKE_C_FLAGS_DEBUG           "${OPT_DEBUG}")
# RelWithDebInfo and MinSizeRel are CMake-only and unused by this project and
# its CI; derive both from the release profile instead of inventing a third and
# fourth flag set. RelWithDebInfo keeps the debug info its name promises.
set(CMAKE_C_FLAGS_RELWITHDEBINFO  "${OPT_RELEASE} -g -gdwarf")
set(CMAKE_C_FLAGS_MINSIZEREL      "${OPT_RELEASE}")

# A single-config generator leaves CMAKE_BUILD_TYPE empty, and CMake then adds no
# optimisation flags at all - the old build worked only because -Os was
# hardcoded into the targets. Default to Release so a bare `cmake -B build`
# produces the same image as `make`.
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE "Release" CACHE STRING "Build type" FORCE)
    message(STATUS "stm32-async-1wire: CMAKE_BUILD_TYPE not set, defaulting to Release")
endif()

# ---------------------------------------------------------------------------
# Library targets (code only — no startup/system, no syscall.c)
# ---------------------------------------------------------------------------

# src/ds18b20.c is an amalgamated translation unit: the search / txn /
# resolution / measurement code lives in these include-only parts (guarded by
# DS18B20_DRIVER_BUILD). Mark them as header-only-sources so the IDE shows the
# split while only ds18b20.c itself is compiled into the library.
set_source_files_properties(
    src/ds18b20_resolution.c
    src/ds18b20_txn.c
    src/ds18b20_search.c
    src/ds18b20_measure.c
    PROPERTIES HEADER_FILE_ONLY TRUE
)

# Build a core library target <tgt> from the supplied driver sources.
function(ow_add_core_library tgt)
    add_library(${tgt} STATIC ${ARGN})
    target_include_directories(${tgt} PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/inc>
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/port>
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/port/stm32f1>
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/port/stm32f0>
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/port/stm32g0>
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/port/stm32f4>
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/port/common>
        $<INSTALL_INTERFACE:include/stm32-async-1wire>
        # Device/CMSIS headers are required to compile the library (ow_port.h
        # pulls in stm32f1xx.h).  BUILD_INTERFACE: the _deps paths live in the
        # build tree and must not leak into install exports — consumers of the
        # installed lib provide their own CMSIS/device headers.
        $<BUILD_INTERFACE:${CMSIS_CORE_INCLUDE}>
        $<BUILD_INTERFACE:${CMSIS_DEVICE_INCLUDE}>
    )
    target_compile_definitions(${tgt} PUBLIC
        ${CMSIS_DEVICE_DEF}
        ${OW_PORT_DEF}
        # The part's default clock, so this build system resolves the same
        # OW_PORT_SYSCLK_MHZ the Makefile build does instead of relying on the
        # fallback in onewire.h.
        OW_PORT_SYSCLK_MHZ=${OW_PORT_SYSCLK_MHZ}
        ${OW_EXTRA_DEFINES}
    )
    target_compile_options(${tgt} PUBLIC
        ${MCU_FLAGS}
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
    inc/onewire.h
    inc/ds18b20.h
    inc/ow_port.h
    inc/ow_stats.h
    inc/ow_config.h
    inc/ow_bits.h  # included by every ow_port_<family>.h backend
    # ow_port.h selects its backend with sibling quote-includes
    # (#include "ow_port_f1.h"), so the backends must ship alongside it.
    port/stm32f1/ow_port_f1.h
    port/stm32f0/ow_port_f0.h
    port/stm32g0/ow_port_g0.h
    port/stm32f4/ow_port_f4.h
    # Shared TIM1/DMA1 core: ow_port_f0.h, ow_port_f1.h and ow_port_g0.h
    # include it, so it must be installed alongside them.
    port/common/ow_port_tim_dma.h
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
               "OW_CHIP=${OW_CHIP}  "
               "CMSIS_DEVICE_DEF=${CMSIS_DEVICE_DEF}  "
               "OW_PORT_DEF=${OW_PORT_DEF}  "
               "OW_PORT_SYSCLK_MHZ=${OW_PORT_SYSCLK_MHZ}")

# ---------------------------------------------------------------------------
# Examples (opt-in with -DOW_BUILD_EXAMPLES=ON)
# ---------------------------------------------------------------------------
if(OW_BUILD_EXAMPLES)
    # Complete platform: startup, system, linker script, syscall.c
    add_library(stm32_async_1wire_platform STATIC
        src/syscall.c
        ${CMSIS_STARTUP_DIR}/${STARTUP_ASM}
        ${CMSIS_SYSTEM_DIR}/${SYSTEM_SRC}
    )
    target_include_directories(stm32_async_1wire_platform PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/inc
        ${CMAKE_CURRENT_SOURCE_DIR}/port
        ${CMAKE_CURRENT_SOURCE_DIR}/port/stm32f1
        ${CMAKE_CURRENT_SOURCE_DIR}/port/stm32f0
        ${CMAKE_CURRENT_SOURCE_DIR}/port/stm32g0
        ${CMAKE_CURRENT_SOURCE_DIR}/port/stm32f4
        ${CMSIS_CORE_INCLUDE}
        ${CMSIS_DEVICE_INCLUDE}
    )
    target_compile_definitions(stm32_async_1wire_platform PUBLIC
        ${CMSIS_DEVICE_DEF}
        ${OW_PORT_DEF}
        OW_PORT_SYSCLK_MHZ=${OW_PORT_SYSCLK_MHZ}
        ${OW_EXTRA_DEFINES}
    )
    target_compile_options(stm32_async_1wire_platform PUBLIC
        ${MCU_FLAGS}
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
    # objcopy next to the compiler, so the .hex/.bin pair matches what `make all`
    # produces and a programmer can be pointed at either.
    get_filename_component(OW_TOOLCHAIN_BIN_DIR "${CMAKE_C_COMPILER}" DIRECTORY)
    find_program(OW_OBJCOPY
        NAMES arm-none-eabi-objcopy
        HINTS "${OW_TOOLCHAIN_BIN_DIR}"
        DOC "Path to arm-none-eabi-objcopy, used to emit .hex/.bin per example"
    )
    if(NOT OW_OBJCOPY)
        message(FATAL_ERROR
            "arm-none-eabi-objcopy not found next to ${CMAKE_C_COMPILER}. "
            "The example build needs it to produce .hex/.bin; pass "
            "-DOW_OBJCOPY=/path/to/arm-none-eabi-objcopy to point at one.")
    endif()
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
            ${OW_EXTRA_DEFINES}
        )
        # `make all` emits elf + hex + bin; mirror that so flashing does not
        # need a manual objcopy. BYPRODUCTS keeps the files visible to ninja and
        # to `cmake --build ... --target clean`.
        set(OW_EXE_DIR "${CMAKE_CURRENT_BINARY_DIR}/examples")
        add_custom_command(TARGET ${ex} POST_BUILD
            COMMAND "${CMAKE_COMMAND}" -E make_directory "${OW_EXE_DIR}"
            COMMAND "${OW_OBJCOPY}" -O ihex
                    "$<TARGET_FILE:${ex}>" "${OW_EXE_DIR}/${ex}.hex"
            COMMAND "${OW_OBJCOPY}" -O binary -S
                    "$<TARGET_FILE:${ex}>" "${OW_EXE_DIR}/${ex}.bin"
            BYPRODUCTS "${OW_EXE_DIR}/${ex}.hex" "${OW_EXE_DIR}/${ex}.bin"
            COMMENT "objcopy: ${ex}.hex / ${ex}.bin"
            VERBATIM
        )
    endforeach()
endif()
