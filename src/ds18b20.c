#include "ds18b20.h"
#include "onewire.h"
/* Intra-stack instrumentation, not a layer leak: stats emission needs
 * driver-owned concepts (device ROM, DS18B20_TEMP_ERROR_*) that the onewire
 * layer must not know, so the driver is the rightful emitter. Compiles away
 * when OW_STATS_ENABLE=0. */
#include "ow_stats.h"

/**
 * @defgroup DS18B20_Private_Types DS18B20 Private Types
 * @{
 */

/**
 * @defgroup DS18B20_Private_Constants DS18B20 Private Constants
 * @{
 */

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
/** @brief Number of DMA transfers for command transmission (2 bytes × 8 bits) */
#define DS18B20_DMA_TRANSFERS (2 * DS18B20_BITS_PER_BYTE)
#define SCAN_DEVICE_GAP_US 1000 /**< 1ms scheduling bridge between scan-mode device reads (no bus requirement) */
#define SCAN_DEVICE_GAP_RCR 0
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
 * @}
 */

#define DS18B20_DRIVER_BUILD 1

/**
 * @defgroup DS18B20_Private_Types DS18B20 Private Types
 * @{
 */

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
 *       shared phase_pulses.txn workspace (see the union above), which must
 *       stay valid across poll calls because the DMA feeds CCR3 from it
 *       asynchronously.
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

/**
 * @}
 */

/**
 * @defgroup DS18B20_Private_Variables DS18B20 Private Variables
 * @{
 */

/** @brief Global driver context instance */
static DS18B20_ctx_t ctx;

/* Packing guard: the context layout is hand-packed (u8 phases, flag bytes,
 * no padding). If a field is added, update this number knowingly — every
 * byte here is .bss on all families. */
_Static_assert(sizeof(ctx) == 84, "driver context must stay 84 bytes");

/* RCR guard: every DS18B20 pass must fit one 8-bit RCR window (256 slots). */
_Static_assert(DS18B20_MATCH_SLOTS <= ONEWIRE_MAX_SLOTS,
               "Match ROM write must fit one RCR window");
_Static_assert(DS18B20_SCRATCHPAD_BITS <= ONEWIRE_MAX_SLOTS,
               "scratchpad read must fit one RCR window");
_Static_assert(DS18B20_SCRATCHPAD_LEN <= ONEWIRE_MAX_READ_BYTES,
               "scratchpad length must fit one read pass");

/** @brief Global single-command transaction context instance */
static ds18b20_txn_ctx_t txn_ctx;

/* Packing guard: pointer/wait first, flag byte last — 24 bytes, no padding.
 * Firmware-only: on the host the pointer is 8 bytes wide. */
#ifndef HOST_BUILD
_Static_assert(sizeof(txn_ctx) == 24, "transaction context must stay 24 bytes");
#endif

_Static_assert(DS18B20_RES_SLOTS_MAX <= ONEWIRE_MAX_SLOTS,
               "longest txn write must fit one RCR window");

/**
 * @}
 */

/**
 * @defgroup DS18B20_Private_Functions DS18B20 Private Functions
 * @{
 */

/**
 * @brief Default weak implementation for busy indicator (e.g. LED toggling during measurement)
 * @param[in] action 0 = idle, non-zero = busy
 */
__WEAK void ds18b20_busy(unsigned action) {
    (void)action;
    // Default implementation - empty (no LED control)
}

/**
 * @brief Default weak implementation for measurement completion callback
 * @param[in] temp_tenths Temperature value in tenths of degrees Celsius, or error code
 */
__WEAK void ds18b20_complete(int16_t temp_tenths) {
    (void)temp_tenths;
    // Default implementation - empty (no temperature handling)
}

/**
 * @brief Calculate Dallas/Maxim CRC-8 over a byte buffer
 * @param[in] data Input buffer
 * @param[in] len Number of bytes to process
 * @return CRC-8 checksum value
 * @note Delegates to the shared 1-Wire layer (same Dallas/Maxim algorithm).
 */
uint8_t ds18b20_crc8(const uint8_t* data, uint8_t len) {
    return onewire_crc8(data, len);
}

/**
 * @brief Calculate CRC8 checksum for DS18B20 scratchpad data validation
 * @return CRC8 checksum value
 */
__STATIC_FORCEINLINE uint8_t check_scratchpad_crc(void) {
    return ds18b20_crc8(ctx.scratchpad, DS18B20_CRC8_BYTES);
}

/**
 * @brief Decode pulse durations into scratchpad bytes using bit timing analysis
 * @note Branchless implementation: accumulates bits into native-width variable,
 *       then writes once per byte. Relies on union aliasing invariant — see DS18B20_ctx_t.
 */
