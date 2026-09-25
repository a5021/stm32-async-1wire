cmake_minimum_required(VERSION 3.14)
project(installed_consumer C)

find_package(stm32_async_1wire CONFIG REQUIRED)

add_library(consumer STATIC main.c)
target_link_libraries(consumer PRIVATE stm32_async_1wire::stm32_async_1wire)
