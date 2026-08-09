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
 * - Built-in non-blocking device search (ds18b20_search_*) for multi-sensor
 *   buses, with zero busy-waits
 * 
 * Usage:
 * 1. Call ds18b20_init() once at startup
 * 2. Call ds18b20_poll() repeatedly from main loop
 * 3. (Optional) Run the non-blocking device search (ds18b20_search_*) before
 *    starting poll(); the search helper hands back to poll() automatically
 * 4. Implement weak callbacks ds18b20_busy() and ds18b20_complete()
 *    to handle status indication and temperature results
 */

#ifndef DS18B20_H
#define DS18B20_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief DS18B20 driver state machine states
 */
typedef enum {
    DS18B20_ST_IDLE = 0,    /**< Initial state, falls through to START */
    DS18B20_ST_START,       /**< Begin measurement, reset bus */
    DS18B20_ST_CONVERT,     /**< Check presence, send Convert T command */
    DS18B20_ST_WAIT,        /**< Wait for conversion to complete (750ms) */
    DS18B20_ST_CONTINUE,    /**< Second bus reset before read */
    DS18B20_ST_REQUEST,     /**< Check presence, send Read Scratchpad command */
    DS18B20_ST_READ,        /**< Read 72 bits of scratchpad data */
    DS18B20_ST_DECODE       /**< Decode data, validate CRC, report result */
} ds18b20_state_t;

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
 * @brief Standard 8 bits per byte
 */
#define DS18B20_BITS_PER_BYTE 8

/**
 * @brief Number of bytes in a device ROM address
 */
#define DS18B20_ROM_BYTES 8

/**
 * @brief DS18B20 Search ROM command
 */
#define DS18B20_SEARCH_ROM 0xF0

/**
 * @brief DS18B20 Match ROM command
 */
#define DS18B20_MATCH_ROM 0x55

/**
 * @brief DS18B20 Convert T command
 */
#define DS18B20_CONVERT_T 0x44

/**
 * @brief DS18B20 Read Scratchpad command
 */
#define DS18B20_READ_SCRATCHPAD 0xBE

/**
 * @}
 */

/**
 * @defgroup DS18B20_Search DS18B20 Non-Blocking Device Search
 * @brief Maxim Search ROM (0xF0) state machine, driven from the main loop
 *        like the measurement state machine. It filters by family code,
 *        validates the CRC and reports each found DS18B20 via a callback.
 *        When the search finishes it hands the timer back to ds18b20_poll().
 * @{
 */

/**
 * @brief Callback invoked for every DS18B20 found by the search
 * @param[in] rom Pointer to the 8-byte ROM address (LSB first)
 * @return 0 to continue the search, non-zero to stop
 * @note The pointer is only valid for the duration of the callback.
 */
typedef uint8_t (*ds18b20_search_sink_t)(const uint8_t* rom);

/**
 * @brief Start a non-blocking device search
 * @param[in] sink Callback invoked per found DS18B20 device (may be NULL)
 * @param[in] max_devices Maximum number of devices to report (0 aborts)
 */
void ds18b20_search_start(ds18b20_search_sink_t sink, uint8_t max_devices);

/**
 * @brief Advance the non-blocking device search by one hardware operation
 * @return 1 when the search is finished, 0 while still running
 */
uint8_t ds18b20_search_poll(void);

/**
 * @brief Number of DS18B20 devices found (valid once the search finished)
 * @return Count of found devices
 */
uint8_t ds18b20_search_count(void);

/**
 * @}
 */

/**
 * @defgroup DS18B20_Exported_Functions DS18B20 Exported Functions
 * @{
 */

/**
 * @brief Calculate Dallas/Maxim CRC-8 over a byte buffer
 * @param[in] data Input buffer
 * @param[in] len Number of bytes to process
 * @return CRC-8 checksum value
 */
uint8_t ds18b20_crc8(const uint8_t* data, uint8_t len);

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
 * @brief Select which DS18B20 device to measure by its ROM address
 * @param[in] rom Pointer to the 8-byte ROM address (LSB first), or NULL to
 *                return to Skip ROM (broadcast) addressing
 * @note With a non-NULL address, the state machine sends Match ROM (0x55)
 *       plus the device address before each command, so only that device
 *       responds. Pass NULL to keep the legacy single-sensor Skip ROM
 *       behaviour. To measure a specific device, pass its ROM address
 *       (e.g., from a bus search) to ds18b20_select().
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
