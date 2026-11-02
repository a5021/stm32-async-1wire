/**
 * @file ow_glue.h
 * @brief Library-internal bridge between the driver sources and the engine.
 *
 * A few operations have always belonged to the DS18B20/DS2431 drivers, not
 * to a consumer: scheduling a hardware-timed wait (a DS18B20 conversion
 * wait, an EEPROM hold-off window) and kicking the owner's state machine so
 * it advances; polling a scheduled operation to completion (with the
 * low-power sleep handled once, in one place); proving a device presence
 * from the last reset capture; and encoding command-write pulses (the ROM
 * addressing prefix, then raw payload bytes). They are engine calls, but
 * they are made by the driver, and the two are separate translation
 * units - so the contract between them needs a header.
 *
 * It is not a public header. The engine headers are not installed, and a
 * consumer has no business scheduling a bus wait with a value in timer
 * registers: the wait belongs to the driver that knows the device's timing.
 * That header used to expose the two oldest calls in inc/onewire.h, where
 * they read as part of the bus API; they were never meant to be called from
 * outside the library.
 *
 * Library-internal (src/): reached only by the driver sources and by
 * src/onewire.c, which defines the calls.
 */

#ifndef OW_GLUE_H
#define OW_GLUE_H

#include <stdint.h>

#include "onewire.h"
#include "ow_compiler.h"

/**
 * @brief Start a hardware-timed wait of a given duration
 * @param[in] us Requested wait in microseconds
 * @note The engine picks the timer periods; the caller states a duration.
 *       See ow_port_plan_wait() for the rule.
 */
void ow_start_wait(uint32_t us);

/**
 * @brief Raise the timer update flag so the caller's state machine advances
 * @note No hardware timing is involved: the operation this call unblocks is
 *       scheduled by the owner itself. Used by the DS18B20 driver to start one
 *       measurement cycle (ds18b20_start_measure()). Call it only while no
 *       operation is in flight.
 */
void ow_timer_kick(void);

/**
 * @brief Poll the scheduled hardware operation to completion
 * @return 1 when the operation finished, 0 while still running
 * @note Checks onewire_bus_done() once; on low-power builds a still-running
 *       long stage is slept through instead of spun on (WFE, woken by the
 *       timer's own update event via SEVONPEND - no ISR so this is invisible
 *       to the application: the poll just returns when the stage is done),
 *       and the wake event is consumed so the caller sees completion on the
 *       same poll. Non-low-power builds are a single bus_done() check.
 */
uint8_t ow_op_done(void);

/**
 * @brief Prove a device presence from the last reset capture
 * @param[in] cap Reset + presence capture (2 entries)
 * @return 1 when at least one device answered, 0 otherwise
 * @note Presence is proved by the captured edge count, not by the buffer
 *       contents: the transfer engine reports how many edges arrived, so a
 *       stale second slot can never fake a device. Single place for that rule.
 */
__STATIC_FORCEINLINE uint8_t ow_has_presence(const volatile uint16_t* cap) {
    return onewire_present(cap, onewire_reset_captured());
}

/**
 * @brief Encode raw bytes into pulse-table entries, advancing past them
 * @param[in,out] dst Cursor into the pulse table; advanced by n entries
 * @param[in] src Bytes to encode
 * @param[in] n Byte count
 * @return Bytes encoded (n)
 * @note Pure mechanical transform shared by every command build; what the
 *       bytes mean (prefix, command, payload) stays in the drivers.
 */
__STATIC_FORCEINLINE uint8_t ow_encode_bytes(ow_pulse_t** dst, const uint8_t* src, uint8_t n) {
    for (uint8_t i = 0; i < n; i++) {
        onewire_encode_byte(*dst, src[i]);
        *dst += ONEWIRE_BITS_PER_BYTE;
    }
    return n;
}

/**
 * @brief Encode a Skip ROM prefix, advancing the cursor past it
 * @param[in,out] dst Cursor into the pulse table; advanced by one byte
 * @return Bytes encoded (1)
 * @note Split from the Match ROM form on purpose: each call site knows its
 *       addressing mode statically, so there is no dead mode branch to
 *       carry (and no branch to leave uncovered). Skip ROM 0xCC is a 1-Wire
 *       protocol constant, identical for every slave.
 */
__STATIC_FORCEINLINE uint8_t ow_encode_skip_rom(ow_pulse_t** dst) {
    onewire_encode_byte(*dst, ONEWIRE_SKIP_ROM);
    *dst += ONEWIRE_BITS_PER_BYTE;
    return 1u;
}

/**
 * @brief Encode a Match ROM prefix and the 8-byte ROM, advancing the cursor
 * @param[in,out] dst Cursor into the pulse table; advanced past the prefix
 * @param[in] rom 8-byte ROM address (LSB first) of the addressed device
 * @return Bytes encoded (1 + ONEWIRE_ROM_BYTES)
 * @note Match ROM 0x55 is a 1-Wire protocol constant; the addressing policy
 *       (when to use Match rather than Skip) stays in the driver.
 */
__STATIC_FORCEINLINE uint8_t ow_encode_match_rom(ow_pulse_t** dst, const uint8_t* rom) {
    onewire_encode_byte(*dst, ONEWIRE_MATCH_ROM);
    *dst += ONEWIRE_BITS_PER_BYTE;
    return (uint8_t)(1u + ow_encode_bytes(dst, rom, ONEWIRE_ROM_BYTES));
}

#endif /* OW_GLUE_H */
