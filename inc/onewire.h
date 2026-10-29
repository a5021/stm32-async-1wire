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
#include "ow_version.h"
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
/** @brief 1-Wire ROM command: Search ROM (all devices respond with their ROM) */
#define ONEWIRE_SEARCH_ROM 0xF0
/** @brief 1-Wire ROM command: Alarm Search (only alarming devices respond) */
#define ONEWIRE_ALARM_SEARCH 0xEC
/** @brief Board facts: the family selection token and ow_pulse_t.
 *
 *  The token resolves from the CMSIS device macro of the target (e.g.
 *  -DSTM32F030x6) or, for host-test mocks, PlatformIO and STM32CubeMX
 *  projects, from a family macro (STM32F0, STM32F1, STM32F3, STM32F4,
 *  STM32G0, STM32G4, STM32H5).  Concrete device spellings are accepted for
 *  the parts this project supports (STM32F030x6 for F0; STM32F103xB for F1;
 *  STM32F303xC for F3; STM32F407xx / STM32F401xC / STM32F401xE / STM32F446xx
 *  for F4; STM32G031xx for G0; STM32G474xx for G4; STM32H503xx for H5).
 *  The token is the single point of family selection: app.c takes the device
 *  header and clock configuration from it, and the per-family facts
 *  ow_pulse_t and the OW_HSE_MHZ / OW_*_MAX_SYSCLK_MHZ defaults live in
 *  inc/ow_board.h beside it.  The TIM1+DMA engine and the register-level
 *  per-family facts are library-internal and are not needed to use this
 *  API.  To add a family, extend the dispatch in
 *  inc/ow_board.h (plus the \#error spelling list), the app.c clock/UART
 *  config, a chips/<part>.mk part file, the Makefile (OW_KNOWN_TARGETS, the
 *  OW_TARGET family block, the OW_MCU mapping entries, TEST_CLOCK_FLAG) and
 *  CMakeLists.txt (the OW_MCU mapping, the OW_TARGET branch, the default
 *  chip) wiring, the tests/mock device header, and a case in
 *  tests/test/test_sysclk_fallback.c. */
#include "ow_board.h"

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

/**
 * @brief Configuration token the library was compiled with
 * @return OW_CONFIG_TOKEN as seen by the library translation units
 * @note Compare with the application-side OW_CONFIG_TOKEN at startup:
 *       a mismatch means the archive and the application were built with
 *       different OW flags or DS18B20_MAX_DEVICES. Fail loudly there instead
 *       of running with a silently different bus timing or table size.
 */
uint32_t onewire_config_token(void);

/**
 * @brief Whether a runtime clock reading agrees with the build
 * @param[in] sysclk_mhz System clock in whole MHz as the firmware actually
 *                       runs, e.g. (SystemCoreClock / 1000000u)
 * @return 1 when the reading matches the clock the port timing was built for
 * @note Call once at startup before any bus operation and refuse to run on 0:
 *       with -DOW_SYSCLK_MHZ the port prescaler is fixed at compile time, so
 *       a firmware clocked differently scales every 1-Wire slot and the bus
 *       just looks dead. Without -DOW_SYSCLK_MHZ (SystemCoreClock auto-path)
 *       the port derives its prescaler from SystemCoreClock itself, so this
 *       only rejects a zero/uninitialized reading - staleness after a
 *       clock-tree change is undetectable here, call SystemCoreClockUpdate().
 */
uint8_t onewire_clock_ok(uint32_t sysclk_mhz);

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
 *                          durations (2 x 16-bit; entry 1 is only valid when
 *                          onewire_reset_captured() returns 2)
 * @note On completion, call onewire_reset_captured() and pass its count,
 *       together with this buffer, to onewire_present().
 */
void onewire_reset(volatile uint16_t* reset_pulses);

/**
 * @brief Edges captured by the last capture operation armed
 * @return 0..2; after a reset, 2 means the presence edge arrived, 1 means
 *         only the master-release edge was captured, 0 means neither
 * @note Read from the capture DMA's remaining-transfer count, so the value
 *       describes whatever capture was armed last - a reset, a read pair, a
 *       merged write+read or a data read - not the reset specifically. Every
 *       completed capture drains that counter, so after any of them this call
 *       returns 2 again; before the first reset it also returns 2, because the
 *       register is still at its reset value. Treat 2 as evidence only for a
 *       reset that onewire_reset() actually armed: read the count after that
 *       reset's onewire_bus_done() and before any other capture is armed, and
 *       pass it to onewire_present() with the buffer of the same reset.
 */
uint8_t onewire_reset_captured(void);

