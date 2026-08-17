#include "ds18b20.h"
#include "macro.h"
#include "onewire.h"
#include "stm32f1xx.h"

/**
 * @defgroup DS18B20_Private_Types DS18B20 Private Types
 * @{
 */

/**
 * @defgroup DS18B20_Private_Constants DS18B20 Private Constants
 * @{
 */

/** @brief Duration of '1' bit pulse in microseconds (from the 1-Wire layer) */
#define ONE_PULSE ONEWIRE_ONE_PULSE
/** @brief Duration of '0' bit pulse in microseconds (from the 1-Wire layer) */
#define ZERO_PULSE ONEWIRE_ZERO_PULSE
/** @brief Guard band between slots to prevent overlap due to bus rise time and DMA latency */
#define GUARD_BAND ONEWIRE_GUARD_BAND
/** @brief Total length of DS18B20 scratchpad in bytes */
#define DS18B20_SCRATCHPAD_LEN 9
/** @brief Number of bytes to include in the scratchpad CRC calculation */
#define DS18B20_CRC8_BYTES 8
/** @brief Total number of bits in DS18B20 scratchpad */
#define DS18B20_SCRATCHPAD_BITS (DS18B20_SCRATCHPAD_LEN * DS18B20_BITS_PER_BYTE)
/** @brief Pulse threshold separating short ('1') from long ('0') slots (from the 1-Wire layer) */
#define SHORT_PULSE_MAX ONEWIRE_SHORT_PULSE_MAX
/** @brief Total slots for Match ROM + 8-byte ROM + command */
#define DS18B20_MATCH_SLOTS ((DS18B20_ROM_BYTES + 2) * DS18B20_BITS_PER_BYTE)
/** @brief Slots for the invariant Match ROM + 8-byte ROM prefix (built on select) */
#define DS18B20_PREFIX_SLOTS ((DS18B20_ROM_BYTES + 1) * DS18B20_BITS_PER_BYTE)
/** @brief Number of DMA transfers for command transmission (2 bytes × 8 bits) */
#define DS18B20_DMA_TRANSFERS (2 * DS18B20_BITS_PER_BYTE)
/** @brief Timer configuration for wait and pause (ARR, RCR) — 62500 ticks @ 1µs = 62.5ms per period */
#define PAUSE_750MS 62500, 11 /**< 750ms delay for temperature conversion (62.5ms × 12) */
#define PAUSE_5S 62500, 79 /**< 5s pause between measurement cycles (62.5ms × 80) */
#define SCAN_DEVICE_GAP 1000, 0 /**< 1ms scheduling bridge between scan-mode device reads (no bus requirement) */
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
 * @brief Convert byte bit to pulse duration (ONE_PULSE µs for '1', ZERO_PULSE µs for '0')
 * @param B Byte value
 * @param N Bit position (0-7)
 * @return Pulse duration in microseconds
 */
#define B2P(B, N) (((B) & (1 << (N))) ? ONE_PULSE : ZERO_PULSE)

/**
 * @brief Convert entire byte to sequence of pulse durations for transmission
 * @param B Byte value to convert
 */
#define BYTE_TO_PULSES(B)                       \
    B2P(B, 0), B2P(B, 1), B2P(B, 2), B2P(B, 3), \
        B2P(B, 4), B2P(B, 5), B2P(B, 6), B2P(B, 7)

/** @brief DS18B20 Convert T command sequence in pulse duration format */
static const uint8_t conv_cmd[] = {BYTE_TO_PULSES(0xCC), BYTE_TO_PULSES(0x44), 0};

/** @brief DS18B20 Read Scratchpad command sequence in pulse duration format */
static const uint8_t read_cmd[] = {BYTE_TO_PULSES(0xCC), BYTE_TO_PULSES(0xBE), 0};

/**
 * @}
 */

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
        volatile uint16_t edge[DS18B20_SCRATCHPAD_BITS / 2]; /**< Edge timestamps for presence detection */
        volatile uint8_t pulse[DS18B20_SCRATCHPAD_BITS]; /**< Pulse durations for data decoding */
        uint8_t scratchpad[DS18B20_SCRATCHPAD_LEN]; /**< Sensor scratchpad data */
        uint64_t fill_union; /**< Utility field for filling the union */
    };
    ds18b20_state_t current_state; /**< Current state of the state machine */
    uint8_t address_mode; /**< 0 = Skip ROM (all devices), non-zero = Match ROM */
    uint8_t scan_mode; /**< 1 = simultaneous multi-device conversion (scan) mode */
    uint8_t scan_index; /**< Index of the device currently read in scan mode */
    uint8_t selected_rom[DS18B20_ROM_BYTES]; /**< ROM of the selected device */
    uint8_t addr_cmd[DS18B20_MATCH_SLOTS + 1]; /**< Pulse buffer for Match ROM command (+ trailing 0 for hardware bus release) */
    uint8_t resolution; /**< Conversion resolution in bits (9..12); drives the conversion wait */
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
 *       across poll calls because the DMA feeds CCR1 from it asynchronously.
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
    uint8_t pulses[DS18B20_RES_SLOTS_MAX + 1]; /**< Built command (+ trailing 0 for hardware bus release) */
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

/** @brief ROM table of the discovered devices (filled by the device search). */
static uint8_t dev_roms[DS18B20_MAX_DEVICES][DS18B20_ROM_BYTES];
/** @brief Number of devices currently stored in dev_roms. */
static uint8_t dev_count;

/* B1 guard: the 1-Wire layer reads cmd[slots] as the trailing zero-pulse that
 * the final DMA transfer feeds into CCR1 to release the 1-Wire bus. The
 * addr_cmd buffer must therefore hold DS18B20_MATCH_SLOTS + 1 entries, not
 * DS18B20_MATCH_SLOTS, or that last slot reads one byte past the buffer. */
