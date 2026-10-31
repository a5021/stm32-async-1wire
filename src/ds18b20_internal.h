/**
 * @file ds18b20_internal.h
 * @brief Private DS18B20 driver contract (library-internal).
 *
 * Owns the driver-private types, flag bits, sizing constants and the one
 * stateless leaf helper shared across the umbrella translation unit
 * src/ds18b20.c and its four include-only parts (resolution, txn, search,
 * measure): measurement/transaction/resolution contexts, phase enums and
 * the pulse-geometry constants. Not installed, never included by consumers;
 * the core includes it before the parts, and the host test amalgamation
 * reaches it through src/ds18b20.c.
 *
 * Ownership (instances stay where they are; this header only declares types):
 *   - src/ds18b20.c: `ctx`, `txn_ctx`, `phase_pulses`, `detect_buf`,
 *     `build_addr_cmd()`, `wait_conversion()` / `resolution_wait_us()`;
 *   - src/ds18b20_resolution.c: `res_ctx`;
 *   - src/ds18b20_search.c: `dev_roms`, `dev_count`, `search_user_sink`;
 *   - src/ds18b20_measure.c: `build_skip_cmd()` (the CONVERT/REQUEST
 *     encoders share `phase_pulses.cmd`, addressed writes `phase_pulses.addr`).
 *
 * The four parts are peers sharing this state, and the phase_pulses union
 * aliasing plus the cross-phase ownership guards are only provable inside
 * one translation unit — that is why the driver stays amalgamated while
 * DS2431 (strict core/engine hierarchy) is split. The include order in
 * src/ds18b20.c is load-bearing: every part references declarations from
 * the parts included above it (txn uses res_config_byte from resolution;
 * search reads txn_ctx/res_ctx finished flags; measure reads dev_roms).
 */

#ifndef DS18B20_INTERNAL_H
#define DS18B20_INTERNAL_H

#include <stdint.h>

#include "ds18b20.h"
#include "ow_compiler.h"

/** @brief Total length of DS18B20 scratchpad in bytes */
#define DS18B20_SCRATCHPAD_LEN 9
/** @brief Number of bytes to include in the scratchpad CRC calculation */
#define DS18B20_CRC8_BYTES 8
/** @brief Total number of bits in DS18B20 scratchpad */
#define DS18B20_SCRATCHPAD_BITS (DS18B20_SCRATCHPAD_LEN * DS18B20_BITS_PER_BYTE)
/** @brief Total slots for Match ROM + 8-byte ROM + command */
#define DS18B20_MATCH_SLOTS ((DS18B20_ROM_BYTES + 2) * DS18B20_BITS_PER_BYTE)
/** @brief Slots for the invariant Match ROM + 8-byte ROM prefix (built on select) */
#define DS18B20_PREFIX_SLOTS ((DS18B20_ROM_BYTES + 1) * DS18B20_BITS_PER_BYTE)
/** @brief Number of DMA transfers for command transmission (2 bytes x 8 bits) */
#define DS18B20_DMA_TRANSFERS (2 * DS18B20_BITS_PER_BYTE)
#define SCAN_DEVICE_GAP_US 1000 /**< 1ms scheduling bridge between scan-mode device reads (no bus requirement) */
/** @brief TH byte written together with the config register by the resolution
 *         state machine (Write Scratchpad requires TH + TL + CFG in one go).
 *         0 disables the alarm trigger threshold. */
#define DS18B20_RES_TH 0x00
/** @brief TL byte written together with the config register by the resolution
 *         state machine. 0 disables the alarm trigger threshold. */
#define DS18B20_RES_TL 0x00
/** @brief Bytes in the resolution config write for Skip ROM mode
 *         (Skip ROM 0xCC + Write Scratchpad 0x4E + TH + TL + CFG) */
