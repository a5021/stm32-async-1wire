cmake_minimum_required(VERSION 3.14)
project(fetchcontent_consumer C)

if(NOT OW_SOURCE_DIR)
    message(FATAL_ERROR "Set OW_SOURCE_DIR to the stm32-async-1wire source tree")
endif()

file(TO_CMAKE_PATH "${OW_SOURCE_DIR}" OW_SOURCE_DIR)
include(FetchContent)
FetchContent_Declare(stm32_async_1wire SOURCE_DIR "${OW_SOURCE_DIR}")
FetchContent_MakeAvailable(stm32_async_1wire)

add_library(consumer STATIC main.c)
target_link_libraries(consumer PRIVATE stm32_async_1wire)