_Static_assert(sizeof(ctx.addr_cmd) >= DS18B20_MATCH_SLOTS + 1,
               "addr_cmd must be DS18B20_MATCH_SLOTS + 1 to hold the trailing "
               "bus-release pulse consumed by the 1-Wire layer");

/** @brief Global single-command transaction context instance */
static ds18b20_txn_ctx_t txn_ctx;

/* B1 guard: the trailing zero-pulse consumed by the CCR1-feed DMA's final
 * transfer must always be present at the exact slot index used for the write
 * (see txn_build_pulses); the buffer is sized for the longest (Match ROM)
 * command write. */
_Static_assert(sizeof(txn_ctx.pulses) >= DS18B20_RES_SLOTS_MAX + 1,
               "txn_ctx.pulses must be DS18B20_RES_SLOTS_MAX + 1 to hold the "
               "trailing bus-release pulse consumed by the 1-Wire layer");

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
    for (unsigned byte = 0; byte < DS18B20_SCRATCHPAD_LEN; ++byte) {
        const unsigned base = byte * DS18B20_BITS_PER_BYTE;
        unsigned value = 0;
        for (unsigned bit = 0; bit < DS18B20_BITS_PER_BYTE; ++bit) {
            value |= (unsigned)(ctx.pulse[base + bit] <= SHORT_PULSE_MAX) << bit;
        }
        ctx.scratchpad[byte] = (uint8_t)value;
    }
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
__STATIC_FORCEINLINE void start_cycle_pause(void) { onewire_start_timer(PAUSE_5S); }

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
    /* B1: guarantee the trailing zero-pulse that the 1-Wire layer reads as its
     * final DMA transfer into CCR1 is present, even though build_addr_cmd()
     * only ever writes slots 0 .. DS18B20_MATCH_SLOTS - 1. Without this, the
     * bus-release pulse would depend on whatever happened to sit at
     * addr_cmd[DS18B20_MATCH_SLOTS] (typically 0 from .bss, but not guaranteed). */
    ctx.addr_cmd[DS18B20_MATCH_SLOTS] = 0;
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

/**
 * @defgroup DS18B20_Search_Internal DS18B20 Device Search (via the 1-Wire layer)
 * @brief Wraps the generic Search ROM (0xF0) / Alarm Search (0xEC) engine of
 *        the shared 1-Wire layer. The device search additionally stores every
 *        found ROM in the scan-mode device table; the alarm search leaves the
 *        table untouched so a previous scan keeps its addresses.
 * @{
 */

/** @brief User sink stored for the duration of a search */
static ds18b20_search_sink_t search_user_sink;

/**
 * @brief Device-search sink: store the ROM in the scan-mode device table
 *        (capped at DS18B20_MAX_DEVICES), then forward to the user sink.
 */
static uint8_t search_store_sink(const uint8_t* rom) {
    if (dev_count < DS18B20_MAX_DEVICES) {
        for (uint8_t i = 0; i < DS18B20_ROM_BYTES; i++) {
            dev_roms[dev_count][i] = rom[i];
        }
        dev_count++;
    }
    return search_user_sink ? search_user_sink(rom) : 0;
}

/**
 * @brief Alarm-search sink: forward to the user sink without touching the
 *        scan-mode device table.
 */
static uint8_t search_alarm_sink(const uint8_t* rom) {
    return search_user_sink ? search_user_sink(rom) : 0;
}

/**
 * @brief Start a non-blocking device search
 * @param[in] sink Callback invoked per found DS18B20 device (may be NULL)
 * @param[in] max_devices Maximum number of devices to report (0 aborts)
 * @note The device search (re)populates the scan-mode device table.
 * @note Ownership guards: the search and the measurement state machine share
 *       TIM1/DMA, so a new search may only be started while the measurement
 *       state machine is IDLE; a running search rejects a new start.
 */
void ds18b20_search_start(ds18b20_search_sink_t sink, uint8_t max_devices) {
    if (ctx.current_state != DS18B20_ST_IDLE) {
        return; // a measurement cycle is in progress
    }
    if (onewire_search_active()) {
        return; // a search is already running - keep its sink and table
    }
    if (!txn_ctx.finished) {
        return; // a command transaction is running
    }
    dev_count = 0;
    search_user_sink = sink;
    onewire_search_start(search_store_sink, max_devices, DS18B20_SEARCH_ROM, DS18B20_FAMILY_CODE);
}

/**
 * @brief Start a non-blocking alarm search
 * @param[in] sink Callback invoked per DS18B20 currently in alarm (may be NULL)
 * @param[in] max_devices Maximum number of alarmed devices to report (0 aborts)
 * @note Only devices in alarm state respond to Alarm Search (0xEC). The
 *       scan-mode device table is left untouched.
 */
void ds18b20_alarm_search_start(ds18b20_search_sink_t sink, uint8_t max_devices) {
    if (ctx.current_state != DS18B20_ST_IDLE) {
        return; // a measurement cycle is in progress
    }
    if (onewire_search_active()) {
        return; // a search is already running - keep its sink
    }
    if (!txn_ctx.finished) {
        return; // a command transaction is running
    }
    search_user_sink = sink;
    onewire_search_start(search_alarm_sink, max_devices, DS18B20_ALARM_SEARCH, DS18B20_FAMILY_CODE);
}

/**
 * @brief Advance the non-blocking device search by one hardware operation
 * @return 1 when the search is finished, 0 while still running
 */
uint8_t ds18b20_search_poll(void) { return onewire_search_poll(); }

/**
 * @brief Number of DS18B20 devices found (valid once the search finished)
 * @return Count of found devices
 */
uint8_t ds18b20_search_count(void) { return onewire_search_count(); }

/**
 * @brief Advance the non-blocking alarm search by one hardware operation
 * @return 1 when the search is finished, 0 while still running
 */
uint8_t ds18b20_alarm_search_poll(void) { return onewire_search_poll(); }

/**
 * @brief Number of DS18B20 devices found in alarm (valid once finished)
 * @return Count of alarmed devices
 */
