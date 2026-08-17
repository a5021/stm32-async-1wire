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
    DS18B20_ST_IDLE = 0, /**< Initial state, falls through to START */
    DS18B20_ST_START, /**< Begin measurement, reset bus */
    DS18B20_ST_CONVERT, /**< Check presence, send Convert T command */
    DS18B20_ST_WAIT, /**< Wait for conversion to complete (750ms) */
    DS18B20_ST_CONTINUE, /**< Second bus reset before read */
    DS18B20_ST_REQUEST, /**< Check presence, send Read Scratchpad command */
    DS18B20_ST_READ, /**< Read 72 bits of scratchpad data */
    DS18B20_ST_DECODE /**< Decode data, validate CRC, report result */
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
 * @brief DS18B20 Alarm Search ROM command
 * @note Returns only the devices currently in alarm state, i.e. whose last
 *       measured temperature is outside the TH/TL thresholds configured with
 *       Write Scratchpad (DS18B20_WRITE_SCRATCHPAD).
 */
#define DS18B20_ALARM_SEARCH 0xEC

/**
 * @brief DS18B20 Match ROM command
 */
#define DS18B20_MATCH_ROM 0x55

/**
 * @brief DS18B20 Convert T command
 */
#define DS18B20_CONVERT_T 0x44

/**
 * @brief DS18B20 Read ROM command
 * @note Valid only when exactly one device is on the bus (no addressing).
 */
#define DS18B20_READ_ROM 0x33

/**
 * @brief DS18B20 Read Scratchpad command
 */
#define DS18B20_READ_SCRATCHPAD 0xBE

/**
 * @brief DS18B20 Write Scratchpad command
 * @note Writes TH, TL and the configuration register in one command.
 */
#define DS18B20_WRITE_SCRATCHPAD 0x4E

/**
 * @brief DS18B20 Copy Scratchpad command
 * @note Copies the scratchpad (TH/TL/CFG) into the EEPROM (non-volatile).
 *       Exposed as a non-blocking transaction (ds18b20_copy_scratchpad());
 *       the resolution state machine does not use it because the config
 *       write takes effect immediately.
 */
#define DS18B20_COPY_SCRATCHPAD 0x48

/**
 * @brief DS18B20 Recall EEPROM command
 * @note Loads the last EEPROM copy (TH/TL/CFG) into the volatile scratchpad.
 */
#define DS18B20_RECALL_EEPROM 0xB8

/**
 * @brief DS18B20 Read Power Supply command
 * @note The device drives one bit: 0 = parasite power, 1 = external power.
 */
#define DS18B20_READ_POWER_SUPPLY 0xB4

/**
 * @brief Minimum supported temperature resolution in bits
 */
#define DS18B20_RES_MIN 9

/**
 * @brief Maximum supported temperature resolution in bits
 */
#define DS18B20_RES_MAX 12

/**
 * @brief Default temperature resolution in bits (power-on default of DS18B20)
 */
#define DS18B20_RES_DEFAULT 12

/**
 * @brief Maximum number of DS18B20 devices tracked by the driver
 * @note Size of the internal device table filled by the device search; the
 *       simultaneous-conversion (scan) mode uses it to address every sensor.
 */