__STATIC_FORCEINLINE void decode_scratchpad(void) {
    /* Captured pulse durations (volatile, written by the read DMA) carry one
     * bit each; onewire_decode_pulses() recovers the scratchpad bytes. */
    onewire_decode_pulses(ctx.scratchpad, ctx.pulse, DS18B20_SCRATCHPAD_LEN);
}

/**
 * @brief Convert raw temperature data from scratchpad to tenths of degrees Celsius
 * @return Temperature value in tenths of degrees Celsius
 */
__STATIC_FORCEINLINE int16_t decode_temperature(void) {
    // Combine LSB and MSB of temperature register (bytes 0 and 1)
    int16_t raw = (int16_t)((ctx.scratchpad[1] << 8) | ctx.scratchpad[0]);
    // Convert to tenths of degrees Celsius (raw value in 1/16th degrees):
    // multiply by 10 and divide by 16 with round-half-away-from-zero so the
    // sign is preserved for small negative values (raw = -1 would otherwise
    // truncate to 0 and report +0.0 °C for a temperature below freezing).
    return (int16_t)(((int32_t)raw * 10 + ((raw < 0) ? -8 : 8)) / 16);
}

/**
 * @brief Map a conversion resolution to its exact DS18B20 conversion time
 * @param[in] res Resolution in bits (9..12)
 * @param[out] arr Auto-reload value (one timer period in µs)
 * @param[out] rcr Repetition counter (number of periods - 1)
 * @note DS18B20 datasheet conversion times: 9-bit 93.75ms, 10-bit 187.5ms,
 *       11-bit 375ms, 12-bit 750ms. The (ARR, RCR) pairs below reproduce
 *       exactly those minimum waits at 1µs/tick with the invariant
 *       (RCR + 1) × ARR = wait in µs.
 */
__STATIC_FORCEINLINE void resolution_to_wait(uint8_t res, uint16_t* arr, uint8_t* rcr) {
    switch (res) {
    case 9:
        *arr = 9375;
        *rcr = 9;
        break; /* 10 × 9.375ms = 93.75ms */
    case 10:
        *arr = 18750;
        *rcr = 9;
        break; /* 10 × 18.75ms = 187.5ms */
    case 11:
        *arr = 18750;
        *rcr = 19;
        break; /* 20 × 18.75ms = 375ms */
    case 12:
    default:
        *arr = 62500;
        *rcr = 11;
        break; /* 12 × 62.5ms = 750ms */
    }
}

/**
 * @brief Wait for temperature conversion to complete
 * @note Non-blocking - starts a timer that generates an update event when the
 *       conversion of the currently configured resolution (ctx.resolution)
 *       is guaranteed finished: 93.75ms (9 bit) .. 750ms (12 bit).
 */
__STATIC_FORCEINLINE void wait_conversion(void) {
    uint16_t arr;
    uint8_t rcr;
    resolution_to_wait(ctx.resolution, &arr, &rcr);
    onewire_start_timer(arr, rcr);
}

/**
 * @brief Start inter-measurement pause period (5s)
 * @note Non-blocking - starts timer for inter-measurement delay
 */
/**
 * @}
 */

/* Include-only driver parts: the four functional modules of the
 * DS18B20 driver live in their own translation units of this file and
 * are compiled in here so the whole driver stays one object file. Each
 * part owns its private state (DS18B20_DRIVER_BUILD gate above) and is
 * guarded so it cannot be compiled standalone (it would miss the shared
 * ctx/txn_ctx/res_ctx/dev_roms statics below).
 *
 * The include order is load-bearing: every part references file-scope
 * declarations from the parts included above it (txn uses res_config_byte
 * from resolution; search uses txn_ctx, and measure uses dev_roms/dev_count
 * from search). clang-format must not alphabetise these lines. */
/* Phase enums are stored packed in uint8_t context fields (see above); all
 * values must fit. If a state is ever added past 255, the packing — not just
 * the asserts — needs revisiting. */
