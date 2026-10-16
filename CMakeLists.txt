cmake_minimum_required(VERSION 3.14)
project(stm32_async_1wire VERSION 2.3.0 LANGUAGES C ASM)

# ---------------------------------------------------------------------------
# Options
# ---------------------------------------------------------------------------
set(OW_MCU "" CACHE STRING "Exact STMicroelectronics device macro (e.g. STM32F030x6, STM32F103xB, STM32H503xx); empty = use OW_TARGET/OW_CHIP")
set_property(CACHE OW_MCU PROPERTY STRINGS STM32F030x6 STM32F103xB STM32F303xC STM32F407xx STM32F401xC STM32F401xE STM32F446xx STM32G031xx STM32G474xx STM32H503xx)
set(OW_TARGET "" CACHE STRING "MCU family: f0, f1, f3, f4, g0, g4, h5; empty = derived from OW_MCU, otherwise f1")
set_property(CACHE OW_TARGET PROPERTY STRINGS f0 f1 f3 f4 g0 g4 h5)
# Part within the family, named after the chips/<part>.mk file the Makefile
# build uses. Both build systems read the same part file, so they cannot drift
# on the device macro, startup file, linker script or clock default.
set(OW_CHIP "" CACHE STRING "Part within OW_TARGET (e.g. f030x6, f103xb, f303xc, f407xx, f401xc, f401xe, f446xx, g031xx, g474cb, h503cb); empty = derived from OW_MCU, otherwise the family's default")
set(OW_SYSCLK_MHZ "" CACHE STRING "System clock in MHz; empty = the part's own default from chips/<part>.mk (mirrors the Makefile's SYSCLK_MHZ)")
# The CMake counterpart of the Makefile's EXT="...": extra -D definitions applied
# to every library and example, so a CMake build can select the same firmware
# variants without CMakeLists.txt growing one option per feature.
#   cmake -DOW_EXTRA_DEFINES="-DOW_PARASITE_POWER=1;-DOW_PORT_LOW_POWER=1" ...
set(OW_EXTRA_DEFINES "" CACHE STRING "Extra definitions applied to all targets, e.g. \"-DOW_PARASITE_POWER=1;-DOW_PORT_LOW_POWER=1\"")

option(OW_BUILD_EXAMPLES "Build example applications" OFF)

# Exact STMicroelectronics device macros supported by chips/<part>.mk. This is
# the same table the Makefile uses for OW_MCU; ow_mcu_selection() keeps the
# macro -> family/part mapping in one place for both directions below.
set(OW_KNOWN_MCUS STM32F030x6 STM32F103xB STM32F303xC STM32F407xx STM32F401xC STM32F401xE STM32F446xx STM32G031xx STM32G474xx STM32H503xx)
function(ow_mcu_selection macro out_target out_chip)
    if(macro STREQUAL "STM32F030x6")
        set(${out_target} "f0" PARENT_SCOPE)
        set(${out_chip} "f030x6" PARENT_SCOPE)
    elseif(macro STREQUAL "STM32F103xB")
        set(${out_target} "f1" PARENT_SCOPE)
        set(${out_chip} "f103xb" PARENT_SCOPE)
    elseif(macro STREQUAL "STM32F303xC")
        set(${out_target} "f3" PARENT_SCOPE)
        set(${out_chip} "f303xc" PARENT_SCOPE)
    elseif(macro STREQUAL "STM32F407xx")
        set(${out_target} "f4" PARENT_SCOPE)
        set(${out_chip} "f407xx" PARENT_SCOPE)
    elseif(macro STREQUAL "STM32F401xC")
        set(${out_target} "f4" PARENT_SCOPE)
        set(${out_chip} "f401xc" PARENT_SCOPE)
    elseif(macro STREQUAL "STM32F401xE")
        set(${out_target} "f4" PARENT_SCOPE)
        set(${out_chip} "f401xe" PARENT_SCOPE)
    elseif(macro STREQUAL "STM32F446xx")
        set(${out_target} "f4" PARENT_SCOPE)
        set(${out_chip} "f446xx" PARENT_SCOPE)
    elseif(macro STREQUAL "STM32G031xx")
        set(${out_target} "g0" PARENT_SCOPE)
        set(${out_chip} "g031xx" PARENT_SCOPE)
    elseif(macro STREQUAL "STM32G474xx")
        set(${out_target} "g4" PARENT_SCOPE)
        set(${out_chip} "g474cb" PARENT_SCOPE)
    elseif(macro STREQUAL "STM32H503xx")
        set(${out_target} "h5" PARENT_SCOPE)
        set(${out_chip} "h503cb" PARENT_SCOPE)
    else()
        set(${out_target} "" PARENT_SCOPE)
        set(${out_chip} "" PARENT_SCOPE)
    endif()
