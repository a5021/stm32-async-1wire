cmake_minimum_required(VERSION 3.14)
project(add_subdirectory_consumer C)

if(NOT OW_SOURCE_DIR)
    message(FATAL_ERROR "Set OW_SOURCE_DIR to the stm32-async-1wire source tree")
endif()

add_subdirectory("${OW_SOURCE_DIR}" stm32_async_1wire)

add_library(consumer STATIC main.c)
target_link_libraries(consumer PRIVATE stm32_async_1wire)
