#include "ds18b20.h"
#include "onewire.h"
#include "ow_port.h"
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
/** @brief Timer configuration for wait and pause (ARR, RCR) — 62500 ticks @ 1µs = 62.5ms per period */
#define OW_PAUSE_US (DS18B20_CYCLE_PAUSE_US > 0 ? DS18B20_CYCLE_PAUSE_US : 1)
#if OW_PAUSE_US <= 62500
#define PAUSE_ARR (OW_PAUSE_US)
#define PAUSE_RCR 0
#else
#define PAUSE_ARR 62500
#define PAUSE_RCR ((OW_PAUSE_US / 62500) - 1)
#endif
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
    ds18b20_state_t current_state; /**< Current state of the state machine */
    uint8_t address_mode; /**< 0 = Skip ROM (all devices), non-zero = Match ROM */
    uint8_t scan_mode; /**< 1 = simultaneous multi-device conversion (scan) mode */
    uint8_t scan_index; /**< Index of the device currently read in scan mode */
    uint8_t selected_rom[DS18B20_ROM_BYTES]; /**< ROM of the selected device */
    uint8_t addr_cmd[DS18B20_MATCH_SLOTS + 1]; /**< Pulse buffer for Match ROM command (+ ONEWIRE_RELEASE_PULSE for hardware bus release) */
    uint8_t resolution; /**< Conversion resolution in bits (9..12); drives the conversion wait */
    uint8_t parasite; /**< 1 = parasite-powered bus: engage the strong pull-up during conversion and EEPROM programming windows (see ds18b20_set_parasite) */
} DS18B20_ctx_t;

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
 *       as the resolution state machine. The pulse buffer must stay valid
 *       across poll calls because the DMA feeds CCR3 from it asynchronously.
 */
typedef struct {
    ds18b20_txn_phase_t phase; /**< Current phase of the transaction */
    uint8_t command; /**< DS18B20 function command byte (0x33/0x4E/0x48/0xB8/0xB4/0xBE) */
    uint8_t* out; /**< User result buffer (valid until the command finishes) */
    uint8_t payload[3]; /**< Write Scratchpad payload (TH, TL, CFG) */
    uint8_t payload_len; /**< 0..3 (payload bytes written after the command) */
    uint8_t read_bytes; /**< Bytes to read back (0 = no read phase) */
    uint16_t wait_us; /**< Timed wait after the command (0 = none) */
    uint8_t bare; /**< 1 = no addressing prefix (Read ROM: single-device bus only) */
    uint8_t slots; /**< Bit slots in the built pulses (incl. prefix and payload) */
    uint8_t pulses[DS18B20_RES_SLOTS_MAX + 1]; /**< Built command (+ ONEWIRE_RELEASE_PULSE for hardware bus release) */
    uint8_t raw[DS18B20_SCRATCHPAD_LEN]; /**< Decoded read result */
    uint8_t ok; /**< 1 once the transaction completed with a device present / valid read */
    uint8_t finished; /**< 1 once the transaction finished (or aborted) */
} ds18b20_txn_ctx_t;

/**
 * @}
 */

/**
 * @defgroup DS18B20_Private_Variables DS18B20 Private Variables
 * @{
 */

/** @brief Global driver context instance */
static DS18B20_ctx_t ctx;

/* B1 guard: the 1-Wire layer reads cmd[slots] as the trailing
 * ONEWIRE_RELEASE_PULSE that the final DMA transfer feeds into CCR3 to
 * release the 1-Wire bus. The addr_cmd buffer must therefore hold
 * DS18B20_MATCH_SLOTS + 1 entries, not DS18B20_MATCH_SLOTS, or that last
 * slot reads one byte past the buffer. */
_Static_assert(sizeof(ctx.addr_cmd) >= DS18B20_MATCH_SLOTS + 1,
               "addr_cmd must be DS18B20_MATCH_SLOTS + 1 to hold the trailing "
               "bus-release pulse consumed by the 1-Wire layer");
/* RCR guard: every DS18B20 pass must fit one 8-bit RCR window (256 slots). */
_Static_assert(DS18B20_MATCH_SLOTS <= ONEWIRE_MAX_SLOTS,
               "Match ROM write must fit one RCR window");
_Static_assert(DS18B20_SCRATCHPAD_BITS <= ONEWIRE_MAX_SLOTS,
               "scratchpad read must fit one RCR window");
_Static_assert(DS18B20_SCRATCHPAD_LEN <= ONEWIRE_MAX_READ_BYTES,
               "scratchpad length must fit one read pass");