uint8_t ds18b20_alarm_search_count(void) { return onewire_search_count(); }

/**
 * @}
 */

/**
 * @defgroup DS18B20_Resolution_Internal DS18B20 Internal Non-Blocking Resolution Change
 * @brief Change the temperature conversion resolution (9..12 bit) with the same
 *        non-blocking discipline as the device search: every state performs
 *        exactly one hardware-timed operation via the internal bus primitives,
 *        so a poll call never blocks. The config write is sent with Write
 *        Scratchpad (0x4E) + TH + TL + CFG; it takes effect immediately and is
 *        not persisted to the EEPROM (no Copy Scratchpad, which would need a
 *        strong pull-up under parasitic power).
 * @{
 */

/** @brief Resolution state machine phases */
typedef enum {
    DS18B20_RES_RESET, /**< reset scheduled; check presence */
    DS18B20_RES_WRITE, /**< config write scheduled (skip/match + 0x4E + TH + TL + CFG) */
    DS18B20_RES_DONE /**< operation finished; hand the timer back to the measurement */
} res_phase_t;

/**
 * @brief Non-blocking resolution change context
 * @note The pulse buffer must stay valid across poll calls because the DMA
 *       feeds CCR1 from it asynchronously while the config write is sent.
 */
typedef struct {
    res_phase_t phase; /**< Current phase of the resolution state machine */
    uint8_t pending_res; /**< Resolution (bits) to apply */
    uint8_t applied; /**< 1 once the config write completed (resolution actually changed) */
    uint8_t finished; /**< 1 once the operation has completed (or aborted) */
    uint8_t pulses[DS18B20_RES_SLOTS_MAX + 1]; /**< Pulse buffer for the config write (+ trailing 0 for hardware bus release) */
} res_ctx_t;

/** @brief Global resolution context instance */
static res_ctx_t res_ctx;

/* B1 guard: the trailing zero-pulse consumed by the CCR1-feed DMA's final
 * transfer must always be present at the exact slot index used for the write
 * (see build_res_pulses); the buffer is sized for the longest (Match ROM) mode. */
_Static_assert(sizeof(res_ctx.pulses) >= DS18B20_RES_SLOTS_MAX + 1,
               "res_ctx.pulses must be DS18B20_RES_SLOTS_MAX + 1 to hold the "
               "trailing bus-release pulse consumed by the 1-Wire layer");

/**
 * @brief Build the DS18B20 configuration register byte for a resolution
 * @param[in] res Resolution in bits (9..12)
 * @return Configuration register byte (R1/R0 bits set, rest at reset value)
 * @note 9 bit -> 0x1F, 10 bit -> 0x3F, 11 bit -> 0x5F, 12 bit -> 0x7F.
 */
__STATIC_FORCEINLINE uint8_t res_config_byte(uint8_t res) {
    return (uint8_t)(0x1Fu | ((res - DS18B20_RES_MIN) << 5));
}

/**
 * @brief Pre-build the resolution config write into res_ctx.pulses
 * @param[in] res Resolution in bits (9..12)
 * @note Encodes Skip ROM (0xCC) or Match ROM (0x55 + selected ROM) followed by
 *       Write Scratchpad (0x4E), TH, TL and the config byte. The trailing
 *       zero-pulse that the 1-Wire layer consumes as the final DMA transfer
 *       (hardware bus release) is written at the slot index of the mode
 *       actually used, not always at the end of the buffer.
 */
__STATIC_FORCEINLINE void build_res_pulses(uint8_t res) {
    // In scan mode the config write must reach every sensor, so the Match ROM
    // address is skipped even if a single-device address is still selected.
    const uint8_t use_match = ctx.address_mode && !ctx.scan_mode;
    uint8_t* p = res_ctx.pulses;
    if (use_match) {
        onewire_encode_byte(p, DS18B20_MATCH_ROM);
        p += DS18B20_BITS_PER_BYTE;
        for (uint8_t i = 0; i < DS18B20_ROM_BYTES; i++) {
            onewire_encode_byte(p, ctx.selected_rom[i]);
            p += DS18B20_BITS_PER_BYTE;
        }
    } else {
        onewire_encode_byte(p, 0xCC); /* Skip ROM */
        p += DS18B20_BITS_PER_BYTE;
    }
    onewire_encode_byte(p, DS18B20_WRITE_SCRATCHPAD);
    p += DS18B20_BITS_PER_BYTE;
    onewire_encode_byte(p, DS18B20_RES_TH);
    p += DS18B20_BITS_PER_BYTE;
    onewire_encode_byte(p, DS18B20_RES_TL);
    p += DS18B20_BITS_PER_BYTE;
    onewire_encode_byte(p, res_config_byte(res));
    res_ctx.pulses[use_match ? DS18B20_RES_SLOTS_MAX : DS18B20_RES_SLOTS_MIN] = 0;
}

/**
 * @brief Start a non-blocking resolution change
 * @param[in] bits New resolution in bits: DS18B20_RES_MIN (9) .. DS18B20_RES_MAX (12)
 * @note Out-of-range values are ignored. The change is scheduled only between
 *       measurement cycles and only while the device search is idle; otherwise
 *       it is ignored. While running, it owns TIM1/DMA; poll it with
 *       ds18b20_set_resolution_poll() until it reports completion, then call
 *       ds18b20_poll() again to resume measuring with the new resolution.
 */
void ds18b20_set_resolution(uint8_t bits) {
    if (bits < DS18B20_RES_MIN || bits > DS18B20_RES_MAX) {
        return; // out of range - ignore
    }
    if (!res_ctx.finished) {
        return; // a resolution change is already running
    }
    if (!txn_ctx.finished) {
        return; // a command transaction is running
    }
    if (onewire_search_active()) {
        return; // the device search owns the timer
    }
    if (ctx.current_state != DS18B20_ST_IDLE) {
        return; // a measurement cycle is in progress
    }
    res_ctx.pending_res = bits;
    res_ctx.applied = 0;
    res_ctx.finished = 0;
    build_res_pulses(bits); // Pre-build the config write for the current address mode
    res_ctx.phase = DS18B20_RES_RESET;
    onewire_reset(ctx.edge); // Schedule the first hardware operation
}

