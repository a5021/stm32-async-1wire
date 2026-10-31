/**
 * @file ds2413.h
 * @brief DS2413 dual-channel addressable switch - placeholder
 *
 * Reserved for a future non-blocking driver. Only the bus identity is defined
 * here so that examples can already recognise the device during a search and
 * report it, without pretending a driver exists (there is no implementation
 * in src/ds2413.c yet, and no ds2413_init()/poll() API).
 *
 * DS2413 facts the future driver will need:
 * - family code 0x3A, 64-bit ROM, parasite powered;
 * - PIO Access Read  (0xF5) returns one status byte (PIOB/PIOA complemented);
 * - PIO Access Write (0x5A) takes one byte plus its inverted complement;
 * - PIO Output Latch Read (0xF5) / Write (0x5A) use the same opcodes at the
 *   latch level; both reply with the Acknowledge byte pattern before the data
 *   byte is clocked in.
 * See the DS2413 datasheet for the exact slot-level handshake.
 */

#ifndef DS2413_H
#define DS2413_H

#include <stdint.h>

/** @brief DS2413 1-Wire family code (LSB of the 64-bit ROM address) */
#define DS2413_FAMILY_CODE 0x3Au

/** @brief Number of bytes in a device ROM address */
#define DS2413_ROM_BYTES 8u

#endif /* DS2413_H */
