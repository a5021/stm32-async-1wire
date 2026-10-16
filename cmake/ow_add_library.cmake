# =============================================================================
# ow_add_library.cmake - drop-in CMake helper for vendored consumers
# =============================================================================
#
# For projects that copy the library's inc/ and src/ into their own tree (see
# the README "Vendoring" section) and drive the build with CMake. It creates a
# static library target compiling the four driver sources and attaches the
# include directory, the target/clock/feature defines and the family's -mcpu
# flags, so a consumer writes:
#
#   include(path/to/stm32-async-1wire/cmake/ow_add_library.cmake)
#   ow_add_library(stm32_async_1wire
#       MCU STM32H503xx
#       SYSCLK_MHZ 64
#       PARASITE
#       CMSIS_INCLUDE_DIRS ${CMSIS}/core ${CMSIS}/device)
#   target_link_libraries(app PRIVATE stm32_async_1wire)
#
# The flags are PUBLIC, so the consumer's own sources get the same -mcpu and
# the same device macro - a consumer TU that includes ds18b20.h must see them
# (ds18b20.h -> ow_port.h -> ow_mcu.h is self-checking and #errors otherwise).
#
# This helper does NOT fetch CMSIS. A consumer supplies its own core/device
# include directories through CMSIS_INCLUDE_DIRS (STM32Cube, PlatformIO, or a
# manual checkout). Consumers on the FetchContent/add_subdirectory path do not
# need this file at all: the library's own CMakeLists.txt already provides the
# stm32_async_1wire target with CMSIS managed.
#
# Options:
#   MCU <macro>                required; device (STM32H503xx) or family (STM32H5)
#   SYSCLK_MHZ <n>             optional; omit for the SystemCoreClock runtime path
#   PARASITE                   -DOW_PARASITE_POWER=1
#   LOW_POWER                  -DOW_PORT_LOW_POWER=1
#   ACTIVE_DRIVE               -DOW_DRIVE_ACTIVE=1
#   STATS                      -DOW_STATS_ENABLE=1 (whole target, including the
#                              library's own sources - unlike the repo's example
#                              build, which enables it for 6_statistics only)
#   CMSIS_INCLUDE_DIRS <dir>.. extra PUBLIC include dirs (your CMSIS core/device)
#   EXTRA_DEFINITIONS <def>..  extra PUBLIC compile definitions
#   SOURCE_DIR <dir>           library root (default: the parent of this file's
#                              directory, i.e. the checkout this file lives in)
#
# LTO: with the repository's own Make/CMake profiles LTO is on by default and a
# plain `ar` drops the LTO symbols from the archive index. When the GNU
# toolchain and arm-none-eabi-gcc-ar are available this helper points CMAKE_AR/
# CMAKE_RANLIB at them, the same way the root CMakeLists.txt does. Harmless
# when LTO is off.
# =============================================================================

if(NOT CMAKE_VERSION VERSION_LESS 3.14)
    # no-op: documents the minimum this file targets
endif()

# Resolve the checkout root once, at include time: CMAKE_CURRENT_LIST_FILE is
# this file while it is being processed, so its parent directory is the root
# that holds inc/ and src/.
get_filename_component(_ow_add_library_dir "${CMAKE_CURRENT_LIST_FILE}" DIRECTORY)
get_filename_component(OW_LIBRARY_ROOT "${_ow_add_library_dir}" DIRECTORY)

