#include "ds2431.h"
#include "ds2431_internal.h"
#include "onewire.h"
/* The driver schedules the EEPROM hold-off window through the engine; that
 * bridge is library-internal. See src/ow_glue.h. */
#include "ow_glue.h"

/* Private sizing, phases, context layout and flags live in
 * src/ds2431_internal.h shared with the engine TU. */

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

/* Engine implementation lives in src/ds2431_txn.c and operates on the
 * core-owned ctx via src/ds2431_internal.h. */

/**
 * @brief Read up to DS2431_READ_PASS bytes of EEPROM
 * @param[in,out] dst Buffer for the read data; written on success
 * @param[in] addr Start address in EEPROM (0 .. DS2431_EEPROM_SIZE - 1)
 * @param[in] len Bytes to read, 1 .. DS2431_READ_PASS
 * @note Out-of-range requests are ignored.
 */
void ds2431_read_memory(uint8_t* dst, uint16_t addr, uint8_t len) {
    if (dst == 0) {
        return;
    }
    if (len < 1u || len > DS2431_READ_PASS || addr >= DS2431_EEPROM_SIZE ||
        (uint16_t)addr + len > DS2431_EEPROM_SIZE) {
        return;
    }
    ctx.addr_lo = (uint8_t)(addr & 0xffu);
    ctx.addr_hi = (uint8_t)(addr >> 8);
    ctx.len = len;
    ctx.plen = 0;
    ds2431_start(&ctx, DS2431_READ_MEMORY, dst);
}

/**
 * @brief Advance the non-blocking Read Memory operation
 * @return 1 when finished (successfully or aborted), 0 while running
 */
uint8_t ds2431_read_memory_poll(void) {
    if (!ds2431_poll(&ctx)) {
        return 0;
    }
    if (((ctx.flags & DS2431_FLAG_OK) != 0u) && ctx.out) {
        for (uint8_t i = 0; i < ctx.len; i++) {
            ctx.out[i] = ctx.raw[i];
        }
    }
    return 1;
}

/**
 * @brief Write up to 8 bytes into the scratchpad
 * @param[in] addr Block-aligned target address in EEPROM
 * @param[in] data Data to write to the scratchpad
 * @param[in] len Bytes to write, 1 .. DS2431_SCRATCH_SIZE
 * @return 1 if the write was scheduled, 0 if rejected (NULL data, len out of
 *         range, addr + len beyond the EEPROM, addr not 8-byte aligned, or
 *         the driver is busy / a search owns the timer)
 * @note A misaligned addr is rejected: the EEPROM programs in 8-byte blocks.
 */
uint8_t ds2431_write_scratchpad(uint16_t addr, const uint8_t* data, uint8_t len) {
    if (data == 0) {
        return 0;
    }
    if (len < 1u || len > DS2431_SCRATCH_SIZE || addr >= DS2431_EEPROM_SIZE ||
        (uint16_t)addr + len > DS2431_EEPROM_SIZE || (addr & 0x07u) != 0u) {
        return 0;
    }
    /* Schedule first: on a busy/search reject nothing is latched, so a
     * previous write cycle's pending TA/ES is never silently replaced by a
     * write that never ran. */
    if (!ds2431_start(&ctx, DS2431_WRITE_SCRATCHPAD, 0)) {
        return 0;
    }
    ctx.addr_lo = (uint8_t)(addr & 0xffu);
    ctx.addr_hi = (uint8_t)(addr >> 8);
    /* ES = last byte index of the block touched by the write, in the 8-byte
     * scratchpad window: the device echoes it in Read Scratchpad and requires
     * it in Copy Scratchpad. */
    ctx.es = (uint8_t)((addr + len - 1u) & 0x07u);
    ctx.len = 0; // no read phase
    ctx.plen = len;
    for (uint8_t i = 0; i < len; i++) {
        ctx.payload[i] = data[i];
    }
    ctx.flags |= DS2431_FLAG_PENDING;
    return 1;
}

/**
 * @brief Advance the non-blocking Write Scratchpad operation
 * @return 1 when finished (successfully or aborted), 0 while running
 */
uint8_t ds2431_write_scratchpad_poll(void) { return ds2431_poll(&ctx); }

