#include "ds2431.h"
#include "onewire.h"
/* The driver schedules the EEPROM hold-off window through the engine; that
 * bridge is library-internal. See src/ow_glue.h. */
#include "ow_glue.h"

/**
 * @defgroup DS2431_Private_Constants DS2431 Private Constants
 * @{
 */

/** @brief Longest DS2431 command write in bytes: Match ROM prefix (1 + 8) +
 *         function command + 2 target-address bytes + 8-byte payload. */
#define DS2431_CMD_MAX_BYTES ((1 + DS2431_ROM_BYTES) + 1 + 2 + DS2431_SCRATCH_SIZE)
/** @brief Bit slots for the longest command write (DS2431_CMD_MAX_BYTES x 8) */
#define DS2431_CMD_SLOTS (DS2431_CMD_MAX_BYTES * DS2431_BITS_PER_BYTE)
/** @brief Bit slots for the scratchpad frame read, CRC included */
#define DS2431_FRAME_SLOTS (DS2431_SCRATCH_FRAME_CRC * DS2431_BITS_PER_BYTE)

/**
 * @}
 */

#define DS2431_DRIVER_BUILD 1

/**
 * @defgroup DS2431_Private_Types DS2431 Private Types
 * @{
 */

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

/**
 * @}
 */

/**
 * @defgroup DS2431_Private_Variables DS2431 Private Variables
 * @{
 */

/** @brief Global driver context instance */
static DS2431_ctx_t ctx;

/* Phase packing guard: ds2431_phase_t values are stored in a uint8_t field. */
_Static_assert(DS2431_PH_IDLE <= 255, "phase enum must fit uint8_t packing");

/* RCR guards: every DS2431 pass must fit one 8-bit RCR window (256 slots) and
 * one read pass must fit the layer's 32-byte read limit. */
_Static_assert(DS2431_CMD_SLOTS <= ONEWIRE_MAX_SLOTS,
               "longest DS2431 command write must fit one RCR window");
_Static_assert(DS2431_FRAME_SLOTS <= ONEWIRE_MAX_SLOTS,
               "scratchpad frame read must fit one RCR window");
_Static_assert(DS2431_READ_PASS <= ONEWIRE_MAX_READ_BYTES,
               "a Read Memory pass must fit one read window");
_Static_assert(DS2431_SCRATCH_FRAME_CRC <= ONEWIRE_MAX_READ_BYTES,
               "a scratchpad frame read (incl. CRC) must fit one read window");
/* B1: the trailing ONEWIRE_RELEASE_PULSE consumed by the CCR3-feed DMA's final
 * transfer must always be present. Every build writes it at [slots], and slots
 * never exceeds DS2431_CMD_SLOTS, so [DS2431_CMD_SLOTS] is always in range. */

/**
 * @}
 */

/**
 * @defgroup DS2431_Public_Functions DS2431 Public Functions
 * @{
 */

/**
 * @brief Initialize DS2431 driver - configure clocks and peripherals
 * @pre Call exactly once after MCU reset, after the system clock is configured
 *      and before any 1-Wire bus operation or DS2431 driver activity.
 * @note Calls onewire_init(), which takes exclusive ownership of the shared
 *       TIM1/DMA/GPIO resources for the lifetime of the driver (until reset),
 *       and parks the driver at DS2431_PH_IDLE: nothing runs until the
 *       application schedules a Read Memory, a Write Scratchpad cycle or a
 *       Copy Scratchpad. The DS18B20 driver and the DS2431 driver share that
 *       bus ownership, so call ds2431_init() INSTEAD of ds18b20_init().
 * @warning This function is a bootstrap operation, not a reinitialization hook.
 *          A second call is unsupported and does not fully reset the timer,
 *          DMA, low-power or DS2431 software state.
 */
void ds2431_init(void) {
    onewire_init();
    /* No operation running after init; a poll without a scheduled op must
     * still report "finished" (the FINISHED flag is set like the DS18B20
     * transaction engine does). External power is the default wiring
     * assumption; parasite-powered setups opt in via ds2431_set_parasite().
     * The flag byte starts clear (address/parasite/ok/finished/pending/echo
     * all zero) before FINISHED is raised. */
    ctx.flags = DS2431_FLAG_FINISHED;
    ctx.phase = DS2431_PH_IDLE;
    ctx.command = 0;
    ctx.len = 0;
    ctx.plen = 0;
    ctx.es = 0;
}

/**
 * @brief Select which DS2431 device to address by its ROM address
 * @param[in] rom Pointer to the 8-byte ROM address (LSB first), or NULL to
 *                return to Skip ROM (broadcast) addressing
 * @note Accepted only while the driver is idle (nothing owns the timer);
 *       mid-operation calls are ignored. Changing the selection clears the
 *       pending write-cycle state (the latched TA/ES belongs to the addressed
 *       device), so a select() must not be interleaved into a running
 *       write_scratchpad -> read_scratchpad -> copy_scratchpad cycle.
 */
void ds2431_select(const uint8_t* rom) {
    if (ctx.phase != DS2431_PH_IDLE && ctx.phase != DS2431_PH_DONE) {
        return;
    }
    if (onewire_search_active()) {
        // The device search owns the timer - reject to keep its addressing.
        return;
    }
    if (rom == 0) {
        ctx.flags &= (uint8_t)~(DS2431_FLAG_ADDRESS_MODE | DS2431_FLAG_PENDING);
        return;
    }
    for (uint8_t i = 0; i < DS2431_ROM_BYTES; i++) {
        ctx.selected_rom[i] = rom[i];
    }
    /* The Match ROM table itself is rebuilt from selected_rom on every
     * operation (ds2431_build), so select only stores the ROM. */
    ctx.flags |= DS2431_FLAG_ADDRESS_MODE;
    ctx.flags &= (uint8_t)~DS2431_FLAG_PENDING;
}

/**
 * @}
 */

/* Include-only driver part: the non-blocking operation engine and the four
 * commands (Read Memory, Write/Read Scratchpad, Copy Scratchpad) plus the
 * parasite configuration live in their own part file and are compiled in here
 * so the whole driver stays one object file. The part references the file-
 * scope ctx static defined above (DS2431_DRIVER_BUILD gate).
 *
 * clang-format must not reorder this include. */
#include "ds2431_txn.c"