function(ow_add_library tgt)
    # Unified MCU → core/flags table (shared with the root CMakeLists.txt).
    # OW_SOURCE_DIR is resolved below, so the include goes here where it is
    # available.
    set(_ow_opts PARASITE LOW_POWER ACTIVE_DRIVE STATS)
    set(_ow_one  MCU SYSCLK_MHZ SOURCE_DIR)
    set(_ow_multi CMSIS_INCLUDE_DIRS EXTRA_DEFINITIONS)
    cmake_parse_arguments(OW "${_ow_opts}" "${_ow_one}" "${_ow_multi}" ${ARGN})

    if(OW_UNPARSED_ARGUMENTS)
        message(FATAL_ERROR
            "ow_add_library(${tgt}): unrecognized arguments: ${OW_UNPARSED_ARGUMENTS}")
    endif()
    if(NOT OW_MCU)
        message(FATAL_ERROR
            "ow_add_library(${tgt}): MCU is required, e.g. MCU STM32H503xx "
            "(see README, \"Vendoring\")")
    endif()
    if(NOT OW_SOURCE_DIR)
        if(DEFINED OW_LIBRARY_ROOT)
            set(OW_SOURCE_DIR "${OW_LIBRARY_ROOT}")
        else()
            message(FATAL_ERROR
                "ow_add_library(${tgt}): SOURCE_DIR not given and the library "
                "root could not be derived from this helper's location")
        endif()
    endif()

    # Unified MCU → core/flags table (shared with the root CMakeLists.txt).
    include(${OW_SOURCE_DIR}/cmake/ow_flags.cmake)

    # --- Sources --------------------------------------------------------------
    set(_ow_srcs
        "${OW_SOURCE_DIR}/src/onewire.c"
        "${OW_SOURCE_DIR}/src/ds18b20.c"
        "${OW_SOURCE_DIR}/src/ds2431.c"
        "${OW_SOURCE_DIR}/src/ow_stats.c"
    )
    foreach(_ow_s IN LISTS _ow_srcs)
        if(NOT EXISTS "${_ow_s}")
            message(FATAL_ERROR
                "ow_add_library(${tgt}): missing ${_ow_s}. Point SOURCE_DIR at "
                "the library root (the directory holding inc/ and src/).")
        endif()
    endforeach()

    # --- Definitions ----------------------------------------------------------
    set(_ow_defs "${OW_MCU}")
    if(NOT OW_FAMILY_DEF STREQUAL OW_MCU)
        list(APPEND _ow_defs "${OW_FAMILY_DEF}")
    endif()
    if(OW_SYSCLK_MHZ)
        list(APPEND _ow_defs "OW_PORT_SYSCLK_MHZ=${OW_SYSCLK_MHZ}")
    endif()
    if(OW_PARASITE)
        list(APPEND _ow_defs "OW_PARASITE_POWER=1")
    endif()
    if(OW_LOW_POWER)
        list(APPEND _ow_defs "OW_PORT_LOW_POWER=1")
    endif()
    if(OW_ACTIVE_DRIVE)
        list(APPEND _ow_defs "OW_DRIVE_ACTIVE=1")
    endif()
    if(OW_STATS)
        list(APPEND _ow_defs "OW_STATS_ENABLE=1")
    endif()
    if(OW_EXTRA_DEFINITIONS)
        list(APPEND _ow_defs ${OW_EXTRA_DEFINITIONS})
    endif()

    # --- gcc-ar for LTO (same convenience as the root CMakeLists.txt) ---------
    if(CMAKE_C_COMPILER_ID STREQUAL "GNU")
        get_filename_component(_ow_ar_name "${CMAKE_AR}" NAME)
        if(NOT _ow_ar_name MATCHES "gcc-ar")
            get_filename_component(_ow_bin_dir "${CMAKE_C_COMPILER}" DIRECTORY)
            find_program(_ow_gcc_ar NAMES arm-none-eabi-gcc-ar HINTS "${_ow_bin_dir}")
            if(_ow_gcc_ar)
                set(CMAKE_AR "${_ow_gcc_ar}" PARENT_SCOPE)
                find_program(_ow_gcc_ranlib NAMES arm-none-eabi-gcc-ranlib HINTS "${_ow_bin_dir}")
                if(_ow_gcc_ranlib)
                    set(CMAKE_RANLIB "${_ow_gcc_ranlib}" PARENT_SCOPE)
                endif()
            endif()
        endif()
    endif()

    # --- Target ---------------------------------------------------------------
    add_library(${tgt} STATIC ${_ow_srcs})

    target_include_directories(${tgt} PUBLIC
        "${OW_SOURCE_DIR}/inc"
        ${OW_CMSIS_INCLUDE_DIRS}
    )
    target_compile_definitions(${tgt} PUBLIC ${_ow_defs})
    target_compile_options(${tgt} PUBLIC
        ${OW_MCU_FLAGS}
        -Wall -Wextra
        -ffunction-sections -fdata-sections
    )
    # The same core flags on the *link* line, as INTERFACE only. A consumer that
    # builds an ELF must link with -mcpu/-mthumb too: gcc's driver picks the
    # runtime multilib from those flags, and without them it links the default
    # (ARM v4T) libc/libgcc against v8-M objects - "conflicting CPU
    # architectures". The library is static, so this never applies to ${tgt}
    # itself, only to the executables that link it.
    target_link_options(${tgt} INTERFACE ${OW_MCU_FLAGS})

    if(OW_SYSCLK_MHZ)
        message(STATUS "ow_add_library: ${tgt} (MCU=${OW_MCU}, family=${OW_FAMILY}, "
                       "cpu=${OW_CPU}, clock=${OW_SYSCLK_MHZ}MHz)")
    else()
        message(STATUS "ow_add_library: ${tgt} (MCU=${OW_MCU}, family=${OW_FAMILY}, "
                       "cpu=${OW_CPU}, clock=SystemCoreClock)")
    endif()
endfunction()
