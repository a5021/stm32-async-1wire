/**
 * @file onewire_internal.h
 * @brief Private pulse-level interface of the 1-Wire layer
 * @details Not part of the public API. The public onewire.h exposes only the
 *          byte-oriented onewire_write_command()/onewire_write_command_byte();
 *          the raw per-slot pulse encoding stays behind this header so callers
 *          (the slave drivers and the port layer) never build pulse buffers.
 *
 *          The pulse type is family sized: the STM32F4 port feeds a 16-bit
 *          CCR3 in DMA direct mode, so its pulse entries are halfwords
 *          (zero-copy feed); every other supported port latches 8-bit entries.
 */

#ifndef ONEWIRE_INTERNAL_H
#define ONEWIRE_INTERNAL_H

#include <stdint.h>
#include "onewire.h"

#ifdef __cplusplus
extern "C" {
#endif

/** @brief One bit-slot pulse duration in the native width of the active port */
#if defined(OW_PORT_FAMILY_F4)
typedef uint16_t ow_pulse_t;
#else
typedef uint8_t ow_pulse_t;
#endif

/**
 * @brief Encode a byte into write-pulse durations (LSB first)
 * @param[out] out Output buffer (ONEWIRE_BITS_PER_BYTE entries)
 * @param[in] byte Byte value to encode
 */
void onewire_encode_byte(ow_pulse_t* out, uint8_t byte);

/**
 * @brief Schedule a write of `slots` bit slots
 * @param[in] pulses Pulse buffer (one entry per slot); for `slots > 1` the
 *                   entry at index `slots` must be 0 (hardware bus release)
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
uint8_t onewire_write_pulses(const ow_pulse_t* pulses, uint16_t slots);

#ifdef DS18B20_TEST_HARNESS
/**
 * @brief [TEST] Register the internal command buffer with the host DMA mock
 * @note Called by the test harness so hw_resolve() can translate the feed
 *       CMAR back into a host pointer.
 */
void onewire_test_register_cmd_buffer(void);

/**
 * @brief [TEST] Feed-source address of the internal command buffer
 * @return Pointer to &ow_cmd_buf[1] (the CCR3-feed DMA source; slot 0 is
 *         latched directly), for exact-CMAR contract assertions.
 */
const void* onewire_test_cmd_feed_addr(void);
#endif

#ifdef __cplusplus
}
#endif

#endif /* ONEWIRE_INTERNAL_H */