#ifndef DS18B20_MAX_DEVICES
#define DS18B20_MAX_DEVICES 8
#endif

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
 * @defgroup DS18B20_Alarm_Search DS18B20 Non-Blocking Alarm Search
 * @brief Maxim Alarm Search ROM (0xEC) state machine, driven from the main
 *        loop exactly like the device search. It reports only the DS18B20
 *        devices currently in alarm state (temperature outside the TH/TL
 *        thresholds set with Write Scratchpad); the regular Search ROM
 *        returns every device regardless of alarm state.
 *
 * The alarm search reuses the device search engine (same poll/ownership
 * model, same family filter, same sink protocol) and does not touch the
 * scan-mode device table: run ds18b20_search_start() to (re)populate it.
 * When it finishes it hands the timer back to ds18b20_poll().
 * @{
 */

/**
 * @brief Start a non-blocking alarm search
 * @param[in] sink Callback invoked per DS18B20 currently in alarm (may be NULL)
 * @param[in] max_devices Maximum number of alarmed devices to report (0 aborts)
 * @note Only devices in alarm state respond to Alarm Search (0xEC). The
 *       scan-mode device table is left untouched by the alarm search.
 */
void ds18b20_alarm_search_start(ds18b20_search_sink_t sink, uint8_t max_devices);

/**
 * @brief Advance the non-blocking alarm search by one hardware operation
 * @return 1 when the search is finished, 0 while still running
 */
uint8_t ds18b20_alarm_search_poll(void);

/**
 * @brief Number of DS18B20 devices found in alarm (valid once finished)
 * @return Count of alarmed devices
 */
uint8_t ds18b20_alarm_search_count(void);

/**
 * @}
 */

/**
 * @defgroup DS18B20_Resolution DS18B20 Non-Blocking Resolution Change
 * @brief Change the temperature conversion resolution (9..12 bit) between
 *        measurement cycles. The sensor answers Convert T (0x44) after
 *        93.75ms (9 bit) / 187.5ms (10 bit) / 375ms (11 bit) / 750ms
 *        (12 bit); this driver waits exactly as long as the configured
 *        resolution requires and hands the timer back to ds18b20_poll()
 *        when the change is complete.
 *
 * The configuration is written to the volatile scratchpad (Write Scratchpad
 * 0x4E): it takes effect immediately and is not persisted to the EEPROM.
 * Alarm trigger registers TH/TL are reset to 0x00 (disabled).
 * @{
 */

/**
 * @brief Start a non-blocking resolution change
 * @param[in] bits New resolution in bits: DS18B20_RES_MIN (9) .. DS18B20_RES_MAX (12)
 * @note Out-of-range values are ignored. The change is scheduled only between
 *       measurement cycles and only while the device search is idle; otherwise
 *       it is ignored. While running, it owns TIM1/DMA; poll it with
 *       ds18b20_set_resolution_poll() until it reports completion, then call
 *       ds18b20_poll() again to resume measuring with the new resolution.
 */
void ds18b20_set_resolution(uint8_t bits);

/**
 * @brief Advance the non-blocking resolution change by one hardware operation
 * @return 1 when the change is finished (successfully or aborted), 0 while running
 * @note When this returns 1 the next measurement uses the requested resolution
 *       (if the config write actually completed).
 */
uint8_t ds18b20_set_resolution_poll(void);

/**
 * @brief Current conversion resolution in bits
 * @return Resolution in bits (9..12); the default is 12
 * @note Auto-derived from the last valid scratchpad read (byte 4, R1/R0),
 *       so it also tracks a resolution changed externally.
 */
uint8_t ds18b20_get_resolution(void);

/**
 * @}
 */

/**
 * @defgroup DS18B20_Commands DS18B20 Non-Blocking Command Transactions
 * @brief Infrequent DS18B20 commands driven with the same non-blocking
 *        discipline as the device search and the resolution change: every
 *        command owns TIM1/DMA while it runs (reset -> presence -> write ->
 *        read | wait) and hands the timer back to ds18b20_poll() when done.
 *
 * Each command is a start/poll pair; poll until it returns 1, then resume
 * calling ds18b20_poll() for measurements. Commands are ignored mid-cycle,
 * while a device search or a resolution change runs, or while another
 * command transaction is still running. Result buffers passed to the read
 * commands must stay valid until the transaction finishes.
 * @{
 */

/**
 * @brief Read the 64-bit ROM of the (only) DS18B20 on the bus
 * @param[in,out] rom Buffer for the 8-byte ROM (LSB first); written on success
 * @note Valid only when exactly one device is on the bus (datasheet Read ROM
 *       0x33). With several devices use the non-blocking device search
 *       (ds18b20_search_*).
 * @note Result validity: check ds18b20_last_command_ok() or the CRC over the
 *       7 leading bytes (ds18b20_crc8(rom, 7) == rom[7]).
 */
void ds18b20_read_rom(uint8_t* rom);

/**
 * @brief Advance the non-blocking Read ROM transaction
 * @return 1 when finished (successfully or aborted), 0 while running
 */
uint8_t ds18b20_read_rom_poll(void);

/**
 * @brief Configure the alarm trigger thresholds TH and TL
 * @param[in] th High-alarm trigger value (DS18B20 8-bit threshold code)
 * @param[in] tl Low-alarm trigger value (DS18B20 8-bit threshold code)
 * @note Uses the DS18B20 8-bit sign-extended temperature code, the same
 *       encoding the scratchpad TH/TL bytes use; converting to/from Celsius is
 *       left to the application. The current conversion resolution (byte 4,
 *       R1/R0) is written unchanged, so the resolution is not disturbed.
 * @note Takes effect immediately in the scratchpad; run
 *       ds18b20_copy_scratchpad() afterwards to persist TH/TL/CFG to the
 *       EEPROM.
 */
void ds18b20_set_alarm_thresholds(uint8_t th, uint8_t tl);

/**
 * @brief Advance the non-blocking alarm threshold write
 * @return 1 when finished (successfully or aborted), 0 while running
 */
uint8_t ds18b20_set_alarm_thresholds_poll(void);

/**
 * @brief Read the 9-byte scratchpad (raw; includes TH, TL and the CRC)
 * @param[in,out] buf Buffer for the 9 scratchpad bytes (byte 0 = temp LSB,
 *                    bytes 2/3 = TH/TL, byte 8 = CRC); written on success
 * @note Result validity: check ds18b20_last_command_ok() or the CRC over the
 *       8 leading bytes (buf[8] == ds18b20_crc8(buf, 8)).
 */
void ds18b20_read_scratchpad(uint8_t* buf);

/**
 * @brief Advance the non-blocking raw scratchpad read
 * @return 1 when finished (successfully or aborted), 0 while running
 * @note On a valid read (CRC byte matches) the conversion resolution is
 *       auto-derived from the config byte (byte 4), like the measurement path.
 */
uint8_t ds18b20_read_scratchpad_poll(void);

/**
 * @brief Copy the scratchpad into the EEPROM (non-volatile)
 * @note External-power wiring only (as assumed by the whole driver): the copy
 *       is powered by the sensor's VDD, no strong pull-up is needed. The
 *       driver waits the datasheet t_COPY hold-off (10ms) before finishing.
 */
void ds18b20_copy_scratchpad(void);

/**
 * @brief Advance the non-blocking Copy Scratchpad transaction
 * @return 1 when finished (successfully or aborted), 0 while running
 */
uint8_t ds18b20_copy_scratchpad_poll(void);

/**
 * @brief Recall the EEPROM contents into the scratchpad
 * @note Loads the last EEPROM copy (TH/TL/CFG) into the volatile scratchpad.
 *       The driver waits the datasheet t_RECALL hold-off (10ms) before
 *       finishing.
 */
void ds18b20_recall_eeprom(void);

/**
 * @brief Advance the non-blocking Recall EEPROM transaction
 * @return 1 when finished (successfully or aborted), 0 while running
 */
uint8_t ds18b20_recall_eeprom_poll(void);

/**
 * @brief Read the power-supply state of the DS18B20
 * @param[in,out] is_parasite Receives 1 when the device is parasite-powered,
 *                            0 when externally powered; written on success
 */
void ds18b20_read_power_supply(uint8_t* is_parasite);

/**
 * @brief Advance the non-blocking power-supply read
 * @return 1 when finished (successfully or aborted), 0 while running
 */
uint8_t ds18b20_read_power_supply_poll(void);

/**
 * @brief Result of the last completed command transaction
 * @return 1 when the last ds18b20_*_poll() finished a transaction that found
 *         a device present (and, for read commands, read its data back),
 *         0 when it aborted (e.g. no device present) or nothing ran yet
 */
uint8_t ds18b20_last_command_ok(void);

/**
 * @}
 */

/**
 * @defgroup DS18B20_Scan DS18B20 Simultaneous Multi-Device Conversion
 * @brief Convert every discovered sensor at the same time with one broadcast
 *        Convert T (Skip ROM), then read each sensor back through Match ROM.
 *        A single conversion time covers all devices: N x 750ms becomes
 *        750ms + N x read. Each sensor's temperature is reported through
 *        ds18b20_complete() in device-table order.
 *
 * The device table is filled by the non-blocking device search
 * (ds18b20_search_*). Scan mode assumes a single resolution across all
 * sensors (the driver writes the config broadcast) and is mutually exclusive
 * with the single-device ds18b20_select() addressing.
 * @{
 */

/**
 * @brief Begin simultaneous conversion of every discovered device
 * @note Schedules a broadcast Convert T (Skip ROM) so all sensors convert in
 *       parallel; the driver then reads each one via Match ROM. Ignored
 *       mid-cycle or while a device search / resolution change owns the timer.
 * @note ds18b20_select() (single-device addressing) clears scan mode; call
 *       ds18b20_scan_start() again to resume simultaneous conversion.
 */
void ds18b20_scan_start(void);

/**
 * @brief Number of DS18B20 devices stored by the driver
 * @return Count of discovered devices (valid once the search finished)
 */
uint8_t ds18b20_device_count(void);

/**
 * @brief ROM address of a discovered device
 * @param[in] index Device index (0 .. ds18b20_device_count()-1)
 * @return Pointer to the 8-byte ROM (LSB first), or NULL for an out-of-range
 *         index
 * @note The pointer is valid until the next device search.
 */
const uint8_t* ds18b20_device_rom(uint8_t index);

/**
 * @brief Index of the device whose result ds18b20_complete() just reported
 * @return Current device index inside ds18b20_complete() during scan mode
 */
uint8_t ds18b20_scan_index(void);

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
 * @note The selection is applied only when no bus transaction is in flight
 *       (driver IDLE, or the per-device DS18B20_ST_DECODE state inside a scan
 *       callback); calls made mid-cycle are ignored. It is safe to re-select
 *       from inside the ds18b20_complete() callback: in single-device mode the
 *       driver invokes it at IDLE, and in scan mode at DECODE, where a non-NULL
 *       rom switches the driver out of scan mode to address that single device.
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
