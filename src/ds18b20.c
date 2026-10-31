#include "ds18b20.h"
#include "ds18b20_internal.h"
#include "onewire.h"
/* The driver needs two CMSIS compiler spellings (__WEAK, __STATIC_FORCEINLINE)
 * and nothing else from CMSIS; this private shim supplies them without pulling
 * in a device header. See src/ow_compiler.h. */
#include "ow_compiler.h"
/* The driver schedules the bus waits and kicks its own state machine through
 * the engine; that bridge is library-internal. See src/ow_glue.h. */
#include "ow_glue.h"
/* Intra-stack instrumentation, not a layer leak: stats emission needs
 * driver-owned concepts (device ROM, DS18B20_TEMP_ERROR_*) that the onewire
 * layer must not know, so the driver is the rightful emitter. Compiles away
 * when OW_STATS_ENABLE=0. */
#include "ow_stats.h"

/* Private sizing, context/phase types, flag bits and res_config_byte()
 * live in src/ds18b20_internal.h shared with the four parts. */

#define DS18B20_DRIVER_BUILD 1

/* DS18B20_ctx_t / ds18b20_txn_ctx_t and their flag bits live in
 * src/ds18b20_internal.h; the static instances follow. */

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
 * @return The datasheet conversion time in microseconds
 * @note DS18B20 datasheet conversion times: 9-bit 93.75ms, 10-bit 187.5ms,
 *       11-bit 375ms, 12-bit 750ms. The driver states the duration; how the
 *       timer is programmed to produce it is the engine's business.
 */
__STATIC_FORCEINLINE uint32_t resolution_wait_us(uint8_t res) {
    switch (res) {
    case 9:
        return 93750u; /* 93.75ms */
    case 10:
        return 187500u; /* 187.5ms */
    case 11:
        return 375000u; /* 375ms */
    case 12:
    default:
        return 750000u; /* 750ms */
    }
}

/**
 * @brief Wait for temperature conversion to complete
 * @note Non-blocking - starts a timer that generates an update event when the
 *       conversion of the currently configured resolution (ctx.resolution)
 *       is guaranteed finished: 93.75ms (9 bit) .. 750ms (12 bit).
 */
__STATIC_FORCEINLINE void wait_conversion(void) {
    ow_glue_wait_start(resolution_wait_us(ctx.resolution));
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
 * ctx/txn_ctx/res_ctx/dev_roms statics below, whose types and the
 * cross-part leaf res_config_byte() live in src/ds18b20_internal.h).
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
