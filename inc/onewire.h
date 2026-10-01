/**
 * @file onewire.h
 * @brief Universal non-blocking 1-Wire bus layer for STM32
 * @details The driver (ds18b20.c) and any other 1-Wire slave driver (DS2413,
 *          DS2431, ...) are built on top of this layer. Every operation is
 *          scheduled on TIM1/DMA and completes asynchronously: callers poll
 *          onewire_bus_done() / onewire_search_poll() to advance, never wait.
 *          The layer owns TIM1, its DMA1 channels and the bus GPIO pin from
 *          onewire_init() until reset (see there); access the bus only
 *          through this API.
 */

#ifndef ONEWIRE_H
#define ONEWIRE_H

#include "ow_config.h"
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @defgroup ONEWIRE_Protocol 1-Wire Protocol Constants
 * @{
 */

/** @brief Bytes in a device ROM address */
#define ONEWIRE_ROM_BYTES 8
/** @brief Bits in a device ROM address */
#define ONEWIRE_ROM_BITS (ONEWIRE_ROM_BYTES * 8)
/** @brief Bits per byte */
#define ONEWIRE_BITS_PER_BYTE 8
/** @brief Maximum bit slots per hardware operation (TIM1 RCR is 8-bit: RCR = slots - 1) */
#define ONEWIRE_MAX_SLOTS 256u
/** @brief Maximum bytes per onewire_read_data() pass (256 slots / 8 bits) */
#define ONEWIRE_MAX_READ_BYTES (ONEWIRE_MAX_SLOTS / ONEWIRE_BITS_PER_BYTE)
/** @brief Value that releases the bus (idle HIGH) at the end of a multi-slot
 *         write or merged write+read operation.
 *  @note Pulse buffers for multi-slot writes must hold `slots + 1` entries:
 *        entries [0 .. slots-1] encode the transmitted slots and the entry at
 *        index `slots` must equal this value. The CCR3-feed DMA loads it during
 *        the final slot, so the one-pulse timer stops with the line already
 *        released to idle HIGH (hardware bus release — no software CCR3 write
 *        afterwards). It is not an additional 1-Wire slot. For a single-slot
 *        write (slots == 1) the backend uses a separate path without DMA and
 *        does not read entry `slots`. */
#define ONEWIRE_RELEASE_PULSE 0u
/** @brief Family selection: a single OW_PORT_FAMILY_* token resolved from
 *  either the explicit OW_PORT_TARGET_* knob or the family macros
 *  (STM32F1, STM32F0, STM32F3, STM32G0) that PlatformIO / STM32CubeMX define on
 *  their own; for the F4 family the concrete device spellings (STM32F407xx /
 *  STM32F401xC/STM32F401xE) are accepted too. The token is the single point of
 *  family selection: it picks the port backend (included below), app.c takes
 *  the device header and clock configuration from it, and the per-family
 *  facts ow_pulse_t, the OW_PORT_SYSCLK_MHZ / OW_HSE_MHZ /
 *  OW_PORT_F4_MAX_SYSCLK_MHZ defaults and the low-power TIM1 update IRQ
 *  live in the port headers, not here, so the backend and its defaults cannot
 *  drift. To add a family, extend this chain and the \#include branch below,
 *  add the app.c config, and a case in tests/test/test_sysclk_fallback.c. */
#if defined(OW_PORT_TARGET_F1) || defined(STM32F1)
#define OW_PORT_FAMILY_F1
#elif defined(OW_PORT_TARGET_F0) || defined(STM32F0)
#define OW_PORT_FAMILY_F0
#elif defined(OW_PORT_TARGET_F3) || defined(STM32F3)
#define OW_PORT_FAMILY_F3
#elif defined(OW_PORT_TARGET_G0) || defined(STM32G0)
#define OW_PORT_FAMILY_G0
#elif defined(OW_PORT_TARGET_F4) || defined(STM32F4) || \
    defined(STM32F407xx) || defined(STM32F401xC) || defined(STM32F401xE)
#define OW_PORT_FAMILY_F4
#endif

/** @brief Pull in the selected port backend.
 *
 *  The backend is included here (not duplicated in ow_port.h) so there is one
 *  chain and one \#include branch to maintain, and so the per-family facts the
 *  backend defines are visible wherever this header is: ow_pulse_t (used by the
 *  prototypes below) and the clock defaults (used by app.c and the host tests).
 *  Keep the branches in the same order as the chain above. */
#if defined(OW_PORT_FAMILY_F1)
#include "ow_port_f1.h"
#elif defined(OW_PORT_FAMILY_F0)
#include "ow_port_f0.h"
#elif defined(OW_PORT_FAMILY_F3)
#include "ow_port_f3.h"
#elif defined(OW_PORT_FAMILY_G0)
#include "ow_port_g0.h"
#elif defined(OW_PORT_FAMILY_F4)
#include "ow_port_f4.h"
#else
#error "onewire: no family selected (define OW_PORT_TARGET_F1, OW_PORT_TARGET_F0, OW_PORT_TARGET_F3, OW_PORT_TARGET_G0 or OW_PORT_TARGET_F4, or a family macro such as STM32F1/STM32F0/STM32F3/STM32G0/STM32F4/STM32F407xx/STM32F401xC/STM32F401xE)"
#endif

/** @} */

/**
 * @defgroup ONEWIRE_Init Init
 * @{
 */

/**
 * @brief Initialize the shared 1-Wire timer/DMA/GPIO resources
 * @pre Call exactly once after MCU reset, before any 1-Wire bus operation or
 *      slave-driver activity. Initialize the system clock first when its final
 *      frequency is not the backend default.
 * @note Enables the timer/DMA/GPIO clocks, sets the timer prescaler for 1µs
 *       resolution, configures the bus pin as alternate-function open-drain
 *       and marks the search engine idle. The timer is left stopped: the first
 *       operation only starts when the application requests it.
 * @warning This is a bootstrap operation, not a reinitialization hook. A
 *          second call is unsupported: it does not stop or reset an active
 *          TIM1/DMA operation, clear stale timer/DMA/low-power state, or fully
 *          reset slave-driver state. Starting a new driver lifecycle requires
 *          an MCU reset.
 * @warning After initialization the 1-Wire layer exclusively owns TIM1, the
 *          DMA feed/capture pair of the active backend (DMA1 channels 3 and 4
 *          on F0/F1/G0, DMA2 streams 2 and 4 on F4), and the bus GPIO pin
 *          including its alternate-function/remap configuration. Application
 *          code, ISRs and other drivers must not configure or use these
 *          resources while the layer is in use. The library has no deinit or
 *          release API: the ownership lasts until reset.
 */
void onewire_init(void);

/** @} */

/**
 * @defgroup ONEWIRE_Bus Non-Blocking 1-Wire Bus Primitives
 * @brief Each primitive schedules exactly one hardware-timed operation on
 *        TIM1/DMA and returns immediately. Poll onewire_bus_done() to learn
 *        when the operation finished, then decode the result.
 * @{
 */

/**
 * @brief Non-blocking completion check for the scheduled bus operation
 * @return 1 if finished (update flag cleared), 0 while still running
 */
uint8_t onewire_bus_done(void);

/**
 * @brief Schedule a 1-Wire bus reset (presence pulse captured via DMA)
 * @param[out] reset_pulses Buffer for the captured reset + presence pulse
 *                          durations (2 x 16-bit)
 * @note On completion, decode the presence pulse with onewire_present().
 */
void onewire_reset(volatile uint16_t* reset_pulses);

/**
 * @brief Decode the presence pulse captured by onewire_reset()
 * @param[in] pulses Reset + presence pulse durations captured by onewire_reset()
 * @return 1 if at least one device answered, 0 otherwise
 */
uint8_t onewire_present(const volatile uint16_t* pulses);

/**
 * @brief Schedule a write of `slots` bit slots
 * @param[in] pulses Pulse buffer: one entry per slot, plus one trailing entry
 *                   at index `slots` that must equal ONEWIRE_RELEASE_PULSE
 *                   (hardware bus release) when `slots > 1`; in that case the
 *                   buffer therefore holds `slots + 1` entries. A single-slot
 *                   write (slots == 1) uses the DMA-free path and only reads
 *                   entry 0.
 * @param[in] slots Number of bit slots to transmit, 1..ONEWIRE_MAX_SLOTS (256).
 *                  Out-of-range values (0 or > 256) are rejected: TIM1 RCR is
 *                  8-bit (RCR = slots - 1), so larger counts would truncate and
 *                  desync the timer from the DMA (CNDTR).
 * @return 1 if the write was scheduled, 0 if `slots` is out of range (the
 *         call is rejected and no operation is scheduled). In debug builds the
 *         reject path also traps with an assert; with NDEBUG it only reports 0.
 * @note Non-blocking: the DMA feeds CCR3 from the buffer asynchronously, so
 *       the buffer must stay valid until onewire_bus_done() reports completion.
 */
uint8_t onewire_write_slots(const ow_pulse_t* pulses, uint16_t slots);

/**
 * @brief Schedule a single-slot write of one raw bit
 * @param[in] bit Bit value to write (0 or 1)
 * @return 1 (the only valid range always schedules)
 */
uint8_t onewire_write_bit(uint8_t bit);

/**
 * @brief Schedule a two-slot read of a Search ROM id/cmp bit pair
 * @param[out] pair_pulses Buffer for the captured pulse durations (2 × 16-bit)
 * @note On completion, decode the pair with onewire_pair_bits().
 */
void onewire_read_pair(volatile uint16_t* pair_pulses);

/**
 * @brief Decode the id/cmp bits of a two-slot read pair
 * @param[in] pair_pulses Pulses captured by onewire_read_pair()
 * @param[out] id_bit Id bit (0 or 1)
 * @param[out] cmp_bit Complement bit (0 or 1)
 */
void onewire_pair_bits(const volatile uint16_t* pair_pulses, uint8_t* id_bit, uint8_t* cmp_bit);

/**
 * @brief Schedule a merged single-slot write followed by a two-slot read pair
 * @param[in] bit Direction bit to write in slot 1 (0 or 1)
 * @note One timer pass runs three slots: a write of `bit`, then a read of the
 *       next id/cmp pair. Halves the timer passes per search bit compared to a
 *       plain write plus a separate read pair. On completion, the internal
 *       merged buffer holds [write-slot capture, id pulse, cmp pulse]: decode the
 *       pair from entries 1 and 2 with onewire_bit_from_pulse(). Do not pass
 *       this buffer to onewire_pair_bits(), which instead decodes entries 0 and
 *       1 as produced by onewire_read_pair().
 */
void onewire_write_then_read(uint8_t bit);

/**
 * @brief Schedule a read of `bytes` bytes from the bus
 * @param[out] dst Buffer for the captured pulse durations (bytes × 8 × 8-bit)
 * @param[in] bytes Number of bytes to read, 1..ONEWIRE_MAX_READ_BYTES (32).
 *                  Out-of-range values (0 or > 32) are rejected: one pass is
 *                  limited to ONEWIRE_MAX_SLOTS (256) slots by the 8-bit TIM1
 *                  RCR (RCR = bytes*8 - 1); larger reads must be split.
 * @return 1 if the read was scheduled, 0 if `bytes` is out of range (the call
 *         is rejected and no operation is scheduled). In debug builds the
 *         reject path also traps with an assert; with NDEBUG it only reports 0.
 * @note On completion, the caller decodes the 8-bit pulse durations (pulse
 *       `<= ONEWIRE_SHORT_PULSE_MAX` reads as bit 1) into data bytes.
 */
uint8_t onewire_read_data(volatile uint8_t* dst, uint8_t bytes);

/**
 * @brief Decode a single captured pulse duration into a 1-Wire bit
 * @param[in] dur Pulse duration in microseconds
 * @return 1 if the pulse is short (bit value '1'), 0 if long (bit value '0')
 * @note A pulse duration `<= ONEWIRE_SHORT_PULSE_MAX` is the short ('1') slot.
 */
static inline uint8_t onewire_bit_from_pulse(uint16_t dur) {
    return (dur <= ONEWIRE_SHORT_PULSE_MAX) ? 1u : 0u;
}

/**
 * @brief Decode captured pulse durations into data bytes (LSB-first bits)
 * @param[out] dst Decoded bytes
 * @param[in] pulse Captured per-bit pulse durations (one entry per bit)
 * @param[in] nbytes Number of bytes to decode (pulse must hold nbytes × 8 entries)
 * @note Each bit is recovered with onewire_bit_from_pulse(); bit 0 maps to the
 *       LSB of its byte. Recovers scratchpad / register bytes from the 1-Wire
 *       read capture.
 */
void onewire_decode_pulses(uint8_t* dst, const volatile uint8_t* pulse, uint8_t nbytes);

/**
 * @brief Encode a byte into write-pulse durations
 * @param[out] out Output buffer (8 entries)
 * @param[in] byte Byte value to encode
 */
void onewire_encode_byte(ow_pulse_t* out, uint8_t byte);

/**
 * @brief Start a hardware-timed wait with the shared one-pulse timer
 * @param[in] arr Auto-reload value (one timer period in µs)
 * @param[in] rcr Repetition counter (number of periods - 1)
 * @note The timer update event fires after (RCR + 1) × ARR microseconds; used
 *       for DS18B20 conversion waits and EEPROM hold-off windows.
 */
void onewire_start_timer(uint16_t arr, uint8_t rcr);

/**
 * @brief Raise the timer update flag so the caller's state machine advances
 * @note No hardware timing is involved: the operation this call unblocks is
 *       scheduled by the owner itself. Used by the DS18B20 driver to start one
 *       measurement cycle (ds18b20_start_measure()). Call it only while no
 *       operation is in flight.
 */
void onewire_kick(void);

/**
 * @brief Engage or release the parasite-power strong pull-up on the bus
 * @param[in] on 1 drives the bus line HIGH actively (pin leaves the timer-
 *                driven open-drain mode for a push-pull HIGH), 0 releases the
 *                line back to the passive external pull-up
 * @note Parasite-powered devices source their supply from the bus line and
 *       need this active pull-up during energy-intensive phases: the whole
 *       temperature conversion (tCONV, up to 750 ms) after Convert T and the
 *       EEPROM programming window (tPROG) after Copy Scratchpad / Recall E².
 *       Must only be called between hardware operations (bus idle), never
 *       while a timed transaction is running.
 */
void onewire_strong_pullup(uint8_t on);

/** @} */

/**
 * @defgroup ONEWIRE_Search Generic Non-Blocking Search ROM Engine
 * @brief Implements the Maxim Search ROM (0xF0) / Alarm Search (0xEC)
 *        algorithm as a polled state machine. The direction bit written at
 *        position i determines which devices keep participating at position
 *        i+1, so the transaction cannot be batched into one DMA pass; the
 *        linear loop is decomposed into states with loop counters kept in the
 *        context. Every state performs exactly one hardware-timed operation,
 *        so a poll call never blocks.
 * @{
 */

/** @brief Per-device callback invoked by the search engine */
typedef uint8_t (*onewire_search_sink_t)(const uint8_t* rom);

/**
 * @brief Start a non-blocking search (Search ROM or Alarm Search)
 * @param[in] sink Callback invoked per found device (may be NULL). A non-zero
 *                 return value stops the search early.
 * @param[in] max_devices Maximum number of devices to report (0 aborts)
 * @param[in] command Search command byte (0xF0 Search ROM / 0xEC Alarm Search)
 * @param[in] family 1-Wire family code to accept, or 0 to accept every family.
 *                   Only accepted devices increment the found counter and
 *                   reach the sink.
 * @note Ignores the call while a search pass is already running. Arbitration
 *       of the shared bus/timer against other 1-Wire operations lives in the
 *       ds18b20_* layer, not here.
 */
void onewire_search_start(onewire_search_sink_t sink, uint8_t max_devices,
                          uint8_t command, uint8_t family);

/**
 * @brief Advance the non-blocking search by one hardware operation
 * @return 1 when the search is finished, 0 while still running
 */
uint8_t onewire_search_poll(void);

/**
 * @brief Number of devices found (valid once the search finished)
 * @return Count of found devices
 */
uint8_t onewire_search_count(void);

/**
 * @brief Check whether a search is currently running
 * @return 1 while the search owns the timer, 0 when it is idle
 */
uint8_t onewire_search_active(void);

/** @} */

/**
 * @defgroup ONEWIRE_CRC CRC-8
 * @{
 */

/**
 * @brief Calculate the Dallas/Maxim 1-Wire CRC-8 over a byte buffer
 * @param[in] data Input buffer
 * @param[in] len Number of bytes to process
 * @return CRC-8 checksum value
 */
uint8_t onewire_crc8(const uint8_t* data, uint8_t len);

/** @} */

#ifdef DS18B20_TEST_HARNESS
/**
 * @brief [TEST] Set the idle-HIGH gap injected between search slots
 * @param[in] us Gap duration in microseconds (0 disables the injection)
 * @note Temporary test hook for the RTOS-latency experiment only.
 */
void onewire_test_set_gap_us(uint16_t us);
#endif

#ifdef __cplusplus
}
#endif

#endif /* ONEWIRE_H */
