/**
 * @file stm32_async_1wire.h
 * @brief Umbrella header for the stm32-async-1wire library.
 *
 * Include this single header to get the full public API:
 * version macros, compile-time configuration, the shared non-blocking
 * 1-Wire bus layer, the DS18B20 driver, the optional statistics module
 * and the port layer (needed for low-power helpers).
 *
 * @code
 * #include "stm32_async_1wire.h"
 * @endcode
 *
 * Compile the library sources onewire.c, ds18b20.c and ow_stats.c and
 * put include/ and port/ on the include path (see INTEGRATION.md).
 */
#ifndef STM32_ASYNC_1WIRE_H
#define STM32_ASYNC_1WIRE_H

#include "version.h"
#include "ow_config.h"
#include "onewire.h"
#include "ds18b20.h"
#include "ow_stats.h"
#include "ow_port.h"

#endif /* STM32_ASYNC_1WIRE_H */
