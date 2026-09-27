# Downstream consumer of the *installed* package: find_package() against an
# install prefix, exactly as an application outside this repository would.
#
# What this proves, none of which the install step in the cmake CI job proves by
# checking that files exist: the installed headers are self-contained, the
# exported target resolves, and the public compile definitions
# (CMSIS_DEVICE_DEF / OW_PORT_TARGET_* / OW_PORT_SYSCLK_MHZ) reach the
# consumer instead of being lost at the install boundary.
#
# The probe is shared with the fetchcontent fixture so there is one definition
# of "the consumer compiles".
cmake_minimum_required(VERSION 3.14)
project(ow_consumer_installed C)

# The installed package deliberately does not carry the CMSIS include paths it
# used for its own build: those live in the build tree (FetchContent), not in
# the install tree, and hardcoding them would make the package depend on the
# paths of whichever machine built it. So a consumer supplies its own - from
# STM32Cube, PlatformIO, or a manual checkout. cmake/stm32_async_1wireConfig.cmake.in
# documents the same contract, and README repeats it.
foreach(_ow_var OW_CMSIS_CORE_INCLUDE OW_CMSIS_DEVICE_INCLUDE)
    if(NOT ${_ow_var})
        message(FATAL_ERROR
            "${_ow_var} is not set. The installed stm32_async_1wire package does "
            "not ship the CMSIS include paths, so a consumer must pass its own: "
            "-D${_ow_var}=<dir containing cmsis_gcc.h / stm32<family>xx.h>.")
    endif()
endforeach()

find_package(stm32_async_1wire REQUIRED)

add_library(ow_consumer STATIC
    ${CMAKE_CURRENT_LIST_DIR}/../ow_consumer_probe.c
)

# No COMPONENTS: a consumer that asks for a component must get an error from
# check_required_components() in the config file, which is why that call has to
# be there. Requesting one here and requiring the build to fail would be the
# negative half of this test; the positive half is the plain call above.
target_link_libraries(ow_consumer PRIVATE stm32_async_1wire::stm32_async_1wire)

target_include_directories(ow_consumer PRIVATE
    ${OW_CMSIS_CORE_INCLUDE}
    ${OW_CMSIS_DEVICE_INCLUDE}
)
