# =============================================================================
# cmake/ow_flags.cmake - unified MCU → core/flags table
# =============================================================================
#
# Single source of truth for the MCU macro → (family, CPU core, family define,
# -mcpu/-mthumb flags, CMSIS system source, CMSIS device repo) mapping. Both
# the root CMakeLists.txt (subproject/FetchContent path) and
# cmake/ow_add_library.cmake (vendored copy path) include this module so the
# two build systems can never disagree about which core flags a given MCU gets.
#
# Input variable (one of the following):
#   OW_MCU   device macro (STM32H503xx) or family macro (STM32H5)
#   OW_TARGET  family code (f0, f1, f3, f4, g0, g4, h5) - used as fallback
#              when OW_MCU is unset (root CMakeLists.txt path).
#
# Output variables:
#   OW_FAMILY      f0 | f1 | f3 | f4 | g0 | g4 | h5
#   OW_CPU         cortex-m0 | cortex-m3 | cortex-m4 | cortex-m0plus | cortex-m33
#   OW_FAMILY_DEF  STM32F0 | STM32F1 | STM32F3 | STM32F4 | STM32G0 | STM32G4 | STM32H5
#   OW_MCU_FLAGS   -mcpu=<cpu> -mthumb
#   OW_SYSTEM_SRC  system_stm32*.xx.c
#   OW_DEVICE_REPO https://github.com/STMicroelectronics/cmsis_device_*.git
#
# Fatal error if neither OW_MCU nor OW_TARGET is set, or the MCU is unknown.
# =============================================================================

# Resolve the MCU input: explicit device/family macro first, otherwise derive
# from the legacy OW_TARGET family code (root CMakeLists.txt path).
if(NOT OW_MCU AND OW_TARGET)
    if(OW_TARGET STREQUAL "f0")
        set(OW_MCU "STM32F0")
    elseif(OW_TARGET STREQUAL "f1")
        set(OW_MCU "STM32F1")
    elseif(OW_TARGET STREQUAL "f3")
        set(OW_MCU "STM32F3")
    elseif(OW_TARGET STREQUAL "f4")
        set(OW_MCU "STM32F4")
    elseif(OW_TARGET STREQUAL "g0")
        set(OW_MCU "STM32G0")
    elseif(OW_TARGET STREQUAL "g4")
        set(OW_MCU "STM32G4")
    elseif(OW_TARGET STREQUAL "h5")
        set(OW_MCU "STM32H5")
    endif()
endif()

if(NOT OW_MCU)
    message(FATAL_ERROR
        "cmake/ow_flags.cmake: OW_MCU is not set (and OW_TARGET is empty). "
        "Pass a device macro (e.g. -DOW_MCU=STM32H503xx) or a family macro "
        "(e.g. -DOW_MCU=STM32H5), or set OW_TARGET to f0/f1/f3/f4/g0/g4/h5.")
endif()

if(OW_MCU STREQUAL "STM32F030x6" OR OW_MCU STREQUAL "STM32F0")
    set(OW_FAMILY      f0)
    set(OW_CPU         cortex-m0)
    set(OW_FAMILY_DEF  STM32F0)
    set(OW_MCU_FLAGS   -mcpu=cortex-m0 -mthumb)
    set(OW_SYSTEM_SRC  system_stm32f0xx.c)
    set(OW_DEVICE_REPO https://github.com/STMicroelectronics/cmsis_device_f0.git)
elseif(OW_MCU STREQUAL "STM32F103xB" OR OW_MCU STREQUAL "STM32F1")
    set(OW_FAMILY      f1)
    set(OW_CPU         cortex-m3)
    set(OW_FAMILY_DEF  STM32F1)
    set(OW_MCU_FLAGS   -mcpu=cortex-m3 -mthumb)
    set(OW_SYSTEM_SRC  system_stm32f1xx.c)
    set(OW_DEVICE_REPO https://github.com/STMicroelectronics/cmsis_device_f1.git)
elseif(OW_MCU STREQUAL "STM32F303xC" OR OW_MCU STREQUAL "STM32F3")
    set(OW_FAMILY      f3)
    set(OW_CPU         cortex-m4)
    set(OW_FAMILY_DEF  STM32F3)
    set(OW_MCU_FLAGS   -mcpu=cortex-m4 -mthumb)
    set(OW_SYSTEM_SRC  system_stm32f3xx.c)
    set(OW_DEVICE_REPO https://github.com/STMicroelectronics/cmsis-device-f3.git)
elseif(OW_MCU STREQUAL "STM32F407xx" OR OW_MCU STREQUAL "STM32F401xC" OR
       OW_MCU STREQUAL "STM32F401xE" OR OW_MCU STREQUAL "STM32F446xx" OR
       OW_MCU STREQUAL "STM32F4")
    set(OW_FAMILY      f4)
    set(OW_CPU         cortex-m4)
    set(OW_FAMILY_DEF  STM32F4)
    set(OW_MCU_FLAGS   -mcpu=cortex-m4 -mthumb)
    set(OW_SYSTEM_SRC  system_stm32f4xx.c)
    set(OW_DEVICE_REPO https://github.com/STMicroelectronics/cmsis_device_f4.git)
elseif(OW_MCU STREQUAL "STM32G031xx" OR OW_MCU STREQUAL "STM32G0")
    set(OW_FAMILY      g0)
    set(OW_CPU         cortex-m0plus)
    set(OW_FAMILY_DEF  STM32G0)
    set(OW_MCU_FLAGS   -mcpu=cortex-m0plus -mthumb)
    set(OW_SYSTEM_SRC  system_stm32g0xx.c)
    set(OW_DEVICE_REPO https://github.com/STMicroelectronics/cmsis_device_g0.git)
elseif(OW_MCU STREQUAL "STM32G474xx" OR OW_MCU STREQUAL "STM32G4")
    set(OW_FAMILY      g4)
    set(OW_CPU         cortex-m4)
    set(OW_FAMILY_DEF  STM32G4)
    set(OW_MCU_FLAGS   -mcpu=cortex-m4 -mthumb)
    set(OW_SYSTEM_SRC  system_stm32g4xx.c)
    set(OW_DEVICE_REPO https://github.com/STMicroelectronics/cmsis_device_g4.git)
elseif(OW_MCU STREQUAL "STM32H503xx" OR OW_MCU STREQUAL "STM32H5")
    set(OW_FAMILY      h5)
    set(OW_CPU         cortex-m33)
    set(OW_FAMILY_DEF  STM32H5)
    set(OW_MCU_FLAGS   -mcpu=cortex-m33 -mthumb)
    set(OW_SYSTEM_SRC  system_stm32h5xx.c)
    set(OW_DEVICE_REPO https://github.com/STMicroelectronics/cmsis_device_h5.git)
else()
    message(FATAL_ERROR
        "cmake/ow_flags.cmake: unsupported OW_MCU '${OW_MCU}'. Pass a device "
        "macro (STM32F030x6, STM32F103xB, STM32F303xC, STM32F407xx, "
        "STM32F401xC, STM32F401xE, STM32F446xx, STM32G031xx, STM32G474xx, "
        "STM32H503xx) or a family macro (STM32F0/F1/F3/F4/G0/G4/H5).")
endif()