/**
 * @brief Advance the non-blocking resolution change by one hardware operation
 * @return 1 when the change is finished (successfully or aborted), 0 while running
 * @note When this returns 1 the next measurement uses the requested resolution
 *       if (and only if) the config write actually completed; an aborted change
 *       (e.g. no device present) leaves the resolution unchanged.
 */
uint8_t ds18b20_set_resolution_poll(void) {
    if (res_ctx.finished) {
        return 1;
    }

    if (res_ctx.phase == DS18B20_RES_DONE) {
        // The last hardware operation completed (config written or aborted):
        // hand the timer back to the measurement state machine exactly once.
        T1.EGR = TIM_EGR(UG);
        __DSB();
        if (res_ctx.applied) {
            ctx.resolution = res_ctx.pending_res;
        }
        res_ctx.finished = 1;
        return 1;
    }

    // Wait for the currently scheduled hardware operation to complete.
    // This is a non-blocking poll, not a busy-wait.
    if (!onewire_bus_done()) {
        return 0;
    }

    switch (res_ctx.phase) {
    case DS18B20_RES_RESET:
        // Reset completed: a presence pulse means at least one device is on
        // the bus, so send the config write for the requested resolution.
        if (!onewire_present(ctx.edge)) {
            res_ctx.phase = DS18B20_RES_DONE;
            break;
        }
        onewire_write_slots(res_ctx.pulses,
                            ctx.address_mode ? DS18B20_RES_SLOTS_MAX
                                             : DS18B20_RES_SLOTS_MIN);
        res_ctx.phase = DS18B20_RES_WRITE;
        break;

    case DS18B20_RES_WRITE:
        // Config write completed: the sensor now uses the new resolution.
        res_ctx.applied = 1;
        res_ctx.phase = DS18B20_RES_DONE;
        break;

    case DS18B20_RES_DONE:
    default:
        break;
    }

    return 0;
}

/**
 * @brief Current conversion resolution in bits
 * @return Resolution in bits (9..12); the default is 12
 * @note Auto-derived from the last valid scratchpad read (byte 4, R1/R0),
 *       so it also tracks a resolution changed externally.
 */
uint8_t ds18b20_get_resolution(void) { return ctx.resolution; }

/**
 * @brief Finish the current scan-mode device read
 * @note Called after every per-device report in scan mode. Advances to the
 *       next device (CONTINUE, skipping a fresh conversion) or, after the last
 *       device, returns to IDLE and starts the inter-measurement pause so the
 *       next round begins with a new broadcast Convert T. In single-device
 *       mode it only starts the inter-measurement pause.
 */
static void scan_finish_or_next(void) {
    if (!ctx.scan_mode) {
        /* Single-device mode, or a select() issued from the scan callback that
         * switched us out of scan mode: return the state machine to IDLE so the
         * next poll starts a fresh (single-device) cycle instead of re-entering
         * the DECODE state it was left in. */
        ctx.current_state = DS18B20_ST_IDLE;
        start_cycle_pause();
        return;
    }
    ctx.scan_index++;
    if (ctx.scan_index < dev_count) {
        ctx.current_state = DS18B20_ST_CONTINUE;
        /* DECODE armed nothing, so without a running timer no UIF would ever
         * drive the CONTINUE state again (single-device mode gets its UIF from
         * the inter-measurement pause). Arm a short scheduling delay: its UIF
         * is the bridge to CONTINUE, which then arms the real bus reset. */
        onewire_start_timer(SCAN_DEVICE_GAP);
    } else {
        ctx.current_state = DS18B20_ST_IDLE;
        start_cycle_pause();
    }
}

/**
 * @brief Begin simultaneous conversion of every discovered device
 * @see ds18b20_scan_start() in ds18b20.h
 */
void ds18b20_scan_start(void) {
    if (ctx.current_state != DS18B20_ST_IDLE) {
        return; // a measurement cycle is in progress
    }
    if (onewire_search_active() || !res_ctx.finished || !txn_ctx.finished) {
        return; // the search, a resolution change or a command owns the timer
    }
    if (dev_count == 0) {
        return; // nothing discovered: there is no device to convert
    }
    ctx.scan_mode = 1;
    ctx.scan_index = 0;
}

/**
 * @brief Number of DS18B20 devices stored by the driver
 * @see ds18b20_device_count() in ds18b20.h
 */
uint8_t ds18b20_device_count(void) { return dev_count; }

/**
 * @brief ROM address of a discovered device
 * @see ds18b20_device_rom() in ds18b20.h
 */
const uint8_t* ds18b20_device_rom(uint8_t index) {
    if (index >= dev_count) {
        return 0;
    }
    return dev_roms[index];
}

/**
 * @brief Index of the device whose result ds18b20_complete() just reported
 * @see ds18b20_scan_index() in ds18b20.h
 */
uint8_t ds18b20_scan_index(void) { return ctx.scan_index; }

/**
 * @}
 */

/**
 * @defgroup DS18B20_Command_Impl DS18B20 Non-Blocking Command Transactions
 * @brief Shared non-blocking engine for the infrequent DS18B20 commands that
 *        the measurement state machine does not issue: Read ROM (0x33),
 *        Write Scratchpad thresholds (0x4E), Copy Scratchpad (0x48), Recall
 *        EEPROM (0xB8), Read Power Supply (0xB4) and raw Read Scratchpad
 *        (0xBE). Every command runs reset -> presence -> write -> (read |
 *        timed wait) -> done, one hardware-timed operation per poll call, so
 *        the same non-blocking discipline as the measurement, search and
 *        resolution state machines is preserved. Each command owns TIM1/DMA
 *        while it runs and hands the timer back to ds18b20_poll() when done.
 * @{
 */

