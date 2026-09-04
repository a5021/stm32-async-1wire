/**
 * @file onewire.h
 * @brief Universal non-blocking 1-Wire bus layer for STM32
 * @details The driver (ds18b20.c) and any other 1-Wire slave driver (DS2413,
 *          DS2431, ...) are built on top of this layer. Every operation is
 *          scheduled on TIM1/DMA and completes asynchronously: callers poll
 *          onewire_bus_done() / onewire_search_poll() to advance, never wait.
 */

#ifndef ONEWIRE_H
#define ONEWIRE_H

#include "ow_config.h"
#include <stdint.h>

/* --- Compiler helpers the driver expects (portable stand-ins; real CMSIS
 *     headers define them too, so the #ifndef guards keep both paths
 *     identical). Kept here (not only in ow_port.h) so upper layers
 *     (ds18b20.c, app code) get them via this header without including
 *     the port layer directly. --- */
#ifndef __STATIC_FORCEINLINE
#define __STATIC_FORCEINLINE static __attribute__((always_inline)) inline
#endif
#ifndef __WEAK
#define __WEAK __attribute__((weak))
#endif

/**
 * @defgroup ONEWIRE_Protocol 1-Wire Protocol Constants
 * @{
 */

/** @brief Bytes in a device ROM address */
#define ONEWIRE_ROM_BYTES 8
/** @brief Bits in a device ROM address */
#define ONEWIRE_ROM_BITS (ONEWIRE_ROM_BYTES * 8)
/** @brief Clock, pulse and bits-per-byte definitions live in ow_config.h. */
/** @brief Opt-in low-power WFE sleep: when defined, every hardware operation
 *  enables the TIM1 update interrupt (UIE) and SEVONPEND so that a pending
 *  update event wakes the core from WFE without an ISR. Long stages
 *  (> 1 ms: conversion, scratchpad read, EEPROM hold-off, inter-cycle pause)
 *  can then sleep with onewire_sleep_until_done(). Disabled by default so
 *  non-low-power builds pay zero cost. Enable with -DOW_PORT_LOW_POWER.
 *  @note No ISR is ever installed and NVIC_EnableIRQ is never called; the
 *        pending bit is cleared explicitly in onewire_bus_done() so WFE does
 *        not degrade into a busy-loop. */

typedef enum {
    ONEWIRE_TIMING_FAST = 0,
    ONEWIRE_TIMING_STANDARD,
    ONEWIRE_TIMING_SLOW,
    ONEWIRE_TIMING_ROBUST,
    ONEWIRE_TIMING_CUSTOM,
    ONEWIRE_TIMING_COUNT
} onewire_timing_profile_t;

typedef struct {
    uint8_t one_pulse;
    uint8_t zero_pulse;
    uint8_t guard_band;
    uint8_t parasite_guard_band;
    uint8_t short_pulse_max;
} onewire_timing_t;

#ifndef OW_TIMING_DEFAULT
#define OW_TIMING_DEFAULT ONEWIRE_TIMING_STANDARD
#endif
#ifndef ONEWIRE_TIMING_PROFILE_DEFAULT
#define ONEWIRE_TIMING_PROFILE_DEFAULT OW_TIMING_DEFAULT
#endif

/* The runtime timing externs (ow_one_pulse_us, ...) live in ow_config.h. */
void ow_set_parasite_guard(uint8_t parasite);
void onewire_set_timing_profile(onewire_timing_profile_t profile);
onewire_timing_profile_t onewire_get_timing_profile(void);

/** @} */

/**
 * @defgroup ONEWIRE_Init Init
 * @{
 */

/**
 * @brief Initialize the shared 1-Wire timer/DMA/GPIO resources
 * @note Enables GPIOA/TIM1/DMA1 clocks, sets the timer prescaler for 1µs
 *       resolution, configures PA10 as alternate-function open-drain and marks
 *       the search engine idle. Called once at startup, e.g. by the slave
 *       driver's own init.
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
 * @brief Hand the timer back to the measurement loop (ownership handover)
 * @note Sets UIF without clearing it, so the next onewire_bus_done() poll
 *       advances immediately. Called exactly once when a sub-machine (search,
 *       resolution change, command transaction) finishes and returns timer
 *       ownership to ds18b20_poll(). Thin wrapper over ow_port_bus_handover().
 */
void onewire_bus_handover(void);

#ifdef OW_PORT_LOW_POWER
/**
 * @brief Whether the current operation is a long stage (> 1 ms)
 * @return 1 while a long stage (conversion, scratchpad read, EEPROM hold-off,
 *         inter-cycle pause) is in flight, 0 otherwise
 * @note Layer wrapper over ow_port_long_wait_pending() so application code
 *       (e.g. demo6) does not depend on the port layer directly.
 */
