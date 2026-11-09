/**
 * @file ds2413.h
 * @brief Non-blocking DS2413 dual-channel addressable switch driver
 *
 * The DS2413 has two open-drain PIO pins (PIOA, PIOB). A write sets both
 * output latches; a read returns the actual pin levels (the latch when the pin
 * is not driven externally). The device is parasite powered: the driver primes
 * it with a 1 ms strong pull-up before each operation, then releases the bus
 * for the reset/command/reply sequence. A failed reply is retried up to three
 * times.
 *
 * The driver follows the same non-blocking start/poll discipline as the
 * DS18B20 and DS2431 drivers and shares the TIM1/DMA/GPIO bus with them: one
 * operation at a time, no concurrent polling of two drivers.
 *
 * Wire protocol (DS2413 datasheet, also used by the Linux w1_ds2413 driver):
 * - PIO Access Read [F5h]: one reply byte; the low nibble carries PIOA in bit
 *   1 and PIOB in bit 3, bits 4-7 are its complement. 0xFF means the device
 *   did not answer; a byte whose low nibble is not the complement of its high
 *   nibble is rejected. The driver normalizes the reply to the write layout
 *   (bit 0 = PIOA, bit 1 = PIOB).
 * - PIO Access Write [5Ah]: the master sends the state byte (bits 0/1 are
 *   PIOA/PIOB, bits 2-7 must be 1) followed by its complement, then reads one
 *   acknowledge byte. 0xAA means the write was accepted.
 *
 * Addressing: `ds2413_select()` chooses Match ROM (0x55 + 8-byte ROM) for one
 * device or Skip ROM (0xCC) for a broadcast. Broadcast write is useful to set
 * several switches at once. Broadcast read is allowed but only meaningful with
 * a single DS2413 on the bus: several devices answering at once produce a
 * wired-AND of their replies, which the driver cannot attribute.
 */

#ifndef DS2413_H
#define DS2413_H

#include "ow_config.h"
#include "ow_version.h"
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @defgroup DS2413_Exported_Constants DS2413 Exported Constants
 * @{
 */

/** @brief DS2413 1-Wire family code (LSB of the 64-bit ROM address) */
#define DS2413_FAMILY_CODE 0x3Au

/** @brief Number of bytes in a device ROM address */
#define DS2413_ROM_BYTES 8u

/** @brief Standard 8 bits per byte */
#define DS2413_BITS_PER_BYTE 8u

/** @brief PIOA channel bit in a state byte */
#define DS2413_PIOA 0x01u

/** @brief PIOB channel bit in a state byte */
#define DS2413_PIOB 0x02u

/** @brief Both PIO channels */
#define DS2413_PIO_MASK 0x03u

/** @brief PIO Access Read command */
#define DS2413_CMD_READ 0xF5u

/** @brief PIO Access Write command */
#define DS2413_CMD_WRITE 0x5Au

/** @brief Acknowledge byte returned by a successful PIO Access Write */
#define DS2413_ACK 0xAAu

/** @brief Reply byte when the device did not answer */
#define DS2413_NO_RESPONSE 0xFFu

/** @} */

/**
 * @defgroup DS2413_Exported_Functions DS2413 Exported Functions
 * @{
 */

/**
 * @brief Initialize the DS2413 driver hardware
 * @pre Call after MCU reset, after the system clock is configured and before
 *      any 1-Wire bus operation.
 * @note Calls onewire_init(), which takes exclusive ownership of the shared
 *       TIM1/DMA/GPIO resources. The DS18B20 and DS2431 drivers share that
 *       bus; all inits may run before any operation, but only one driver may
 *       own the timer at a time.
 * @warning A second call is a bootstrap operation, not a reinitialization hook.
 */
void ds2413_init(void);

/**
 * @brief Select the DS2413 to address
 * @param[in] rom Pointer to the 8-byte ROM address (LSB first), or NULL to
 *                use Skip ROM (broadcast)
 * @note Accepted only while the driver is idle and no search is running.
 *       With a non-NULL ROM the driver sends Match ROM before every command.
 *       A broadcast read is only meaningful with one DS2413 on the bus.
 */
void ds2413_select(const uint8_t* rom);

/**
 * @brief Read the PIO pin states (non-blocking)
 * @param[out] out Destination byte; on success it receives the PIOA/PIOB
 *                 levels in bits 0/1 (DS2413_PIOA, DS2413_PIOB), the same
 *                 layout ds2413_write_pio() takes
 * @return 1 if the read was scheduled, 0 if the request was rejected (NULL
 *         destination, an operation in flight, or a device search running)
 * @note The result is valid only when ds2413_read_pio_poll() returns 1 and
 *       ds2413_last_command_ok() is 1. `out` must stay valid until then.
 */
uint8_t ds2413_read_pio(uint8_t* out);

/**
 * @brief Advance the non-blocking PIO read
 * @return 1 when finished (successfully or aborted), 0 while running
 */
uint8_t ds2413_read_pio_poll(void);

/**
 * @brief Write the PIO output latches (non-blocking)
 * @param[in] states PIOA/PIOB latch levels in bits 0/1 (DS2413_PIOA,
 *                   DS2413_PIOB); other bits are ignored
 * @return 1 if the write was scheduled, 0 if the request was rejected (an
 *         operation in flight or a device search running)
 * @note Both latches are written together. To change one channel, read the
 *       current state first and pass the modified byte; the device has no
 *       read-modify-write command. A broadcast write addresses every DS2413
 *       on the bus.
 */
uint8_t ds2413_write_pio(uint8_t states);

/**
 * @brief Advance the non-blocking PIO write
 * @return 1 when finished (successfully or aborted), 0 while running
 */
uint8_t ds2413_write_pio_poll(void);

/**
 * @brief Result of the last completed operation
 * @return 1 when the last poll finished a read with a valid reply or a write
 *         with the 0xAA acknowledge, 0 when it aborted (no presence, no
 *         response, a malformed read byte or a rejected write) or nothing ran
 */
uint8_t ds2413_last_command_ok(void);

/**
 * @brief Last valid PIO state read by ds2413_read_pio()
 * @return Bits 0/1 with the PIOA/PIOB levels; 0 before the first valid read
 */
uint8_t ds2413_last_pio(void);

/**
 * @brief Raw reply byte of the last read or write operation
 * @return The byte the device drove (0xFF when it did not answer); useful to
 *         tell a missing device from a malformed PIO reply. 0 before the first
 *         operation.
 */
uint8_t ds2413_last_reply(void);

/** @} */

#ifdef __cplusplus
}
#endif

#endif /* DS2413_H */
