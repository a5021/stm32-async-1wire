/**
 * @file ds2431_internal.h
 * @brief Private DS2431 driver interface (library-internal).
 *
 * Owns the driver-private types and constants shared by the core TU
 * src/ds2431.c and the engine TU src/ds2431_txn.c: command sizing, phase
 * enum, context layout, flag bits and the engine entry points. Not
 * installed, never included by consumers or by the test accessors
 * directly; the core, the engine and the test amalgamation reach it
 * through their own includes.
 *
 * The context instance itself stays a file-scope static in src/ds2431.c so
 * the RAM budget and the host test model (which compiles src/ds2431.c
 * together with its accessors) do not change.
 */

#ifndef DS2431_INTERNAL_H
#define DS2431_INTERNAL_H

#include <stdint.h>

#include "ds2431.h"
#include "onewire.h"
#include "ow_glue.h"

/** @brief Longest DS2431 command write in bytes: Match ROM prefix (1 + 8) +
 *         function command + 2 target-address bytes + 8-byte payload. */
#define DS2431_CMD_MAX_BYTES ((1 + DS2431_ROM_BYTES) + 1 + 2 + DS2431_SCRATCH_SIZE)
/** @brief Bit slots for the longest command write (DS2431_CMD_MAX_BYTES x 8) */
#define DS2431_CMD_SLOTS (DS2431_CMD_MAX_BYTES * DS2431_BITS_PER_BYTE)
/** @brief Bit slots for the scratchpad frame read, CRC included */
#define DS2431_FRAME_SLOTS (DS2431_SCRATCH_FRAME_CRC * DS2431_BITS_PER_BYTE)

/**
 * @brief DS2431 non-blocking operation phases
 */
typedef enum {
    DS2431_PH_RESET = 0, /**< reset scheduled; check presence */
    DS2431_PH_CMD, /**< command (prefix + function + TA + payload) write scheduled */
    DS2431_PH_READ, /**< data read scheduled (ctx.len bytes into the scratchpad frame) */
    DS2431_PH_WAIT, /**< Copy Scratchpad t_PROG hold-off scheduled */
    DS2431_PH_DONE, /**< finished; hand the timer back to the caller */
    DS2431_PH_IDLE /**< parked between operations (post-init) */
} ds2431_phase_t;

/**
 * @brief DS2431 driver context structure using a union for memory efficiency
 * @warning The four members of `u` alias the same storage:
 *          - cap[]: reset + presence pulses, captured by the RESET phase;
 *          - cmd[]: the command write pulse table, fed to the CCR3 DMA;
 *          - pulse[]: the read capture, written by the read DMA.
 *          The command table is built ONLY at the RESET -> CMD transition
 *          (ds2431_build() inside the poll), after the reset capture has been
 *          consumed by onewire_present(): rebuilding over cap[] there is what
 *          makes aliasing safe. It stays live until the write completes, and
 *          the read capture that reuses the storage happens only in the READ
 *          phase of the next op, after the write finished. The output frame
 *          (raw[]) stays a separate member: it overlaps nothing (the decode
 *          source pulse[] must not be overwritten by its own output).
 */
typedef struct {
    uint8_t* out; /**< User result buffer (valid until the op finishes) */
    union {
        ow_pulse_t cmd[DS2431_CMD_SLOTS + 1]; /**< command write build (DMA feed) */
        volatile uint16_t cap[2]; /**< reset + presence pulse durations */
        volatile uint8_t pulse[DS2431_SCRATCH_FRAME_CRC * DS2431_BITS_PER_BYTE]; /**< read capture (<= 13 bytes x 8 bits) */
    } u;
    uint8_t raw[DS2431_SCRATCH_FRAME_CRC]; /**< Decoded read result (TA1, TA2, ES, data..8, CRC16) */
    uint8_t phase; /**< Current phase (ds2431_phase_t values, packed) */
    uint8_t flags; /**< DS2431_FLAG_* bits */
    uint8_t command; /**< DS2431 function command byte (0xF0/0x0F/0xAA/0x55) */
    uint8_t addr_lo; /**< Target address low byte (TA1) */
    uint8_t addr_hi; /**< Target address high byte (TA2) */
    uint8_t len; /**< Bytes to read back (0 = no read phase) */
    uint8_t plen; /**< Payload bytes written after the command (0..8) */
    uint8_t es; /**< Expected end-offset status byte of the last Write Scratchpad */
    uint8_t payload[DS2431_SCRATCH_SIZE]; /**< Write Scratchpad / Copy Scratchpad payload */
    uint8_t selected_rom[DS2431_ROM_BYTES]; /**< ROM of the selected device */
} DS2431_ctx_t;

/** @brief Non-zero = Match ROM addressing (0 = Skip ROM broadcast) */
#define DS2431_FLAG_ADDRESS_MODE (1u << 0)
/** @brief 1 = parasite-powered bus: engage the strong pull-up during the t_PROG window */
#define DS2431_FLAG_PARASITE (1u << 1)
/** @brief Set once the operation completed with a device present / valid read */
#define DS2431_FLAG_OK (1u << 2)
/** @brief Set once the operation finished (or aborted) */
#define DS2431_FLAG_FINISHED (1u << 3)
/** @brief Set while a Write Scratchpad has latched TA + ES for copy/echo checks */
#define DS2431_FLAG_PENDING (1u << 4)
/** @brief Set on the scratchpad frame read: verify the TA/ES echo when pending */
#define DS2431_FLAG_ECHO (1u << 5)

/* Copy Scratchpad completion pattern: after the t_PROG window the device
 * transmits an alternating 0/1 sequence until the master resets it, which a
 * single read byte samples as 0xAA. Any other value (the device drives 1s when
 * it refused the copy: PF set, invalid address or copy-protected page) means
 * the EEPROM block was not programmed. */
#define DS2431_COPY_STATUS_OK 0xAAu

/* Engine entry points implemented by src/ds2431_txn.c on the caller-owned
 * context. The core owns the single static instance and passes it in, so the
 * engine never names a global; the prototypes only document the core/engine
 * boundary. */
uint8_t ds2431_can_start(const DS2431_ctx_t* c);
uint8_t ds2431_build(DS2431_ctx_t* c, uint8_t command);
uint8_t ds2431_start(DS2431_ctx_t* c, uint8_t command, uint8_t* out);
uint8_t ds2431_poll(DS2431_ctx_t* c);

/* Scratchpad reply helpers shared by the engine and the read-scratchpad poll
 * in the core (both need the same length/CRC rule). */
uint8_t ds2431_scratchpad_data_len(const uint8_t* frame);
uint8_t ds2431_scratchpad_crc_ok(const uint8_t* frame, uint8_t n);

#endif /* DS2431_INTERNAL_H */