/**
 * @brief Decode the presence pulse captured by onewire_reset()
 * @param[in] pulses Reset + presence pulse durations captured by onewire_reset()
 * @param[in] captured Edges captured by that reset; pass
 *                     onewire_reset_captured()
 * @return 1 if at least one device answered, 0 otherwise
 * @note A reset that captured only the master-release edge has no presence
 *       entry; `captured` is what distinguishes it from a stale pulse[1],
 *       whose value may otherwise fall inside the presence window.
 *       `captured` must describe the same reset as `pulses`: the two are one
 *       hardware fact read from two places. Both go stale the moment another
 *       capture is armed - a later completed capture makes
 *       onewire_reset_captured() return 2 again, which would otherwise bless
 *       a stale pulses[1].
 */
uint8_t onewire_present(const volatile uint16_t* pulses, uint8_t captured);

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
 *         call is rejected and no operation is scheduled). In firmware the
 *         reject path returns 0; an assert is compiled in only under
 *         DS18B20_TEST_HARNESS or OW_TEST_PARAM_GUARD (and is disabled by
 *         NDEBUG).
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
 *         is rejected and no operation is scheduled). In firmware the reject
 *         path returns 0; an assert is compiled in only under
 *         DS18B20_TEST_HARNESS or OW_TEST_PARAM_GUARD (and is disabled by
 *         NDEBUG).
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

#if OW_LOW_POWER
/**
 * @brief Whether the scheduled hardware stage is a long one (WFE eligible)
 * @return 1 while a long stage (> 1 ms: temperature conversion, scratchpad
 *         read, EEPROM hold-off) is in flight, 0 otherwise
 * @note Derived from the scheduled timer duration, so callers stay on the
 *       onewire API. Only meaningful with
 *       OW_LOW_POWER=1.
 * @note Bus-layer vocabulary, for a slave driver that would rather idle the
 *       core than spin on a long stage: the bundled drivers call it from
 *       their poll(). An application never needs it - the driver APIs
 *       (ds18b20.h, ds2431.h) are start/poll pairs.
 */
uint8_t onewire_long_wait_pending(void);

/**
 * @brief Block in WFE until the currently scheduled long stage completes
 * @note Only the timer's own update event wakes the core (SEVONPEND, no ISR,
 *       no NVIC vector), so to the caller this is an ordinary synchronous
 *       wait. Only meaningful with OW_LOW_POWER=1.
 * @pre A long stage must be in flight - the caller checks
 *       onewire_long_wait_pending() first, which is exactly what the bundled
 *       drivers do. Called with nothing armed, the call does not return: the
 *       sleep waits for the timer's update event, and none is coming.
 * @note The blocking counterpart of onewire_long_wait_pending(), for a slave
 *       driver that wants to sleep rather than poll. The bundled drivers
 *       call it from their poll(); an application never needs it.
 */
void onewire_sleep_until_done(void);
#endif

/** @} */

/**
 * @defgroup ONEWIRE_Search Generic Non-Blocking Search ROM Engine
 * @brief Implements the Maxim Search ROM (0xF0) / Alarm Search (0xEC)
 *        algorithm as a polled state machine. The direction bit written at
 *        position i determines which devices keep participating at position
 *        i+1, so the transaction cannot be batched into one DMA pass; the
 *        linear loop is decomposed into states with loop counters kept in the
 *        context. Every state performs exactly one hardware-timed operation,
 *        and no search stage is long enough to become a low-power wait, so a
 *        poll call returns straight away.
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
 * @defgroup ONEWIRE_CRC CRC Checksums
 * @{
 */

/**
 * @brief Calculate the Dallas/Maxim 1-Wire CRC-8 over a byte buffer
 * @param[in] data Input buffer
 * @param[in] len Number of bytes to process
 * @return CRC-8 checksum value
 */
uint8_t onewire_crc8(const uint8_t* data, uint8_t len);

/**
 * @brief Calculate the Maxim 1-Wire CRC-16 over a byte buffer
 * @param[in] data Input buffer
 * @param[in] len Number of bytes to process
 * @return 16-bit CRC remainder (CRC-16/ARC parameters: polynomial 0x8005,
 *         reflected 0xA001, initial value 0x0000, no final XOR)
 * @note This is the raw remainder (the value that, appended low-byte-first,
 *       drives a re-check to 0). Devices such as the DS2431 transmit the
 *       inverted form (~value); the caller complements when comparing against
 *       the bytes on the bus.
 */
uint16_t onewire_crc16(const uint8_t* data, uint8_t len);

/** @} */

#ifdef __cplusplus
}
#endif

#endif /* ONEWIRE_H */