_Static_assert(DS18B20_ST_DECODE <= 255, "state enum must fit uint8_t packing");
_Static_assert(DS18B20_TXN_DONE <= 255, "txn phase enum must fit uint8_t packing");
/* Phase pulse workspace: one shared buffer reused by the mutually exclusive
 * bus-owner phases, instead of a dedicated static per phase.
 *
 * @warning CRITICAL INVARIANT: the four members alias the same storage.
 *          Each phase rebuilds its member in full before use, and the
 *          ownership guards (measurement/search/resolution/transaction reject
 *          while another owns the timer) guarantee no phase reads a member
 *          outside its own ownership window:
 *          - cmd: shared by the CONVERT and REQUEST command builds, which are
 *            strictly sequential (CONVERT completes via bus_done before
 *            REQUEST builds); build_skip_cmd() rewrites all entries plus the
 *            trailing release on every call.
 *          - res: rebuilt by build_res_pulses() on every set_resolution call;
 *            live only while the resolution change owns the timer.
 *          - txn: rebuilt by txn_build_pulses() on every txn_start; live only
 *            while the command transaction owns the timer.
 *          - addr: rebuilt in FULL by build_addr_cmd() on every addressed
 *            operation (prefix re-encoded from ctx.selected_rom plus the
 *            command byte plus the trailing release); live only while the
 *            owning write owns the timer. Per-operation rebuild (not once
 *            per selection) is what makes union membership safe: a
 *            transaction between two measurements may reuse the storage.
 *          Deliberately EXCLUDED (same storage must NOT be shared):
 *          - dev_roms: the scan device table must survive across measurement
 *            cycles, and 4_scan_mode runs a resolution write between search
 *            and scan rounds.
 *          - onewire.c search buffers: separate translation unit (generic
 *            1-Wire layer), so they cannot see this static; and there is
 *            nothing to overlay with inside onewire.c either (rom/prev_leaf
 *            must persist across the whole search, pulses only during CMD —
 *            all within one search run). Audited 2026-10-07.
 *          - txn raw[]/flags, res/measure state bytes: read after completion
 *            by the app and test accessors, so they stay in their structs.
 */
static union {
    ow_pulse_t cmd[DS18B20_DMA_TRANSFERS + 1]; /**< CONVERT/REQUEST command builds (shared, sequential) */
    ow_pulse_t res[DS18B20_RES_SLOTS_MAX + 1]; /**< resolution config writes */
    ow_pulse_t txn[DS18B20_RES_SLOTS_MAX + 1]; /**< command transaction builds */
    ow_pulse_t addr[DS18B20_MATCH_SLOTS + 1]; /**< addressed (Match ROM) command builds, rebuilt per op */
} phase_pulses;

/* B1 guards: the trailing ONEWIRE_RELEASE_PULSE consumed by the CCR3-feed
 * DMA's final transfer must always be present. Keep the per-member asserts
 * even though all members share storage: each documents the size contract
 * of the phase that fills it. */
_Static_assert(sizeof(phase_pulses.cmd) >= DS18B20_DMA_TRANSFERS + 1,
               "phase cmd must be DS18B20_DMA_TRANSFERS + 1 to hold the trailing "
               "bus-release pulse consumed by the 1-Wire layer");
_Static_assert(sizeof(phase_pulses.res) >= DS18B20_RES_SLOTS_MAX + 1,
               "phase res must be DS18B20_RES_SLOTS_MAX + 1 to hold the trailing "
               "bus-release pulse consumed by the 1-Wire layer");
_Static_assert(sizeof(phase_pulses.txn) >= DS18B20_RES_SLOTS_MAX + 1,
               "phase txn must be DS18B20_RES_SLOTS_MAX + 1 to hold the trailing "
               "bus-release pulse consumed by the 1-Wire layer");
_Static_assert(sizeof(phase_pulses.addr) >= DS18B20_MATCH_SLOTS + 1,
               "phase addr must be DS18B20_MATCH_SLOTS + 1 to hold the trailing "
               "bus-release pulse consumed by the 1-Wire layer");

/**
 * @brief Build the full Match ROM command table (0x55 + selected ROM + cmd)
 * @param[in] cmd_byte Command byte to send after the ROM address
 * @note Fills phase_pulses.addr in full on every call: the prefix depends
 *       only on the selected device, but rebuilding it per operation (instead
 *       of once per selection) is what lets the table live in the shared
 *       phase union — a transaction running between two measurements may
 *       reuse the same storage, and the next operation rebuilds before use.
 *       Cost is one synchronous re-encode (~72 slots, microseconds) per
 *       addressed operation; the 1-Wire timings are milliseconds.
 */
__STATIC_FORCEINLINE void build_addr_cmd(uint8_t cmd_byte) {
    ow_pulse_t* p = phase_pulses.addr;
    onewire_encode_byte(p, DS18B20_MATCH_ROM);
    p += DS18B20_BITS_PER_BYTE;
    for (uint8_t i = 0; i < DS18B20_ROM_BYTES; i++) {
        onewire_encode_byte(p, ctx.selected_rom[i]);
        p += DS18B20_BITS_PER_BYTE;
    }
    onewire_encode_byte(&phase_pulses.addr[DS18B20_PREFIX_SLOTS], cmd_byte);
    phase_pulses.addr[DS18B20_MATCH_SLOTS] = ONEWIRE_RELEASE_PULSE;
}
// clang-format off
#include "ds18b20_resolution.c"
#include "ds18b20_txn.c"
#include "ds18b20_search.c"
#include "ds18b20_measure.c"
// clang-format on