/**
 * @brief Ownership guard shared by every command transaction start
 * @return 1 when a new transaction may be scheduled
 */
__STATIC_FORCEINLINE uint8_t txn_can_start(void) {
    /* A scan session owns the timer for its whole duration (scan_mode stays 1
     * until ds18b20_select() clears it): a command transaction started then
     * would clobber the in-flight measurement/scan cycle, so it must be
     * rejected. Without this check a command could slip through during the
     * brief IDLE pause between scan rounds. */
    return (uint8_t)(ctx.current_state == DS18B20_ST_IDLE &&
                     !ctx.scan_mode && !onewire_search_active() &&
                     res_ctx.finished && txn_ctx.finished);
}

/**
 * @brief Build the command pulse sequence into txn_ctx.pulses
 * @note Encodes the addressing prefix (Skip ROM 0xCC, or Match ROM 0x55 +
 *       selected ROM; none for a bare command such as Read ROM), the function
 *       command byte and the optional payload (Write Scratchpad TH/TL/CFG).
 *       The trailing zero-pulse that the 1-Wire layer consumes as the final
 *       DMA transfer (hardware bus release) is written at the slot index of
 *       the mode actually used, not always at the end of the buffer.
 */
__STATIC_FORCEINLINE void txn_build_pulses(void) {
    // In scan mode the command must reach every sensor, so the Match ROM
    // address is skipped even if a single-device address is still selected.
    const uint8_t use_match = ctx.address_mode && !ctx.scan_mode && !txn_ctx.bare;
    uint8_t* p = txn_ctx.pulses;
    uint8_t bytes = 0;
    if (!txn_ctx.bare) {
        if (use_match) {
            onewire_encode_byte(p, DS18B20_MATCH_ROM);
            p += DS18B20_BITS_PER_BYTE;
            bytes++;
            for (uint8_t i = 0; i < DS18B20_ROM_BYTES; i++) {
                onewire_encode_byte(p, ctx.selected_rom[i]);
                p += DS18B20_BITS_PER_BYTE;
                bytes++;
            }
        } else {
            onewire_encode_byte(p, 0xCC); /* Skip ROM */
            p += DS18B20_BITS_PER_BYTE;
            bytes++;
        }
    }
    onewire_encode_byte(p, txn_ctx.command);
    p += DS18B20_BITS_PER_BYTE;
    bytes++;
    for (uint8_t i = 0; i < txn_ctx.payload_len; i++) {
        onewire_encode_byte(p, txn_ctx.payload[i]);
        p += DS18B20_BITS_PER_BYTE;
        bytes++;
    }
    txn_ctx.slots = (uint8_t)(bytes * DS18B20_BITS_PER_BYTE);
    /* B1: guarantee the trailing zero-pulse that the 1-Wire layer reads as its
     * final DMA transfer into CCR1, even though the command write only ever
     * fills slots 0 .. slots - 1 (see build_res_pulses for the same pattern). */
    txn_ctx.pulses[txn_ctx.slots] = 0;
}

/**
 * @brief Decode the captured read pulses into txn_ctx.raw
 * @note Reads ctx.pulse (written by the read DMA), never aliased with raw:
 *       the union invariant of decode_scratchpad() does not apply here.
 */
__STATIC_FORCEINLINE void txn_decode_read(void) {
    for (uint8_t byte = 0; byte < txn_ctx.read_bytes; byte++) {
        const uint8_t base = (uint8_t)(byte * DS18B20_BITS_PER_BYTE);
        uint8_t value = 0;
        for (uint8_t bit = 0; bit < DS18B20_BITS_PER_BYTE; bit++) {
            value |= (ctx.pulse[base + bit] <= SHORT_PULSE_MAX)
                         ? (uint8_t)(1u << bit)
                         : 0u;
        }
        txn_ctx.raw[byte] = value;
    }
}

/**
 * @brief Copy the decoded read result into the user buffer
 * @param[in] len Number of bytes to copy (txn_ctx.out must hold at least len)
 */
__STATIC_FORCEINLINE void txn_copy_out(uint8_t len) {
    for (uint8_t i = 0; i < len; i++) {
        txn_ctx.out[i] = txn_ctx.raw[i];
    }
}

/**
 * @brief Advance the active command transaction by one hardware operation
 * @return 1 when the transaction finished (successfully or aborted), 0 while
 *         running
 */
static uint8_t txn_poll(void) {
    if (txn_ctx.finished) {
        return 1;
    }

    if (txn_ctx.phase == DS18B20_TXN_DONE) {
        // The last hardware operation completed (command done or aborted):
        // hand the timer back to the measurement state machine exactly once.
        T1.EGR = TIM_EGR(UG);
        __DSB();
        txn_ctx.finished = 1;
        return 1;
    }

    // Wait for the currently scheduled hardware operation to complete.
    // This is a non-blocking poll, not a busy-wait.
    if (!onewire_bus_done()) {
        return 0;
    }

    switch (txn_ctx.phase) {
    case DS18B20_TXN_RESET:
        // Reset completed: a presence pulse means at least one device is on
        // the bus, so send the command for this transaction.
        if (!onewire_present(ctx.edge)) {
            txn_ctx.phase = DS18B20_TXN_DONE;
            break;
        }
        onewire_write_slots(txn_ctx.pulses, txn_ctx.slots);
        txn_ctx.phase = DS18B20_TXN_WRITE;
        break;

    case DS18B20_TXN_WRITE:
        // Command write completed: read the response back if the command has
        // one, otherwise wait the required hold-off or finish immediately.
        if (txn_ctx.read_bytes) {
            onewire_read_data(ctx.pulse, txn_ctx.read_bytes);
            txn_ctx.phase = DS18B20_TXN_READ;
        } else if (txn_ctx.wait_us) {
            onewire_start_timer(txn_ctx.wait_us, 0);
            txn_ctx.phase = DS18B20_TXN_WAIT;
        } else {
            txn_ctx.ok = 1;
            txn_ctx.phase = DS18B20_TXN_DONE;
        }
        break;

    case DS18B20_TXN_READ:
        // Data read completed: decode the captured pulse durations.
        txn_decode_read();
        txn_ctx.ok = 1;
        txn_ctx.phase = DS18B20_TXN_DONE;
        break;

    case DS18B20_TXN_WAIT:
        // Hold-off completed (Copy Scratchpad / Recall EEPROM).
        txn_ctx.ok = 1;
        txn_ctx.phase = DS18B20_TXN_DONE;
        break;

    case DS18B20_TXN_DONE:
    default:
        break;
    }

    return 0;
}

