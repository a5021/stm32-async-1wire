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
 * - Low-level blocking primitives (ds18b20_reset, ds18b20_write_bit, etc.)
 *   for custom 1-Wire protocols such as device search
 * 
 * Usage:
 * 1. Call ds18b20_init() once at startup
 * 2. Call ds18b20_poll() repeatedly from main loop
 * 3. (Optional) Use low-level primitives for custom protocols; call
 *    ds18b20_restore() before returning to poll()
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
 * @defgroup DS18B20_LowLevel Low-Level Blocking 1-Wire Primitives
 * @brief Blocking primitives for custom 1-Wire protocols (e.g., device search).
 * @warning These busy-wait on hardware completion. They use the same TIM1/DMA
 *          as the non-blocking state machine and MUST NOT be called while
 *          polling is active. After using these, call ds18b20_restore()
 *          before starting ds18b20_poll().
 * @{
 */

/**
 * @brief Perform a blocking 1-Wire reset and check for a presence pulse
 * @return 1 if at least one device answered the reset, 0 otherwise
 */
uint8_t ds18b20_reset(void);

/**
 * @brief Write one bit to the 1-Wire bus as a single hardware-timed slot
 * @param[in] bit 1 = short low pulse (~5µs), 0 = long low pulse (~60µs)
 */
void ds18b20_write_bit(uint8_t bit);

/**
 * @brief Read one bit from the 1-Wire bus as a single hardware-timed slot
 * @return The bit value read (0 or 1)
 */
uint8_t ds18b20_read_bit(void);

/**
 * @brief Write one byte to the 1-Wire bus, LSB first
 * @param[in] byte Byte value to transmit
 */
void ds18b20_write_byte(uint8_t byte);

/**
 * @brief Read one byte from the 1-Wire bus, LSB first
 * @return The byte value read
 */
uint8_t ds18b20_read_byte(void);

/**
 * @brief Restore the non-blocking state machine after using low-level primitives
 * @note Call this after finishing low-level operations and before starting
 *       ds18b20_poll(). It re-primes the state machine so the first poll()
 *       begins a measurement cycle.
 */
void ds18b20_restore(void);

/**
 * @brief Calculate Dallas/Maxim CRC-8 over a byte buffer
 * @param[in] data Input buffer
 * @param[in] len Number of bytes to process
 * @return CRC-8 checksum value
 */
uint8_t ds18b20_crc8(const uint8_t* data, uint8_t len);

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