#define DS18B20_RES_BYTES_MIN (1 + 1 + 3)
/** @brief Bytes in the resolution config write for Match ROM mode
 *         (Match ROM 0x55 + 8-byte ROM + 0x4E + TH + TL + CFG) */
#define DS18B20_RES_BYTES_MAX (1 + DS18B20_ROM_BYTES + 1 + 3)
/** @brief Slots for the Skip ROM resolution config write */
#define DS18B20_RES_SLOTS_MIN (DS18B20_RES_BYTES_MIN * DS18B20_BITS_PER_BYTE)
/** @brief Slots for the Match ROM resolution config write */
#define DS18B20_RES_SLOTS_MAX (DS18B20_RES_BYTES_MAX * DS18B20_BITS_PER_BYTE)
/** @brief Wait for Copy Scratchpad (t_COPY) / Recall EEPROM (t_RECALL)
 *         completion in microseconds (DS18B20 datasheet: 10ms max). */
#define DS18B20_EEPROM_WAIT_US 10000

/**
 * @brief DS18B20 driver context structure using union for memory efficiency
 * @note Different stages of communication use the same memory for different purposes
 */
typedef struct {
    /**
     * @brief Union overlay for memory efficiency
     * @warning CRITICAL INVARIANT: scratchpad[n] aliases pulse[n] (same byte).
     *          decode_scratchpad() MUST read all 8 bits of pulse[byte*8..byte*8+7]
     *          BEFORE writing scratchpad[byte]. Reordering loops will corrupt bytes 0-8.
     */
    union {
        volatile uint16_t capture[DS18B20_SCRATCHPAD_BITS / 2]; /**< Captured pulse durations (reset/presence) */
        volatile uint8_t pulse[DS18B20_SCRATCHPAD_BITS]; /**< Pulse durations for data decoding */
        uint8_t scratchpad[DS18B20_SCRATCHPAD_LEN]; /**< Sensor scratchpad data */
    };
    uint8_t current_state; /**< Current state machine state (ds18b20_state_t values, packed from enum to save RAM) */
    uint8_t flags; /**< DS18B20_FLAG_* bits (address_mode/scan_mode/parasite) */
    uint8_t scan_index; /**< Index of the device currently read in scan mode */
    uint8_t selected_rom[DS18B20_ROM_BYTES]; /**< ROM of the selected device */
    uint8_t resolution; /**< Conversion resolution in bits (9..12); drives the conversion wait */
} DS18B20_ctx_t;

/** @brief Non-zero = Match ROM addressing (0 = Skip ROM broadcast) */
#define DS18B20_FLAG_ADDRESS_MODE (1u << 0)
/** @brief 1 = simultaneous multi-device conversion (scan) mode */
#define DS18B20_FLAG_SCAN_MODE (1u << 1)
/** @brief 1 = parasite-powered bus: engage the strong pull-up during conversion and EEPROM programming windows (see ds18b20_set_parasite) */
#define DS18B20_FLAG_PARASITE (1u << 2)

/**
 * @brief Non-blocking single-command transaction phases
 */
typedef enum {
    DS18B20_TXN_RESET, /**< reset scheduled; check presence */
    DS18B20_TXN_WRITE, /**< command (prefix + function + payload) write scheduled */
    DS18B20_TXN_READ, /**< data read scheduled (read_bytes != 0) */
    DS18B20_TXN_WAIT, /**< timed wait scheduled (wait_us != 0) */
    DS18B20_TXN_DONE /**< finished; hand the timer back to the measurement */
} ds18b20_txn_phase_t;

/**
 * @brief Non-blocking single-command transaction context
 * @note Drives every infrequent DS18B20 command (Read ROM, Write Scratchpad
 *       thresholds, Copy/Recall EEPROM, Read Power Supply, raw Read
 *       Scratchpad) with the same reset -> write -> (read | wait) discipline
 *       as the resolution state machine. The command build lives in the
 *       shared phase_pulses.txn workspace (see the union in src/ds18b20.c),
 *       which must stay valid across poll calls because the DMA feeds CCR3
 *       from it asynchronously.
 * @note Field order is load-bearing for RAM: out/wait_us first, then bytes,
 *       so no padding slips between the pointer and the 16-bit wait. The
 *       one-bit states share flags (explicit masks, not C bitfields — layout
 *       must stay deterministic across toolchains). There is no ISR in this
 *       architecture (everything is poll-driven), so read-modify-write on the
 *       flag bytes is safe.
 */