/**
 * @brief Schedule a new non-blocking command transaction
 * @param[in] command DS18B20 function command byte
 * @param[in] out User result buffer (may be NULL; only written on success)
 * @param[in] payload Up to 3 payload bytes (Write Scratchpad TH/TL/CFG)
 * @param[in] payload_len Number of payload bytes (0..3)
 * @param[in] read_bytes Bytes to read back after the command (0 = none)
 * @param[in] wait_us Timed hold-off after the command (0 = none)
 * @param[in] bare 1 to send the command without an addressing prefix
 * @note Ignored unless the driver is IDLE, the device search and any
 *       resolution change are finished, and no transaction is already
 *       running. The result buffer must stay valid until the transaction
 *       completes (ds18b20_*_poll() reports 1).
 */
static void txn_start(uint8_t command, uint8_t* out, const uint8_t* payload,
                      uint8_t payload_len, uint8_t read_bytes, uint16_t wait_us,
                      uint8_t bare) {
    if (!txn_can_start()) {
        return; // the timer belongs to someone else right now
    }
    txn_ctx.command = command;
    txn_ctx.out = out;
    txn_ctx.read_bytes = read_bytes;
    txn_ctx.wait_us = wait_us;
    txn_ctx.bare = bare;
    txn_ctx.payload_len = payload_len;
    for (uint8_t i = 0; i < payload_len && i < sizeof(txn_ctx.payload); i++) {
        txn_ctx.payload[i] = payload[i];
    }
    txn_ctx.ok = 0;
    txn_ctx.finished = 0;
    txn_build_pulses(); // Pre-build the command for the current address mode
    txn_ctx.phase = DS18B20_TXN_RESET;
    onewire_reset(ctx.edge); // Schedule the first hardware operation
}

/**
 * @brief Read the 64-bit ROM of the (only) DS18B20 on the bus
 * @param[in,out] rom Buffer for the 8-byte ROM (LSB first); written on success
 * @note Valid only when exactly one device is on the bus (datasheet Read ROM
 *       0x33). With several devices use the device search (ds18b20_search_*).
 * @note Result validity: check ds18b20_last_command_ok() or the CRC over the
 *       7 leading bytes (ds18b20_crc8(rom, 7) == rom[7]).
 */
void ds18b20_read_rom(uint8_t* rom) {
    txn_start(DS18B20_READ_ROM, rom, 0, 0, DS18B20_ROM_BYTES, 0, 1);
}

/**
 * @brief Advance the non-blocking Read ROM transaction
 * @return 1 when finished (successfully or aborted), 0 while running
 */
uint8_t ds18b20_read_rom_poll(void) {
    if (!txn_poll()) {
        return 0;
    }
    if (txn_ctx.ok && txn_ctx.out) {
        txn_copy_out(DS18B20_ROM_BYTES);
    }
    return 1;
}

/**
 * @brief Configure the alarm trigger thresholds TH and TL
 * @param[in] th High-alarm trigger value (DS18B20 8-bit threshold code)
 * @param[in] tl Low-alarm trigger value (DS18B20 8-bit threshold code)
 * @note Uses the DS18B20 8-bit sign-extended temperature code, the same
 *       encoding the scratchpad TH/TL bytes use; converting to/from Celsius is
 *       left to the application. The current conversion resolution (byte 4,
 *       R1/R0) is written unchanged, so the resolution is not disturbed.
 * @note Takes effect immediately in the scratchpad; run ds18b20_copy_scratchpad()
 *       afterwards to persist TH/TL/CFG to the EEPROM.
 */
void ds18b20_set_alarm_thresholds(uint8_t th, uint8_t tl) {
    const uint8_t payload[3] = {th, tl, res_config_byte(ctx.resolution)};
    txn_start(DS18B20_WRITE_SCRATCHPAD, 0, payload, 3, 0, 0, 0);
}

/**
 * @brief Advance the non-blocking alarm threshold write
 * @return 1 when finished (successfully or aborted), 0 while running
 */
uint8_t ds18b20_set_alarm_thresholds_poll(void) { return txn_poll(); }

/**
 * @brief Read the 9-byte scratchpad (raw; includes TH, TL and the CRC)
 * @param[in,out] buf Buffer for the 9 scratchpad bytes (byte 0 = temp LSB,
 *                    bytes 2/3 = TH/TL, byte 8 = CRC); written on success
 * @note Result validity: check ds18b20_last_command_ok() or the CRC over the
 *       8 leading bytes (buf[8] == ds18b20_crc8(buf, 8)).
 */
void ds18b20_read_scratchpad(uint8_t* buf) {
    txn_start(DS18B20_READ_SCRATCHPAD, buf, 0, 0, DS18B20_SCRATCHPAD_LEN, 0, 0);
}

/**
 * @brief Advance the non-blocking raw scratchpad read
 * @return 1 when finished (successfully or aborted), 0 while running
 * @note On a valid read (CRC byte matches) the conversion resolution is
 *       auto-derived from the config byte (byte 4), like the measurement path.
 */
uint8_t ds18b20_read_scratchpad_poll(void) {
    if (!txn_poll()) {
        return 0;
    }
    if (txn_ctx.ok && txn_ctx.out) {
        txn_copy_out(DS18B20_SCRATCHPAD_LEN);
        if (txn_ctx.raw[DS18B20_SCRATCHPAD_LEN - 1] ==
            ds18b20_crc8(txn_ctx.raw, DS18B20_CRC8_BYTES)) {
            ctx.resolution = DS18B20_RES_MIN + ((txn_ctx.raw[4] >> 5) & 0x3);
        }
    }
    return 1;
}