/**
 * @brief Read the scratchpad frame (TA1, TA2, ES, data..8)
 * @param[in,out] buf Buffer for the frame; written on success
 * @note The driver always clocks the maximum wire reply
 *       (DS2431_SCRATCH_FRAME_CRC bytes, incl. the inverted CRC-16) and
 *       verifies the CRC over the (E[2:0] - T[2:0] + 1) bytes the device
 *       actually transmitted; the valid window is copied to @p buf and the
 *       remaining bytes up to DS2431_SCRATCH_FRAME are zeroed.
 */
void ds2431_read_scratchpad(uint8_t* buf) {
    if (buf == 0) {
        return;
    }
    ctx.len = DS2431_SCRATCH_FRAME_CRC;
    ctx.plen = 0;
    ds2431_start(&ctx, DS2431_READ_SCRATCHPAD, buf);
    ctx.flags |= DS2431_FLAG_ECHO;
}

/**
 * @brief Advance the non-blocking scratchpad frame read
 * @return 1 when finished (successfully or aborted), 0 while running
 */
uint8_t ds2431_read_scratchpad_poll(void) {
    if (!ds2431_poll(&ctx)) {
        return 0;
    }
    if (((ctx.flags & DS2431_FLAG_OK) != 0u) && ctx.out) {
        // The reply carried (E[2:0] - T[2:0] + 1) data bytes; hand the caller
        // exactly that window and zero the unused tail so the buffer is fully
        // defined regardless of the latched end-offset.
        uint8_t valid = (uint8_t)(3u + ds2431_scratchpad_data_len(ctx.raw));
        for (uint8_t i = 0; i < DS2431_SCRATCH_FRAME; i++) {
            ctx.out[i] = (uint8_t)(i < valid ? ctx.raw[i] : 0u);
        }
    }
    return 1;
}

/**
 * @brief Copy the scratchpad into the EEPROM
 * @note Programs the EEPROM block selected by the last Write Scratchpad
 *       (target address + ES) and verifies the device's completion pattern
 *       (0xAA after t_PROG); logic 1 means the device refused the copy (PF
 *       set, invalid address or copy-protected page) and reports failure.
 *       Ignored unless a Write Scratchpad completed since the last
 *       reset/select.
 */
void ds2431_copy_scratchpad(void) {
    if ((ctx.flags & DS2431_FLAG_PENDING) == 0u) {
        return;
    }
    ctx.len = 1; // read the copy completion pattern after the hold-off
    ctx.plen = 0; // ES comes from ctx.es; the write payload stays for the echo check
    ds2431_start(&ctx, DS2431_COPY_SCRATCHPAD, 0);
}

/**
 * @brief Advance the non-blocking Copy Scratchpad operation
 * @return 1 when finished (successfully or aborted), 0 while running
 */
uint8_t ds2431_copy_scratchpad_poll(void) { return ds2431_poll(&ctx); }

/**
 * @brief Declare the bus as parasite-powered
 * @param[in] parasite 1 = devices are powered over the data line, 0 =
 *                     external VDD supply (default)
 */
void ds2431_set_parasite(uint8_t parasite) {
    if (parasite != 0u) {
        ctx.flags |= DS2431_FLAG_PARASITE;
    } else {
        ctx.flags &= (uint8_t)~DS2431_FLAG_PARASITE;
    }
}

/**
 * @brief Current parasite-power configuration of the driver
 * @return 1 when the strong pull-up will be engaged during EEPROM programming
 */
uint8_t ds2431_parasite_mode(void) { return (uint8_t)((ctx.flags & DS2431_FLAG_PARASITE) != 0u); }

/**
 * @brief Result of the last completed DS2431 operation
 * @return 1 when the last ds2431_*_poll() finished an operation that found a
 *         device present (and, for reads, verified its data - including the
 *         variable-length scratchpad reply and the Write Scratchpad payload
 *         read back from the scratchpad - and, for Copy Scratchpad, verified
 *         the device's completion pattern), 0 when it aborted or nothing ran
 *         yet
 */
uint8_t ds2431_last_command_ok(void) { return (uint8_t)((ctx.flags & DS2431_FLAG_OK) != 0u); }

/**
 * @}
 */