endfunction()

# When OW_MCU names one exact device, it determines the legacy selectors. Any
# nonempty legacy value must agree; CMake caches OW_TARGET/OW_CHIP, so use a
# fresh build directory (or matching values) when changing devices.
if(OW_MCU)
    if(NOT OW_MCU IN_LIST OW_KNOWN_MCUS)
        message(FATAL_ERROR "OW_MCU='${OW_MCU}' is not a known MCU. Use one of: ${OW_KNOWN_MCUS}")
    endif()
    ow_mcu_selection("${OW_MCU}" _ow_mcu_target _ow_mcu_chip)
    if(NOT _ow_mcu_target OR NOT _ow_mcu_chip)
        message(FATAL_ERROR "internal error: OW_MCU='${OW_MCU}' has no target/part mapping")
    endif()
    if(OW_TARGET AND NOT OW_TARGET STREQUAL _ow_mcu_target)
        message(FATAL_ERROR "OW_TARGET='${OW_TARGET}' conflicts with OW_MCU='${OW_MCU}' (expected '${_ow_mcu_target}')")
    endif()
    if(OW_CHIP AND NOT OW_CHIP STREQUAL _ow_mcu_chip)
        message(FATAL_ERROR "OW_CHIP='${OW_CHIP}' conflicts with OW_MCU='${OW_MCU}' (expected '${_ow_mcu_chip}')")
    endif()
    set(OW_TARGET "${_ow_mcu_target}")
    set(OW_CHIP "${_ow_mcu_chip}")
endif()
if(NOT OW_TARGET)
    set(OW_TARGET "f1")
endif()

# ---------------------------------------------------------------------------
# Unified MCU → core/flags table (shared with cmake/ow_add_library.cmake)
# ---------------------------------------------------------------------------
# Derives OW_FAMILY / OW_CPU / OW_FAMILY_DEF / OW_MCU_FLAGS / OW_SYSTEM_SRC /
# OW_DEVICE_REPO from OW_MCU (device or family macro), falling back to OW_TARGET
# when OW_MCU is unset. This is the single source of truth so the two build
# systems can never disagree about which core flags a given MCU gets.
include(${CMAKE_CURRENT_SOURCE_DIR}/cmake/ow_flags.cmake)

# ---------------------------------------------------------------------------
# FetchContent: ARM CMSIS Core + STM32 device headers
# ---------------------------------------------------------------------------
# Pinned SHAs come from config/cmsis_pins.mk, which the Makefile includes
# directly, so there is exactly one copy of the pins in the repository. Parsed
# with the same flat "NAME = value" reader as config/optim.mk and the
# chips/<part>.mk files: plain assignments, no includes, no conditionals,
# no multi-line values, no trailing comments on a value line.
set(OW_CMSIS_PINS_MK "${CMAKE_CURRENT_SOURCE_DIR}/config/cmsis_pins.mk")
if(NOT EXISTS "${OW_CMSIS_PINS_MK}")
    message(FATAL_ERROR "CMSIS pins not found: ${OW_CMSIS_PINS_MK}")
endif()
file(STRINGS "${OW_CMSIS_PINS_MK}" _ow_pins_lines)
foreach(_line IN LISTS _ow_pins_lines)
    if(_line MATCHES "^[ \t]*(CMSIS_[A-Z_0-9]+)[ \t]*=[ \t]*(.*)$")
        set(_k "${CMAKE_MATCH_1}")
        set(_v "${CMAKE_MATCH_2}")
        string(REGEX REPLACE "#.*$" "" _v "${_v}")
        string(STRIP "${_v}" _v)
        set(${_k} "${_v}")
    endif()
endforeach()
foreach(_k CMSIS_CORE_REF CMSIS_DEVICE_REF_F0 CMSIS_DEVICE_REF_F1
           CMSIS_DEVICE_REF_F3 CMSIS_DEVICE_REF_F4 CMSIS_DEVICE_REF_G0
           CMSIS_DEVICE_REF_G4 CMSIS_DEVICE_REF_H5)
    if(NOT ${_k})
        message(FATAL_ERROR "${OW_CMSIS_PINS_MK} does not define ${_k}")
    endif()
endforeach()
include(FetchContent)