/**
 * @brief Copy the scratchpad into the EEPROM (non-volatile)
 * @note External-power wiring only (as assumed by the whole driver): the copy
 *       is powered by the sensor's VDD, no strong pull-up is needed. The
 *       driver waits the datasheet t_COPY hold-off (10ms) before finishing.
 */
void ds18b20_copy_scratchpad(void) {
    txn_start(DS18B20_COPY_SCRATCHPAD, 0, 0, 0, 0, DS18B20_EEPROM_WAIT_US, 0);
}

/**
 * @brief Advance the non-blocking Copy Scratchpad transaction
 * @return 1 when finished (successfully or aborted), 0 while running
 */
uint8_t ds18b20_copy_scratchpad_poll(void) { return txn_poll(); }

/**
 * @brief Recall the EEPROM contents into the scratchpad
 * @note Loads the last EEPROM copy (TH/TL/CFG) into the volatile scratchpad.
 *       The driver waits the datasheet t_RECALL hold-off (10ms) before
 *       finishing.
 */
void ds18b20_recall_eeprom(void) {
    txn_start(DS18B20_RECALL_EEPROM, 0, 0, 0, 0, DS18B20_EEPROM_WAIT_US, 0);
}

/**
 * @brief Advance the non-blocking Recall EEPROM transaction
 * @return 1 when finished (successfully or aborted), 0 while running
 */
uint8_t ds18b20_recall_eeprom_poll(void) { return txn_poll(); }

/**
 * @brief Read the power-supply state of the DS18B20
 * @param[in,out] is_parasite Receives 1 when the device is parasite-powered,
 *                            0 when externally powered; written on success
 */
void ds18b20_read_power_supply(uint8_t* is_parasite) {
    txn_start(DS18B20_READ_POWER_SUPPLY, is_parasite, 0, 0, 1, 0, 0);
}

/**
 * @brief Advance the non-blocking power-supply read
 * @return 1 when finished (successfully or aborted), 0 while running
 */
uint8_t ds18b20_read_power_supply_poll(void) {
    if (!txn_poll()) {
        return 0;
    }
    if (txn_ctx.ok && txn_ctx.out) {
        // The sensor drives one bit: 0 = parasite power, 1 = external power.
        *txn_ctx.out = (txn_ctx.raw[0] & 0x01) ? 0u : 1u;
    }
    return 1;
}

/**
 * @brief Result of the last completed command transaction
 * @return 1 when the last ds18b20_*_poll() finished a transaction that found
 *         a device present (and, for read commands, read its data back),
 *         0 when it aborted (e.g. no device present) or nothing ran yet
 */
uint8_t ds18b20_last_command_ok(void) { return txn_ctx.ok; }

/**
 * @}
 */

/**
 * @defgroup DS18B20_Public_Functions DS18B20 Public Functions
 * @{
 */

/**
 * @brief Initialize DS18B20 driver - configure clocks and peripherals
 * @note Initializes the shared 1-Wire layer (timer/DMA/GPIO) and marks the
 *       driver idle so the measurement state machine owns the timer until the
 *       application starts a device search.
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
 *       IDLE). Calls made while a cycle is running are ignored: applying them
 *       mid-cycle would overwrite ctx.addr_cmd while the DMA is still feeding
 *       it, corrupting the in-flight bus transaction. Re-call once the cycle
 *       finished (e.g., from ds18b20_complete(), which the driver invokes
 *       already in the IDLE state).
 */
