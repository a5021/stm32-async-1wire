# Downstream consumer of the library as a *subproject*, which is the shape the
# README documents with FetchContent_Declare(... GIT_REPOSITORY ... GIT_TAG ...).
# Here the source comes from the local checkout instead of a clone, so the
# fixture needs no network and cannot drift from the commit under test.
#
# This path differs from installed/ in one way that matters: the library target
# still carries its BUILD_INTERFACE include directories, which include the
# FetchContent CMSIS paths (CMakeLists.txt builds them with BUILD_INTERFACE so
# they cannot leak into an install export). So this consumer needs no CMSIS of
# its own, and asking it to pass one would be testing nothing.
cmake_minimum_required(VERSION 3.14)
project(ow_consumer_fetchcontent C)

if(NOT OW_SOURCE_DIR)
    message(FATAL_ERROR
        "OW_SOURCE_DIR is not set; point it at the stm32-async-1wire checkout "
        "this consumer is meant to build against.")
endif()

include(FetchContent)

# SOURCE_DIR rather than GIT_REPOSITORY/GIT_TAG, per the comment above. The
# code path through add_subdirectory() is the same one FetchContent takes once
# it has the sources.
FetchContent_Declare(stm32_async_1wire SOURCE_DIR ${OW_SOURCE_DIR})
FetchContent_MakeAvailable(stm32_async_1wire)

add_library(ow_consumer STATIC
    ${CMAKE_CURRENT_LIST_DIR}/../ow_consumer_probe.c
)

# The un-namespaced target: add_subdirectory() yields the plain name, which is
# what the README tells downstream projects to link.
target_link_libraries(ow_consumer PRIVATE stm32_async_1wire)