FetchContent_Declare(cmsis_core
    GIT_REPOSITORY https://github.com/ARM-software/CMSIS_5.git
    GIT_TAG        ${CMSIS_CORE_REF}
    GIT_SHALLOW    TRUE
    GIT_PROGRESS   TRUE
)
FetchContent_MakeAvailable(cmsis_core)

# CMSIS Core headers live under CMSIS/Core/Include in the repo
set(CMSIS_CORE_INCLUDE "${cmsis_core_SOURCE_DIR}/CMSIS/Core/Include")

# Device-specific repo and defines — from cmake/ow_flags.cmake (included above).
set(CMSIS_DEVICE_REPO "${OW_DEVICE_REPO}")
set(OW_FAMILY_DEF     "${OW_FAMILY_DEF}")
set(MCU_FLAGS         "${OW_MCU_FLAGS}")
set(SYSTEM_SRC        "${OW_SYSTEM_SRC}")

# The device checkout follows the pinned SHA from config/cmsis_pins.mk, so a
# fresh clone resolves byte-identical headers years later. The per-family key
# matches the pins file (CMSIS_DEVICE_REF_<FAMILY>, uppercased OW_TARGET).
string(TOUPPER "${OW_TARGET}" _ow_target_up)
set(CMSIS_DEVICE_TAG "${CMSIS_DEVICE_REF_${_ow_target_up}}")
if(NOT CMSIS_DEVICE_TAG)
    message(FATAL_ERROR "config/cmsis_pins.mk does not define CMSIS_DEVICE_REF_${_ow_target_up}")
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
elseif(OW_TARGET STREQUAL "f3")
    set(_ow_default_chip f303xc)
elseif(OW_TARGET STREQUAL "f4")
    set(_ow_default_chip f407xx)
elseif(OW_TARGET STREQUAL "g0")
    set(_ow_default_chip g031xx)
elseif(OW_TARGET STREQUAL "g4")
    set(_ow_default_chip g474cb)
elseif(OW_TARGET STREQUAL "h5")
    set(_ow_default_chip h503cb)
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
# The part file is authoritative for its own CMSIS macro. For known exact
# macros, require the selected family to match the part, even when OW_MCU was
# not used. Unknown/custom part files keep the previous behaviour.
string(REGEX REPLACE "^-D" "" _ow_part_macro "${CHIP_DEV_DEF}")
ow_mcu_selection("${_ow_part_macro}" _ow_part_target _ow_part_chip)
if(_ow_part_target AND NOT _ow_part_target STREQUAL OW_TARGET)
    message(FATAL_ERROR "OW_CHIP='${OW_CHIP}' belongs to OW_TARGET='${_ow_part_target}', not OW_TARGET='${OW_TARGET}'")
endif()
# CHIP_JFLASH and CHIP_SVD are optional, excluded from the empty-check because
# check_chips.sh already treats them that way (CHIP_JFLASH is in OPTIONAL_PATHS,
# CHIP_SVD is called informational): a part can have no J-Flash project (the H5
# JTAG CoreID is unverified) and no SVD upstream (cmsis-svd-data carries no
# STM32H5 files). The Makefile handles both blank - JFLASH only feeds the J-Link
# jprogram path, CHIP_SVD only EXTERNAL_DEPS - so requiring them here drifted
# from the repo-side checklist, and faking a non-empty value that pointed at
# nothing would be worse than an honest blank.
foreach(_k CHIP_DEV_DEF CHIP_DEVICE_HDR CHIP_STARTUP CHIP_LINKER
           CHIP_JDEBUG CHIP_SYSCLK_MHZ)
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
    # CHIP_JFLASH may be blank by design (no J-Flash project), so skip the
    # existence check for it - an empty path must not be treated as missing.
    if(_p AND NOT EXISTS "${_p}")
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
    GIT_TAG        ${CMSIS_DEVICE_TAG}
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
foreach(_k OPT_RELEASE OPT_DEBUG)
    if(NOT ${_k})
        message(FATAL_ERROR "${OW_OPTIM_MK} does not define ${_k}")
    endif()
endforeach()
# OPT_NO_LTO is allowed to be empty: an empty list means LTO stays on for
# every family. It keeps its initialized empty value when optim.mk leaves it
# blank, so removing the last excluded family needs no other edit here.
string(REPLACE " " ";" OPT_NO_LTO "${OPT_NO_LTO}")