/**
 * @defgroup DS18B20_Public_Functions DS18B20 Public Functions
 * @{
 */

/**
 * @brief Initialize DS18B20 driver - configure clocks and peripherals
 * @pre Call exactly once after MCU reset, after the system clock is configured
 *      and before any 1-Wire bus operation or DS18B20 driver activity.
 * @note Calls onewire_init(), which takes exclusive ownership of the shared
 *       TIM1/DMA/GPIO resources for the lifetime of the driver (until reset),
 *       and parks the driver at DS18B20_ST_IDLE: nothing runs until the
 *       application calls ds18b20_start_measure() or a search API.
 * @warning This function is a bootstrap operation, not a reinitialization hook.
 *          A second call is unsupported and does not fully reset the timer,
 *          DMA, low-power or DS18B20 software state. Starting a new driver
 *          lifecycle requires an MCU reset.
 */
void ds18b20_init(void) {
    onewire_init();
    // No resolution change or command transaction running after init; the
    // DS18B20 powers up at 12 bit (750ms conversion), so wait for exactly that
    // until a scratchpad read or set_resolution tells us otherwise.
    res_ctx.flags |= DS18B20_RES_FLAG_FINISHED;
    txn_ctx.flags |= DS18B20_TXN_FLAG_FINISHED;
    ctx.resolution = DS18B20_RES_DEFAULT;
    ctx.scan_index = 0;
    // External power is the default wiring assumption; parasite-powered
    // setups opt in explicitly via ds18b20_set_parasite(). The flag byte
    // starts clear (address_mode/scan_mode/parasite all zero).
    ctx.flags = 0u;
}

/**
 * @brief Select which DS18B20 device to measure by its ROM address
 * @param[in] rom Pointer to the 8-byte ROM address (LSB first), or NULL to
 *                return to Skip ROM (broadcast) addressing
 * @note With a non-NULL address, the state machine sends Match ROM (0x55)
 *       plus the device address before each command, so only that device
 *       responds. Pass NULL (or a freshly initialised driver) to keep the
 *       legacy single-sensor Skip ROM behaviour. The address should come from
 *       the non-blocking device search (ds18b20_search_*).
 * @note The selection is applied only between measurement cycles (driver
 *       IDLE). Calls made while a cycle is running are ignored, including from
 *       the per-device scan callback (which the driver invokes at
 *       DS18B20_ST_DECODE mid-round): a select() there is rejected and the
 *       scan round continues. Applying a select mid-cycle would change
 *       ctx.selected_rom out from under the in-flight cycle, so the next
 *       addressed operation of that cycle would rebuild its Match ROM table
 *       for the wrong device. Re-call at IDLE (e.g. from
 *       ds18b20_complete() in single-device mode, or between rounds) to switch
 *       addressing.
 */
void ds18b20_select(const uint8_t* rom) {
    /* Select is accepted only when no bus transaction is in flight, i.e. at
     * driver IDLE. The per-device scan callback runs at DS18B20_ST_DECODE
     * mid-round: a select() there is rejected so it cannot interrupt the
     * in-progress scan. To leave scan mode, call select() between measurement
     * rounds (at IDLE) or from the single-device ds18b20_complete() callback
     * (which runs at IDLE). */
    if (ctx.current_state != DS18B20_ST_IDLE) {
        return;
    }
    if ((txn_ctx.flags & DS18B20_TXN_FLAG_FINISHED) == 0u) {
        // A command transaction is running - reject to keep its addressing.
        return;
    }
    if ((res_ctx.flags & DS18B20_RES_FLAG_FINISHED) == 0u) {
        // A resolution change is running - reject to keep its addressing.
        return;
    }
    if (onewire_search_active()) {
        // The device search owns the timer - reject to keep its addressing.
        return;
    }
    // Explicit single-device addressing: leave simultaneous-conversion mode.
    ctx.flags &= (uint8_t)~DS18B20_FLAG_SCAN_MODE;
    if (rom == 0) {
        ctx.flags &= (uint8_t)~DS18B20_FLAG_ADDRESS_MODE;
        return;
    }
    for (uint8_t i = 0; i < DS18B20_ROM_BYTES; i++) {
        ctx.selected_rom[i] = rom[i];
    }
    /* The Match ROM table itself is rebuilt from selected_rom on every
     * addressed operation (build_addr_cmd), so select only stores the ROM. */
    ctx.flags |= DS18B20_FLAG_ADDRESS_MODE;
}

/**
 * @}
 */
