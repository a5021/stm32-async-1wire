/* DS18B20 resolution-change module (include-only part of src/ds18b20.c).
 * Not a translation unit on its own: compile src/ds18b20.c, which
 * resolves the DS18B20_DRIVER_BUILD gate. This part owns res_ctx and the
 * resolution-change state machine (ds18b20_set_resolution{,_poll}). */
#ifndef DS18B20_DRIVER_BUILD
#error "ds18b20_resolution.c is an include-only driver part; compile src/ds18b20.c"
#endif

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
 *       feeds CCR3 from it asynchronously while the config write is sent.
 */
typedef struct {
    res_phase_t phase; /**< Current phase of the resolution state machine */
    uint8_t pending_res; /**< Resolution (bits) to apply */
    uint8_t applied; /**< 1 once the config write completed (resolution actually changed) */
    uint8_t finished; /**< 1 once the operation has completed (or aborted) */
    uint8_t slots; /**< Bit slots in the built config write (incl. prefix and payload) */
    ow_pulse_t pulses[DS18B20_RES_SLOTS_MAX + 1]; /**< Pulse buffer for the config write (+ trailing 0 for hardware bus release) */
} res_ctx_t;

/** @brief Global resolution context instance */
static res_ctx_t res_ctx;

/* B1 guard: the trailing zero-pulse consumed by the CCR3-feed DMA's final
 * transfer must always be present at the exact slot index used for the write
 * (see build_res_pulses); the buffer is sized for the longest (Match ROM) mode. */
_Static_assert(sizeof(res_ctx.pulses) >= DS18B20_RES_SLOTS_MAX + 1,
               "res_ctx.pulses must be DS18B20_RES_SLOTS_MAX + 1 to hold the "
               "trailing bus-release pulse consumed by the 1-Wire layer");

/**
 * @}
 */

/**
 * @brief Build the DS18B20 configuration register byte for a resolution
 * @param[in] res Resolution in bits (9..12)
 * @return Configuration register byte (R1/R0 bits set, rest at reset value)
 * @note 9 bit -> 0x1F, 10 bit -> 0x3F, 11 bit -> 0x5F, 12 bit -> 0x7F.
 */
__STATIC_FORCEINLINE uint8_t res_config_byte(uint8_t res) {
    return (uint8_t)(0x1Fu | ((uint8_t)(res - DS18B20_RES_MIN) << 5));
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
    ow_pulse_t* p = res_ctx.pulses;
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
    res_ctx.slots = use_match ? DS18B20_RES_SLOTS_MAX : DS18B20_RES_SLOTS_MIN;
    res_ctx.pulses[res_ctx.slots] = 0;
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
    onewire_reset(ctx.capture); // Schedule the first hardware operation
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
        ow_port_kick();
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
        if (!onewire_present(ctx.capture)) {
            res_ctx.phase = DS18B20_RES_DONE;
            break;
        }
        onewire_write_slots(res_ctx.pulses, res_ctx.slots);
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
