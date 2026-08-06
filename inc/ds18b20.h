/**
 * @file ds18b20.h
 * @brief Non-blocking DS18B20 temperature sensor driver for STM32F103
 * 
 * This driver implements a strictly non-blocking interface for the DS18B20 
 * temperature sensor using hardware timers and DMA on STM32F103 microcontrollers.
 * 
 * Key features:
 * - Pure bare-metal, register-level programming
 * - No interrupts; no software delays or busy-waits in the measurement path
 * - Hardware timer-based timing with DMA for data capture
 * - Non-blocking state machine architecture
 * - Weak function callbacks for customization
 * - Optional blocking startup scan (ds18b20_search_devices) to enumerate
 *   all sensors on the bus and read their 64-bit ROM addresses
 * 
 * Usage:
 * 1. Call ds18b20_init() once at startup
 * 2. (Optional) Call ds18b20_search_devices() to enumerate bus devices
 * 3. (Optional) Call ds18b20_select() to measure one specific device
 * 4. Call ds18b20_poll() repeatedly from main loop
 * 5. Implement weak callbacks ds18b20_busy() and ds18b20_complete()
 *    to handle status indication and temperature results
 */

#ifndef DS18B20_H
#define DS18B20_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @defgroup DS18B20_Exported_Constants DS18B20 Exported Constants
 * @{
 */

/**
 * @brief Special error values (0.1°C units, outside -550..1250 range)
 * @note These values are outside the normal temperature range to indicate errors
 */
#define DS18B20_TEMP_ERROR_GENERIC INT16_MIN /**< Generic/unspecified error */
#define DS18B20_TEMP_ERROR_NO_SENSOR (INT16_MIN + 1) /**< No sensor detected on bus */
#define DS18B20_TEMP_ERROR_CRC_FAIL (INT16_MIN + 2) /**< CRC checksum validation failed */

/**
 * @brief DS18B20 1-Wire family code (LSB of the 64-bit ROM address)
 * @note Other 1-Wire devices on the bus use different family codes and are
 *       skipped by the device search.
 */
#define DS18B20_FAMILY_CODE 0x28

/**
 * @}
 */

/**
 * @defgroup DS18B20_Exported_Functions DS18B20 Exported Functions
 * @{
 */

/**
 * @brief Initialize DS18B20 driver hardware and peripherals
 */
void ds18b20_init(void);

/**
 * @brief Advance the state machine (non-blocking)
 * @note Call periodically from main loop
 * 
 * This function implements the core non-blocking state machine that manages
 * the 1-Wire communication protocol with the DS18B20 sensor. It uses hardware
 * timer and DMA to handle timing-critical operations without software delays.
 */
void ds18b20_poll(void);

/**
 * @brief Enumerate all DS18B20 devices on the 1-Wire bus (blocking)
 * @param[in] sink Callback invoked once per found DS18B20 device with its
 *                 64-bit ROM address (LSB first). May be NULL to only count
 *                 devices. Return a non-zero value from the callback to stop
 *                 the search early; the device is still counted in the return
 *                 value.
 * @param[in] max_devices Maximum number of devices to report (0 aborts the
 *                        search and returns 0)
 * @return Number of DS18B20 devices found on the bus
 * @note BLOCKING function: it busy-waits on the timer update flag for the
 *       whole search (~15 ms per device). Intended for one-time use at startup
 *       before the main loop starts polling. It reuses the same hardware-timed
 *       1-Wire primitives as the state machine but does not touch the
 *       non-blocking measurement path. The Search ROM (0xF0) algorithm
 *       enumerates every 1-Wire device on the bus; only devices whose ROM
 *       family code is DS18B20_FAMILY_CODE (0x28) are reported, other devices
 *       are silently skipped. After the search, pass one of the reported ROM
 *       addresses to ds18b20_select() to measure that device; otherwise the
 *       measurement path keeps using Skip-ROM (single-sensor) addressing.
 */
uint8_t ds18b20_search_devices(uint8_t (*sink)(const uint8_t* rom), uint8_t max_devices);

/**
 * @brief Select which DS18B20 device to measure by its ROM address
 * @param[in] rom Pointer to the 8-byte ROM address (LSB first), or NULL to
 *                return to Skip ROM (broadcast) addressing
 * @note With a non-NULL address, the state machine sends Match ROM (0x55)
 *       plus the device address before each command, so only that device
 *       responds. Pass NULL to keep the legacy single-sensor Skip ROM
 *       behaviour. The address should come from ds18b20_search_devices().
 */
void ds18b20_select(const uint8_t* rom);

/**
 * @brief Busy indicator callback (weak)
 * @param[in] action 0 = idle, non-zero = busy
 */
void ds18b20_busy(unsigned action);

/**
 * @brief Measurement complete callback (weak)
 * @param[in] temp Temperature in tenths of degrees Celsius, or error code
 */
void ds18b20_complete(int16_t temp);

/**
 * @}
 */

#ifdef __cplusplus
}
#endif

#endif // DS18B20_H