void ds18b20_select(const uint8_t* rom) {
    /* Select is safe only when no bus transaction is in flight. In scan mode
     * the per-device ds18b20_complete() callback runs at DS18B20_ST_DECODE
     * (after the read, nothing pending), so a select() there is accepted too
     * and lets the application switch to single-device addressing from inside
     * the callback. Every other state means a transaction is mid-flight. */
    const uint8_t selectable =
        (uint8_t)(ctx.current_state == DS18B20_ST_IDLE ||
                  (ctx.current_state == DS18B20_ST_DECODE && ctx.scan_mode));
    if (!selectable) {
        // Mid-cycle call - reject to keep the in-flight transaction intact.
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
 * @brief Check presence and issue command (shared by CONVERT and REQUEST states)
 * @param[in] cmd_byte Command byte to send
 * @param[in] skip_tbl Skip-ROM command table (for broadcast mode)
 * @param[in] next_state State to transition to on success
 */
static void issue_command(uint8_t cmd_byte, const uint8_t* skip_tbl, ds18b20_state_t next_state) {
    if (!onewire_present(ctx.edge)) {
        // Return to IDLE before the callback so a re-selection from inside
        // ds18b20_complete() is accepted (ds18b20_select() only acts at IDLE).
        ctx.current_state = DS18B20_ST_IDLE;
        // Turn the busy indicator off: busy(1) was set in START and this early
        // exit skips the DECODE state where busy(0) is normally cleared.
        ds18b20_busy(0);
        ds18b20_complete(DS18B20_TEMP_ERROR_NO_SENSOR);
        start_cycle_pause();
        return;
    }
    if (ctx.address_mode) {
        build_addr_cmd(cmd_byte);
        onewire_write_slots(ctx.addr_cmd, DS18B20_MATCH_SLOTS);
    } else {
        onewire_write_slots(skip_tbl, DS18B20_DMA_TRANSFERS);
    }
    ctx.current_state = next_state;
}

/**
 * @brief Main state machine function - must be called periodically from main loop
 * @note Non-blocking state machine that advances 1-Wire communication state
 * @note Uses timer update interrupt flag to determine when operations complete
 */
void ds18b20_poll(void) {
    // Ownership guard: while the device search, a resolution change or a
    // command transaction owns the timer, the measurement state machine must
    // stay out of the way and not react to their UIFs.
    if (onewire_search_active() || !res_ctx.finished || !txn_ctx.finished) {
        return;
    }

    // Check if timer update interrupt occurred (indicates operation completion)
    // This is the non-blocking way to detect when timed operations finish
    if (!(T1.SR & TIM_SR(UIF))) return;
    // Clear timer update interrupt flag
    T1.SR = 0;

    // State machine to manage 1-Wire communication sequence
    switch (ctx.current_state) {
    case DS18B20_ST_IDLE:
        // Initialize union memory (fills with 0xFF pattern)
        ctx.fill_union = (uint64_t)-1;
        // Transition to START state
        ctx.current_state = DS18B20_ST_START;
        /* fallthrough to START state immediately */
        __attribute__((fallthrough));

    case DS18B20_ST_START:
        // Turn on LED to indicate measurement in progress
        ds18b20_busy(1);
        // Initiate 1-Wire bus reset sequence
        onewire_reset(ctx.edge);
        // Transition to CONVERT state
        ctx.current_state = DS18B20_ST_CONVERT;
        break;

    case DS18B20_ST_CONVERT:
        if (ctx.scan_mode) {
            // Scan mode: broadcast Convert T (Skip ROM) so every sensor starts
            // converting in parallel; a single conversion wait covers them all.
            ctx.scan_index = 0; // new round: read back starting from device 0
            ctx.address_mode = 0;
        }
        issue_command(DS18B20_CONVERT_T, conv_cmd, DS18B20_ST_WAIT);
        break;

    case DS18B20_ST_WAIT:
        // Start timer for conversion wait period (750ms typical)
        wait_conversion();
        ctx.current_state = DS18B20_ST_CONTINUE;
        break;

    case DS18B20_ST_CONTINUE:
        // Initiate second 1-Wire bus reset sequence
        onewire_reset(ctx.edge);
        ctx.current_state = DS18B20_ST_REQUEST;
        break;

    case DS18B20_ST_REQUEST:
        if (ctx.scan_mode) {
            // Scan mode: read the current device back via Match ROM.
            for (uint8_t i = 0; i < DS18B20_ROM_BYTES; i++) {
                ctx.selected_rom[i] = dev_roms[ctx.scan_index][i];
            }
            build_addr_prefix();
            ctx.address_mode = 1;
        }
        issue_command(DS18B20_READ_SCRATCHPAD, read_cmd, DS18B20_ST_READ);
        break;

    case DS18B20_ST_READ:
        // Initiate scratchpad data read using timer capture and DMA
        onewire_read_data(ctx.pulse, DS18B20_SCRATCHPAD_LEN);
        // Transition to DECODE state
        ctx.current_state = DS18B20_ST_DECODE;
        break;

    case DS18B20_ST_DECODE: // Process received data and report temperature
        // Decode captured pulse durations into scratchpad bytes
        decode_scratchpad();
        // Turn off LED to indicate measurement complete
        ds18b20_busy(0);

        // Return to IDLE before the callback so a re-selection from inside
        // ds18b20_complete() is accepted (ds18b20_select() only acts at IDLE).
        // Scan mode keeps its own per-device addressing, so it stays in DECODE
        // and reports every device before returning to IDLE at the round end.
        if (!ctx.scan_mode) {
            ctx.current_state = DS18B20_ST_IDLE;
        }

        // Match ROM mode: if the addressed device is absent, nobody drives
        // the bus after the address, so the whole scratchpad reads back as
        // 0xFF. Report it as a missing sensor instead of a bogus CRC error.
        if (ctx.address_mode) {
            uint8_t all_ones = 1;
            for (uint8_t i = 0; i < DS18B20_SCRATCHPAD_LEN; i++) {
                if (ctx.scratchpad[i] != 0xFF) {
                    all_ones = 0;
                    break;
                }
            }
            if (all_ones) {
                ds18b20_complete(DS18B20_TEMP_ERROR_NO_SENSOR);
                scan_finish_or_next();
                break;
            }
        }

        // Validate reserved bytes per DS18B20 specification:
        // Byte 5 must be 0xFF, Byte 7 must be 0x10.
        // This catches all-zero, all-0xFF, and bus fault conditions.
        if (ctx.scratchpad[5] != 0xFF || ctx.scratchpad[7] != 0x10) {
            ds18b20_complete(DS18B20_TEMP_ERROR_CRC_FAIL);
            scan_finish_or_next();
            break;
        }

        // Validate CRC and report temperature or error
        if (ctx.scratchpad[DS18B20_SCRATCHPAD_LEN - 1] == check_scratchpad_crc()) {
            // CRC valid - decode and report temperature. The scratchpad is
            // trustworthy, so also trust the config byte (byte 4, R1/R0 bits
            // 6:5) and adapt the conversion wait for the next cycle: this keeps
            // the wait in sync with a resolution changed via
            // ds18b20_set_resolution() or externally. (R1/R0 are 0..3, so the
            // derived value is always within DS18B20_RES_MIN..DS18B20_RES_MAX.)
            // It is derived only on a valid CRC so a corrupted config byte can
            // never shorten the next conversion wait prematurely.
            ctx.resolution = DS18B20_RES_MIN + ((ctx.scratchpad[4] >> 5) & 0x3);
            ds18b20_complete(decode_temperature());
        } else {
            // CRC invalid - report error (resolution kept unchanged)
            ds18b20_complete(DS18B20_TEMP_ERROR_CRC_FAIL);
        }

        // Next scan-mode device (CONTINUE, no fresh conversion) or, after the
        // last device, back to IDLE plus the inter-measurement pause. In
        // single-device mode this only starts the pause.
        scan_finish_or_next();
        break;

    default:
        // Unexpected state - report generic error
        ctx.current_state = DS18B20_ST_IDLE;
        ds18b20_complete(DS18B20_TEMP_ERROR_GENERIC);
        break;
    }
}

/**
 * @}
 */