uint8_t onewire_long_wait_pending(void);

/**
 * @brief Block in WFE until the scheduled long stage completes
 * @note Layer wrapper over ow_port_sleep_until_done(). Valid only while a
 *       long stage is running; the pending bit is cleared in onewire_bus_done().
 */
void onewire_sleep_until_done(void);
#endif

/**
 * @brief Schedule a 1-Wire bus reset (presence pulse captured via DMA)
 * @param[out] edge_out Buffer for the captured edge timestamps (2 × 16-bit)
 * @note On completion, decode the presence pulse with onewire_present().
 */
void onewire_reset(volatile uint16_t* edge_out);

/**
 * @brief Decode the presence pulse captured by onewire_reset()
 * @param[in] edge Edge timestamps captured by onewire_reset()
 * @return 1 if at least one device answered, 0 otherwise
 */
uint8_t onewire_present(const volatile uint16_t* edge);

/**
 * @brief Schedule a write of `slots` bit slots
 * @param[in] pulses Pulse buffer (one entry per slot); for `slots > 1` the
 *                   entry at index `slots` must be 0 (hardware bus release)
 * @param[in] slots Number of bit slots to transmit
 * @note Non-blocking: the DMA feeds CCR1 from the buffer asynchronously, so
 *       the buffer must stay valid until onewire_bus_done() reports completion.
 */
void onewire_write_slots(const uint8_t* pulses, uint16_t slots);

/**
 * @brief Schedule a single-slot write of one raw bit
 * @param[in] bit Bit value to write (0 or 1)
 */
void onewire_write_bit(uint8_t bit);

/**
 * @brief Schedule a two-slot read of a Search ROM id/cmp bit pair
 * @param[out] edge_out Buffer for the captured edge timestamps (2 × 16-bit)
 * @note On completion, decode the pair with onewire_pair_bits().
 */
void onewire_read_pair(volatile uint16_t* edge_out);

/**
 * @brief Decode the id/cmp bits of a two-slot read pair
 * @param[in] edge Edge timestamps captured by onewire_read_pair()
 * @param[out] id_bit Id bit (0 or 1)
 * @param[out] cmp_bit Complement bit (0 or 1)
 */
void onewire_pair_bits(const volatile uint16_t* edge, uint8_t* id_bit, uint8_t* cmp_bit);

/**
 * @brief Schedule a merged single-slot write followed by a two-slot read pair
 * @param[in] bit Direction bit to write in slot 1 (0 or 1)
 * @note One timer pass runs three slots: a write of `bit`, then a read of the
 *       next id/cmp pair. Halves the timer passes per search bit compared to a
 *       plain write plus a separate read pair. On completion, decode the pair
 *       from the internal merged-edge buffer with onewire_pair_bits()
 *       (entry 0 holds the write-slot edge; id/cmp are entries 1 and 2).
 */
void onewire_write_then_read(uint8_t bit);

/**
 * @brief Schedule a read of `bytes` bytes from the bus
 * @param[out] dst Buffer for the captured pulse durations (bytes × 8 × 8-bit)
 * @param[in] bytes Number of bytes to read
 * @note On completion, the caller decodes the 8-bit pulse durations (pulse
 *       `<= ONEWIRE_SHORT_PULSE_MAX` reads as bit 1) into data bytes.
 */
void onewire_read_data(volatile uint8_t* dst, uint8_t bytes);

/**
 * @brief Decode a single captured pulse duration into a 1-Wire bit
 * @param[in] dur Pulse duration in microseconds
 * @return 1 if the pulse is short (bit value '1'), 0 if long (bit value '0')
 * @note A pulse duration `<= ONEWIRE_SHORT_PULSE_MAX` is the short ('1') slot.
 */
static inline uint8_t onewire_bit_from_pulse(uint16_t dur) {
    return (dur <= ow_short_pulse_max_us) ? 1u : 0u;
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
void onewire_encode_byte(uint8_t* out, uint8_t byte);

/**
 * @brief Start a hardware-timed wait with the shared one-pulse timer
 * @param[in] arr Auto-reload value (one timer period in µs)
 * @param[in] rcr Repetition counter (number of periods - 1)
 * @note The timer update event fires after (RCR + 1) × ARR microseconds; used
 *       for DS18B20 conversion waits and inter-measurement pauses.
 */
void onewire_start_timer(uint16_t arr, uint8_t rcr);

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
 * @note Ignores the call while a search is already running or while another
 *       operation owns the timer.
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

#endif /* ONEWIRE_H */