# LTO is not restated here: it is part of the profile, and the exclusion below
# filters it back out with the same list the Makefile uses. Keyed by OW_TARGET
# in both build systems; the OW_TARGET -> -mcpu mapping is 1:1 (f0 cortex-m0,
# g0 cortex-m0plus, f1 cortex-m3, f3/f4/g4 cortex-m4), so keying on the family
# cannot disagree with the architecture.
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

# src/ds18b20.c and src/ds2431.c are amalgamated translation units: the search /
# txn / resolution / measurement code lives in these include-only parts
# (guarded by DS18B20_DRIVER_BUILD / DS2431_DRIVER_BUILD). Mark them as
# header-only-sources so the IDE shows the split while only ds18b20.c and
# ds2431.c themselves are compiled into the library.
set_source_files_properties(
    src/ds18b20_resolution.c
    src/ds18b20_txn.c
    src/ds18b20_search.c
    src/ds18b20_measure.c
    src/ds2431_txn.c
    PROPERTIES HEADER_FILE_ONLY TRUE
)

# Build a core library target <tgt> from the supplied driver sources.
function(ow_add_core_library tgt)
    add_library(${tgt} STATIC ${ARGN})
    target_include_directories(${tgt} PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/inc>
        $<INSTALL_INTERFACE:include/stm32-async-1wire>
        # Device/CMSIS headers are required to compile the library (ow_mcu.h
        # pulls in stm32f1xx.h).  BUILD_INTERFACE: the _deps paths live in the
        # build tree and must not leak into install exports — consumers of the
        # installed lib provide their own CMSIS/device headers.
        $<BUILD_INTERFACE:${CMSIS_CORE_INCLUDE}>
        $<BUILD_INTERFACE:${CMSIS_DEVICE_INCLUDE}>
    )
    target_compile_definitions(${tgt} PUBLIC
        ${CMSIS_DEVICE_DEF}
        ${OW_FAMILY_DEF}
        # The part's default clock, so this build system resolves the same
        # OW_PORT_SYSCLK_MHZ the Makefile build does instead of relying on the
        # fallback in the family headers.
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
    src/ds2431.c
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
    inc/ds2431.h
    inc/ow_port.h
    inc/ow_mcu.h
    inc/ow_stats.h
    inc/ow_config.h
    inc/ow_bits.h  # included by ow_mcu.h (register aliases) and ow_port.h
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
message(STATUS "stm32-async-1wire: OW_MCU=${OW_MCU}  "
               "OW_TARGET=${OW_TARGET}  "
               "OW_CHIP=${OW_CHIP}  "
               "CMSIS_DEVICE_DEF=${CMSIS_DEVICE_DEF}  "
               "OW_FAMILY_DEF=${OW_FAMILY_DEF}  "
               "OW_PORT_SYSCLK_MHZ=${OW_PORT_SYSCLK_MHZ}")

# ---------------------------------------------------------------------------
# Examples (opt-in with -DOW_BUILD_EXAMPLES=ON)
# ---------------------------------------------------------------------------
if(OW_BUILD_EXAMPLES)
    # Complete platform: startup, system, linker script, syscall.c
    # (example support in examples/app/, not part of the library itself).
    add_library(stm32_async_1wire_platform STATIC
        examples/app/syscall.c
        ${CMSIS_STARTUP_DIR}/${STARTUP_ASM}
        ${CMSIS_SYSTEM_DIR}/${SYSTEM_SRC}
    )
    target_include_directories(stm32_async_1wire_platform PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/inc
        ${CMSIS_CORE_INCLUDE}
        ${CMSIS_DEVICE_INCLUDE}
    )
    target_compile_definitions(stm32_async_1wire_platform PUBLIC
        ${CMSIS_DEVICE_DEF}
        ${OW_FAMILY_DEF}
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
                 5_commands 6_statistics 7_low_power 8_ds2431)
    set(UART_TX_SIZE_1_basic         128)
    set(UART_TX_SIZE_2_device_search 256)
    set(UART_TX_SIZE_3_round_robin   256)
    set(UART_TX_SIZE_4_scan_mode     256)
    set(UART_TX_SIZE_5_commands      256)
    set(UART_TX_SIZE_6_statistics    1024)
    set(UART_TX_SIZE_7_low_power     256)
    set(UART_TX_SIZE_8_ds2431        256)
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
            # Mirror `make APP=6_statistics`: enable the stats module only. The
            # dump period is left to the example's own STATS_DUMP_SWEEPS default,
            # so it stays overridable from the command line instead of being
            # pinned to one value in two places.
            target_compile_definitions(${ex} PRIVATE
                OW_STATS_ENABLE=1
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