/** @brief Global single-command transaction context instance */
static ds18b20_txn_ctx_t txn_ctx;

/* B1 guard: the trailing ONEWIRE_RELEASE_PULSE consumed by the CCR3-feed
 * DMA's final transfer must always be present at the exact slot index used
 * for the write (see txn_build_pulses); the buffer is sized for the longest
 * (Match ROM) command write. */
_Static_assert(sizeof(txn_ctx.pulses) >= DS18B20_RES_SLOTS_MAX + 1,
               "txn_ctx.pulses must be DS18B20_RES_SLOTS_MAX + 1 to hold the "
               "trailing bus-release pulse consumed by the 1-Wire layer");
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
__STATIC_FORCEINLINE void start_cycle_pause(void) { onewire_start_timer(PAUSE_ARR, PAUSE_RCR); }

/**
 * @brief Build the invariant Match ROM prefix (0x55 + selected ROM)
 * @note Fills the first DS18B20_PREFIX_SLOTS entries of ctx.addr_cmd.
 *       The prefix depends only on the selected device, so it is built
 *       once in ds18b20_select() and reused for every command.
 */
__STATIC_FORCEINLINE void build_addr_prefix(void) {
    uint8_t* p = ctx.addr_cmd;
    onewire_encode_byte(p, DS18B20_MATCH_ROM);
    p += DS18B20_BITS_PER_BYTE;
    for (uint8_t i = 0; i < DS18B20_ROM_BYTES; i++) {
        onewire_encode_byte(p, ctx.selected_rom[i]);
        p += DS18B20_BITS_PER_BYTE;
    }
    /* B1: guarantee the trailing ONEWIRE_RELEASE_PULSE that the 1-Wire layer
     * reads as its final DMA transfer into CCR3 is present, even though
     * build_addr_cmd() only ever writes slots 0 .. DS18B20_MATCH_SLOTS - 1.
     * Without this, the bus-release pulse would depend on whatever happened
     * to sit at addr_cmd[DS18B20_MATCH_SLOTS] (typically 0 from .bss, but not
     * guaranteed). */
    ctx.addr_cmd[DS18B20_MATCH_SLOTS] = ONEWIRE_RELEASE_PULSE;
}

/**
 * @brief Append one command byte to the pre-built Match ROM prefix
 * @param[in] cmd_byte Command byte to send after the ROM address
 * @note Requires build_addr_prefix() to have been called for the current
 *       selected device. Only the last byte (8 slots) is re-encoded per call.
 */
__STATIC_FORCEINLINE void build_addr_cmd(uint8_t cmd_byte) {
    onewire_encode_byte(&ctx.addr_cmd[DS18B20_PREFIX_SLOTS], cmd_byte);
}

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
 * @note Calls onewire_init(), which takes exclusive ownership of the shared
 *       TIM1/DMA1/GPIO resources for the lifetime of the driver (until reset),
 *       and marks the driver idle so the measurement state machine owns the
 *       timer until the application starts a device search.
 */
void ds18b20_init(void) {
    onewire_init();
    // No resolution change or command transaction running after init; the
    // DS18B20 powers up at 12 bit (750ms conversion), so wait for exactly that
    // until a scratchpad read or set_resolution tells us otherwise.
    res_ctx.finished = 1;
    txn_ctx.finished = 1;
    ctx.resolution = DS18B20_RES_DEFAULT;
    ctx.scan_mode = 0;
    ctx.scan_index = 0;
    // External power is the default wiring assumption; parasite-powered
    // setups opt in explicitly via ds18b20_set_parasite().
    ctx.parasite = 0;
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
 *       scan round continues. Applying a select mid-cycle would overwrite
 *       ctx.addr_cmd while the DMA is still feeding it, corrupting the
 *       in-flight bus transaction. Re-call at IDLE (e.g. from
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
    if (!txn_ctx.finished) {
        // A command transaction is running - reject to keep its addressing.
        return;
    }
    if (!res_ctx.finished) {
        // A resolution change is running - reject to keep its addressing.
        return;
    }
    if (onewire_search_active()) {
        // The device search owns the timer - reject to keep its addressing.
        return;
    }
    // Explicit single-device addressing: leave simultaneous-conversion mode.
    ctx.scan_mode = 0;
    if (rom == 0) {
        ctx.address_mode = 0;
        return;
    }
    for (uint8_t i = 0; i < DS18B20_ROM_BYTES; i++) {
        ctx.selected_rom[i] = rom[i];
    }
    build_addr_prefix(); // Build the invariant Match ROM prefix once per selection
    ctx.address_mode = 1;
}

/**
 * @}
 */