typedef struct {
    uint8_t* out; /**< User result buffer (valid until the command finishes) */
    uint16_t wait_us; /**< Timed wait after the command (0 = none) */
    uint8_t phase; /**< Current phase of the transaction (ds18b20_txn_phase_t values, packed) */
    uint8_t command; /**< DS18B20 function command byte (0x33/0x4E/0x48/0xB8/0xB4/0xBE) */
    uint8_t payload[3]; /**< Write Scratchpad payload (TH, TL, CFG) */
    uint8_t payload_len; /**< 0..3 (payload bytes written after the command) */
    uint8_t read_bytes; /**< Bytes to read back (0 = no read phase) */
    uint8_t slots; /**< Bit slots in the built pulses (incl. prefix and payload) */
    uint8_t raw[DS18B20_SCRATCHPAD_LEN]; /**< Decoded read result */
    uint8_t flags; /**< DS18B20_TXN_FLAG_* bits (bare/ok/finished) */
} ds18b20_txn_ctx_t;

/** @brief Transaction without addressing prefix (Read ROM: single-device bus only) */
#define DS18B20_TXN_FLAG_BARE (1u << 0)
/** @brief Set once the transaction completed with a device present / valid read */
#define DS18B20_TXN_FLAG_OK (1u << 1)
/** @brief Set once the transaction finished (or aborted) */
#define DS18B20_TXN_FLAG_FINISHED (1u << 2)

/** @brief Resolution state machine phases */
typedef enum {
    DS18B20_RES_RESET, /**< reset scheduled; check presence */
    DS18B20_RES_WRITE, /**< config write scheduled (skip/match + 0x4E + TH + TL + CFG) */
    DS18B20_RES_DONE /**< operation finished; hand the timer back to the measurement */
} res_phase_t;

/**
 * @brief Non-blocking resolution change context
 * @note The config write is built into the shared phase_pulses.res workspace
 *       (see the union in src/ds18b20.c), which must stay valid across poll
 *       calls because the DMA feeds CCR3 from it asynchronously while the
 *       config write is sent.
 */
typedef struct {
    uint8_t phase; /**< Current phase of the resolution state machine (res_phase_t values, packed) */
    uint8_t pending_res; /**< Resolution (bits) to apply */
    uint8_t slots; /**< Bit slots in the built config write (incl. prefix and payload) */
    uint8_t flags; /**< DS18B20_RES_FLAG_* bits (applied/finished) */
} res_ctx_t;

/** @brief Set once the config write completed (resolution actually changed) */
#define DS18B20_RES_FLAG_APPLIED (1u << 0)
/** @brief Set once the operation has completed (or aborted) */
#define DS18B20_RES_FLAG_FINISHED (1u << 1)

/**
 * @brief Build the DS18B20 configuration register byte for a resolution
 * @param[in] res Resolution in bits (9..12)
 * @return Configuration register byte (R1/R0 bits set, rest at reset value)
 * @note 9 bit -> 0x1F, 10 bit -> 0x3F, 11 bit -> 0x5F, 12 bit -> 0x7F.
 *       Stateless leaf shared by the resolution part (config builds) and the
 *       txn part (threshold writes carry the current CFG).
 */
__STATIC_FORCEINLINE uint8_t res_config_byte(uint8_t res) {
    return (uint8_t)(0x1Fu | ((uint8_t)(res - DS18B20_RES_MIN) << 5));
}

#endif /* DS18B20_INTERNAL_H */
