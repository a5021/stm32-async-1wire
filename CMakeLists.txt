cmake_minimum_required(VERSION 3.14)
project(stm32_async_1wire C ASM)

# ---------------------------------------------------------------------------
# Options
# ---------------------------------------------------------------------------
set(OW_TARGET "f1" CACHE STRING "MCU family: f1 (default), f0, g0")
set_property(CACHE OW_TARGET PROPERTY STRINGS f1 f0 g0)

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
    set(CMSIS_DEVICE_DEF  "STM32F103xB")
    set(OW_PORT_DEF       "OW_PORT_TARGET_F1")
    set(MCU_FLAGS          -mcpu=cortex-m3 -mthumb)
    set(STARTUP_ASM        "startup_stm32f103xb.s")
    set(SYSTEM_SRC         "system_stm32f1xx.c")
    set(LINKER_SCRIPT      "${CMAKE_CURRENT_SOURCE_DIR}/port/stm32f1/STM32F103XB_FLASH.ld")
elseif(OW_TARGET STREQUAL "f0")
    set(CMSIS_DEVICE_REPO "https://github.com/STMicroelectronics/cmsis_device_f0.git")
    set(CMSIS_DEVICE_DEF  "STM32F030x6")
    set(OW_PORT_DEF       "OW_PORT_TARGET_F0")
    set(MCU_FLAGS          -mcpu=cortex-m0 -mthumb)
    set(STARTUP_ASM        "startup_stm32f030x6.s")
    set(SYSTEM_SRC         "system_stm32f0xx.c")
    set(LINKER_SCRIPT      "${CMAKE_CURRENT_SOURCE_DIR}/port/stm32f0/STM32F030X6_FLASH.ld")
elseif(OW_TARGET STREQUAL "g0")
    set(CMSIS_DEVICE_REPO "https://github.com/STMicroelectronics/cmsis_device_g0.git")
    set(CMSIS_DEVICE_DEF  "STM32G031xx")
    set(OW_PORT_DEF       "OW_PORT_TARGET_G0")
    set(MCU_FLAGS          -mcpu=cortex-m0plus -mthumb)
    set(STARTUP_ASM        "startup_stm32g031xx.s")
    set(SYSTEM_SRC         "system_stm32g0xx.c")
    set(LINKER_SCRIPT      "${CMAKE_CURRENT_SOURCE_DIR}/port/stm32g0/STM32G031X6_FLASH.ld")
else()
    message(FATAL_ERROR "OW_TARGET must be f1, f0, or g0 (got: ${OW_TARGET})")
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
# Library target
# ---------------------------------------------------------------------------
add_library(stm32_async_1wire STATIC
    src/onewire.c
    src/ds18b20.c
    src/ow_stats.c
    ${CMSIS_STARTUP_DIR}/${STARTUP_ASM}
    ${CMSIS_SYSTEM_DIR}/${SYSTEM_SRC}
)

target_include_directories(stm32_async_1wire PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/inc
    ${CMAKE_CURRENT_SOURCE_DIR}/port
    ${CMAKE_CURRENT_SOURCE_DIR}/port/stm32f1
    ${CMAKE_CURRENT_SOURCE_DIR}/port/stm32f0
    ${CMAKE_CURRENT_SOURCE_DIR}/port/stm32g0
    ${CMSIS_CORE_INCLUDE}
    ${CMSIS_DEVICE_INCLUDE}
)

target_compile_definitions(stm32_async_1wire PUBLIC
    ${CMSIS_DEVICE_DEF}
    ${OW_PORT_DEF}
)

target_compile_options(stm32_async_1wire PUBLIC
    ${MCU_FLAGS}
    -Os
    -Wall
    -Wextra
    -ffunction-sections
    -fdata-sections
)

target_link_options(stm32_async_1wire PUBLIC
    ${MCU_FLAGS}
    -T${LINKER_SCRIPT}
    -Wl,--gc-sections
    --specs=nosys.specs
    --specs=nano.specs
)

# ---------------------------------------------------------------------------
# Convenience: print selected target
# ---------------------------------------------------------------------------
message(STATUS "stm32-async-1wire: OW_TARGET=${OW_TARGET}  "
               "CMSIS_DEVICE_DEF=${CMSIS_DEVICE_DEF}  "
               "OW_PORT_DEF=${OW_PORT_DEF}")
