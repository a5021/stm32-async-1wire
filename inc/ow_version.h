/**
 * @file ow_version.h
 * @brief Library version (semantic versioning).
 *
 * The single source of the project version: tests/check_version.sh reads
 * these four macros and compares every other declaration site (CMake
 * project(), the PlatformIO manifest, Doxygen, the README FetchContent tag,
 * the CHANGELOG links and the release workflow) against them. A release bump
 * edits this file; nothing else defines the version.
 */

#ifndef OW_VERSION_H
#define OW_VERSION_H

/** @brief Library version (semantic versioning) */
#define STM32_ASYNC_1WIRE_VERSION_MAJOR 2
#define STM32_ASYNC_1WIRE_VERSION_MINOR 4
#define STM32_ASYNC_1WIRE_VERSION_PATCH 0
#define STM32_ASYNC_1WIRE_VERSION_STRING "2.4.0"

#endif /* OW_VERSION_H */
