/* DS2431 non-blocking operation engine (include-only part of src/ds2431.c).
 * Not a translation unit on its own: compile src/ds2431.c, which resolves the
 * DS2431_DRIVER_BUILD gate. This part implements the shared reset -> write ->
 * (read | wait) engine plus the four DS2431 commands (Read Memory 0xF0, Write
 * Scratchpad 0x0F, Read Scratchpad 0xAA, Copy Scratchpad 0x55) and the
 * parasite-power configuration. */
#ifndef DS2431_DRIVER_BUILD
#error "ds2431_txn.c is an include-only driver part; compile src/ds2431.c"
#endif

/* Copy Scratchpad completion pattern: after the t_PROG window the device
 * transmits an alternating 0/1 sequence until the master resets it, which a
 * single read byte samples as 0xAA. Any other value (the device drives 1s when
 * it refused the copy: PF set, invalid address or copy-protected page) means
 * the EEPROM block was not programmed. */
#define DS2431_COPY_STATUS_OK 0xAAu

/**
 * @defgroup DS2431_Operation_Impl DS2431 Non-Blocking Operations
 * @brief Shared non-blocking engine for every DS2431 operation. Each
 *        operation runs reset -> presence -> write -> (read | timed wait) ->
 *        done, one hardware-timed operation per poll call, so the same
 *        non-blocking discipline as the DS18B20 drivers is preserved. Each
 *        operation owns TIM1/DMA while it runs and leaves the bus idle when
 *        done.
 * @{
 */

/**
 * @brief Ownership guard shared by every operation start
 * @return 1 when a new operation may be scheduled
 */
static uint8_t ds2431_can_start(void) {
    return (uint8_t)((ctx.phase == DS2431_PH_IDLE || ctx.phase == DS2431_PH_DONE) &&
                     !onewire_search_active());
}

/**
 * @brief Build the command pulse sequence into ctx.u.cmd and return its length
 * @param[in] command DS2431 function command byte
 * @return Number of bit slots in the built table (incl. prefix and payload)
 * @note Encodes the addressing prefix (Skip ROM 0xCC, or Match ROM 0x55 +
 *       selected ROM), the function command byte, the two target-address
 *       bytes (all commands except the bare Read Scratchpad 0xAA) and the
 *       optional payload. The trailing ONEWIRE_RELEASE_PULSE that the 1-Wire
 *       layer consumes as the final DMA transfer into CCR3 is written at the
 *       slot index of the mode actually used.
 * @warning Must run only at the RESET -> CMD transition: the table shares
 *          storage with the reset/read capture union (see DS2431_ctx_t), and
 *          the reset DMA would have clobbered any earlier build.
 */
static uint8_t ds2431_build(uint8_t command) {
    ow_pulse_t* p = (ow_pulse_t*)ctx.u.cmd;
    uint8_t bytes = 0;
    if ((ctx.flags & DS2431_FLAG_ADDRESS_MODE) != 0u) {
        onewire_encode_byte(p, DS2431_MATCH_ROM);
        p += DS2431_BITS_PER_BYTE;
        bytes++;
        for (uint8_t i = 0; i < DS2431_ROM_BYTES; i++) {
            onewire_encode_byte(p, ctx.selected_rom[i]);
            p += DS2431_BITS_PER_BYTE;
            bytes++;
        }
    } else {
        onewire_encode_byte(p, DS2431_SKIP_ROM);
        p += DS2431_BITS_PER_BYTE;
        bytes++;
    }
    onewire_encode_byte(p, command);
    p += DS2431_BITS_PER_BYTE;
    bytes++;
    if (command != DS2431_READ_SCRATCHPAD) {
        onewire_encode_byte(p, ctx.addr_lo);
        p += DS2431_BITS_PER_BYTE;
        bytes++;
        onewire_encode_byte(p, ctx.addr_hi);
        p += DS2431_BITS_PER_BYTE;
        bytes++;
    }
    if (command == DS2431_COPY_SCRATCHPAD) {
        // Copy Scratchpad authorizes with the latched E/S byte. Encode it from
        // ctx.es so the last write payload stays intact for the read-back
        // verification (the scratchpad frame read compares against it).
        onewire_encode_byte(p, ctx.es);
        bytes++;
    } else {
        for (uint8_t i = 0; i < ctx.plen; i++) {
            onewire_encode_byte(p, ctx.payload[i]);
            p += DS2431_BITS_PER_BYTE;
            bytes++;
        }
    }
    const uint8_t slots = (uint8_t)(bytes * DS2431_BITS_PER_BYTE);
    /* B1: guarantee the trailing ONEWIRE_RELEASE_PULSE that the 1-Wire layer
     * reads as its final DMA transfer into CCR3, even though the command write
     * only ever fills slots 0 .. slots - 1. */
    ((ow_pulse_t*)ctx.u.cmd)[slots] = ONEWIRE_RELEASE_PULSE;
    return slots;
}

/**
 * @brief Schedule a new non-blocking DS2431 operation
 * @param[in] command DS2431 function command byte
 * @param[in] out User result buffer (may be NULL; only written on success)
 * @return 1 if the operation was scheduled, 0 if it was rejected (the driver
 *         is busy or a device search owns the timer)
 * @note Ignored unless the driver is parked (IDLE/DONE) and the device search
 *       is finished. The result buffer must stay valid until the operation
 *       completes (ds2431_*_poll() reports 1).
 */
static uint8_t ds2431_start(uint8_t command, uint8_t* out) {
    if (!ds2431_can_start()) {
        return 0; // the timer belongs to someone else right now
    }
    ctx.command = command;
    ctx.out = out;
    /* Fresh operation: clear the per-operation success/echo bits; the
     * address-mode and parasite flags and the pending write-cycle state
     * survive (they belong to the configuration / to the addressed device). */
    ctx.flags &= (uint8_t)~(DS2431_FLAG_OK | DS2431_FLAG_FINISHED | DS2431_FLAG_ECHO);
    onewire_strong_pullup(0);
    ctx.phase = DS2431_PH_RESET;
    onewire_reset((volatile uint16_t*)ctx.u.cap); // Schedule the first hardware operation
    return 1;
}

/**
 * @brief Data byte count of a Read Scratchpad reply, from its own header
 * @param[in] frame Decoded reply: TA1, TA2, E/S, data..
 * @return Number of scratchpad data bytes that follow E/S (1 .. 8), or 0 when
 *         the header is not a valid window
 * @note The DS2431 datasheet Read Scratchpad [AAh] returns (E[2:0] - T[2:0] + 1)
 *       data bytes, so the reply length depends on the latched end-offset and
 *       the low three address bits. The driver always clocks the maximum window
 *       (DS2431_SCRATCH_FRAME_CRC bytes) - the datasheet has the device drive
 *       logic 1 after the CRC - and derives the real length here.
 */
static uint8_t ds2431_scratchpad_data_len(const uint8_t* frame) {
    uint8_t last = (uint8_t)((frame[2] & 0x07u) + 1u); /* E[2:0] + 1, 1 .. 8 */
    uint8_t first = (uint8_t)(frame[0] & 0x07u); /* T[2:0],     0 .. 7 */
    if (last <= first) {
        return 0;
    }
    return (uint8_t)(last - first);
}

/**
 * @brief Verify the inverted CRC-16 the DS2431 appends to a Read Scratchpad reply
 * @param[in] frame Decoded reply: TA1, TA2, E/S, data.., CRC_lo, CRC_hi
 * @param[in] n Number of scratchpad data bytes in this reply (1 .. 8)
 * @return 1 when the trailing inverted CRC-16 matches the command + frame
 * @note CRC input is the command (0xAA) followed by the 3-byte header and the
 *       @p n data bytes the device actually transmitted (datasheet Read
 *       Scratchpad [AAh]); the device transmits the bitwise complement of the
 *       Maxim CRC-16, low byte first, right after the last data byte.
 */
static uint8_t ds2431_scratchpad_crc_ok(const uint8_t* frame, uint8_t n) {
    uint8_t crc_in[1 + 3 + DS2431_SCRATCH_SIZE];
    uint8_t total = (uint8_t)(1u + 3u + n);
    crc_in[0] = DS2431_READ_SCRATCHPAD;
    for (uint8_t i = 0; i < (uint8_t)(3u + n); i++) {
        crc_in[i + 1] = frame[i];
    }
    uint16_t expected = (uint16_t)(~onewire_crc16(crc_in, total));
    uint16_t got = (uint16_t)((uint16_t)frame[3u + n] |
                              ((uint16_t)frame[4u + n] << 8));
    return (uint8_t)(got == expected);
}

/**
 * @brief Advance the active DS2431 operation by one hardware operation
 * @return 1 when the operation finished (successfully or aborted), 0 while
 *         running
 */
static uint8_t ds2431_poll(void) {
    if ((ctx.flags & DS2431_FLAG_FINISHED) != 0u) {
        return 1;
    }

    if (ctx.phase == DS2431_PH_DONE) {
        // The last hardware operation completed (command done or aborted).
        // The timer is idle from here on: it is picked up again by the next
        // scheduled operation, which is what keeps an operation from silently
        // running into the next one.
        ctx.flags |= DS2431_FLAG_FINISHED;
        return 1;
    }

    // Wait for the currently scheduled hardware operation to complete.
    // This is a non-blocking poll, not a busy-wait.
    uint8_t op_done = onewire_bus_done();

#if OW_LOW_POWER
    // Sleep through a long stage of the operation (multi-slot command write,
    // scratchpad frame read, EEPROM programming hold-off) instead of spinning
    // on it - same WFE mechanism as the DS18B20 drivers.
    if (!op_done && onewire_long_wait_pending()) {
        onewire_sleep_until_done();
        op_done = onewire_bus_done(); /* consume the event that woke us */
    }
#endif

    if (!op_done) {
        return 0;
    }

    switch (ctx.phase) {
    case DS2431_PH_RESET:
        // Reset completed: a presence pulse means at least one device is on
        // the bus, so build and send the command for this operation.
        {
            uint8_t present = onewire_present((const volatile uint16_t*)ctx.u.cap, onewire_reset_captured());
            if (!present) {
                ctx.phase = DS2431_PH_DONE;
                break;
            }
        }
        if ((ctx.flags & DS2431_FLAG_PARASITE) != 0u) {
            onewire_strong_pullup(1);
        }
        // Build the command table in-place over the consumed reset capture;
        // it is safe here because the capture is no longer read (the build is
        // what makes union membership possible, see DS2431_ctx_t).
        onewire_write_slots((const ow_pulse_t*)ctx.u.cmd, ds2431_build(ctx.command));
        ctx.phase = DS2431_PH_CMD;
        break;

    case DS2431_PH_CMD:
        // Command write completed. Copy Scratchpad is timed (the status byte
        // is read after the t_PROG hold-off); the other commands either read
        // the answer back (Read Memory, Read Scratchpad) or finish immediately
        // (Write Scratchpad, whose outcome the echo read verifies later).
        if (ctx.command == DS2431_COPY_SCRATCHPAD) {
            // Parasite power: the EEPROM programming draws its supply from the
            // bus while the block is written - drive HIGH actively for the
            // t_PROG hold-off window.
            ow_glue_wait_start(DS2431_TPROG_US);
            ctx.phase = DS2431_PH_WAIT;
        } else if (ctx.len) {
            if ((ctx.flags & DS2431_FLAG_PARASITE) != 0u) {
                onewire_strong_pullup(0);
            }
            onewire_read_data((volatile uint8_t*)ctx.u.pulse, ctx.len);
            ctx.phase = DS2431_PH_READ;
        } else {
            if ((ctx.flags & DS2431_FLAG_PARASITE) != 0u) {
                onewire_strong_pullup(0);
            }
            ctx.flags |= DS2431_FLAG_OK;
            ctx.phase = DS2431_PH_DONE;
        }
        break;

    case DS2431_PH_READ:
        // Data read completed: decode the captured pulse durations.
        onewire_decode_pulses(ctx.raw, (const volatile uint8_t*)ctx.u.pulse, ctx.len);
        if (ctx.command == DS2431_COPY_SCRATCHPAD) {
            // The device reports the copy outcome on the bus after t_PROG:
            // 0xAA when the block was programmed, logic 1 when it refused
            // (PF set, invalid address or copy-protected page). Only the
            // completion pattern counts as success.
            if (ctx.raw[0] == DS2431_COPY_STATUS_OK) {
                ctx.flags |= DS2431_FLAG_OK;
            }
            ctx.phase = DS2431_PH_DONE;
            break;
        }
        if (ctx.command == DS2431_READ_SCRATCHPAD) {
            // The reply length depends on the latched E/S window; validate the
            // inverted CRC-16 over the bytes the device actually transmitted.
            uint8_t n = ds2431_scratchpad_data_len(ctx.raw);
            if (n == 0u || !ds2431_scratchpad_crc_ok(ctx.raw, n)) {
                // The reply's trailing inverted CRC-16 does not cover the
                // frame: the scratchpad read is corrupt. Finish without the
                // success flag.
                ctx.phase = DS2431_PH_DONE;
                break;
            }
            if ((ctx.flags & (DS2431_FLAG_ECHO | DS2431_FLAG_PENDING)) ==
                (DS2431_FLAG_ECHO | DS2431_FLAG_PENDING)) {
                if (ctx.raw[0] != ctx.addr_lo || ctx.raw[1] != ctx.addr_hi ||
                    (ctx.raw[2] & 0x07u) != ctx.es) {
                    // The scratchpad frame echoes neither the latched target
                    // address nor the expected end-offset status byte: the write
                    // did not reach this device. Finish without the success flag.
                    ctx.phase = DS2431_PH_DONE;
                    break;
                }
                if ((ctx.raw[2] & 0x20u) != 0u) {
                    // PF: the device received fewer bytes than needed to fill
                    // the scratchpad (or lost power), so the latched data is
                    // not valid and Copy Scratchpad would refuse it anyway.
                    ctx.phase = DS2431_PH_DONE;
                    break;
                }
                // The reply CRC-16 is computed by the device over what it stored,
                // so a byte corrupted on the way to the device passes it. Compare
                // the read-back data with the payload that was sent. Writes are
                // 8-byte aligned, so the payload starts at scratchpad byte 0 and
                // ES is the index of the last written byte.
                uint8_t i = 0;
                while (i < n && ctx.raw[3u + i] == ctx.payload[i]) {
                    i++;
                }
                if (i < n) {
                    // A latched byte differs from the payload: the write was
                    // corrupted in flight. Finish without the success flag so
                    // Copy Scratchpad is not authorized from this frame.
                    ctx.phase = DS2431_PH_DONE;
                    break;
                }
            }
        }
        ctx.flags |= DS2431_FLAG_OK;
        ctx.phase = DS2431_PH_DONE;
        break;

    case DS2431_PH_WAIT:
        // Hold-off completed (Copy Scratchpad): release the strong pull-up
        // unconditionally (idempotent) so a parasite flag cleared mid-window
        // cannot leave the bus actively driven, then sample the device's copy
        // completion pattern (ctx.len bytes, one for Copy Scratchpad).
        onewire_strong_pullup(0);
        onewire_read_data((volatile uint8_t*)ctx.u.pulse, ctx.len);
        ctx.phase = DS2431_PH_READ;
        break;

    case DS2431_PH_DONE:
    case DS2431_PH_IDLE:
        /* DONE/IDLE: not a running phase. DONE is handled by the finishing
         * branch above; IDLE only exists between init and the first operation
         * (its FINISHED bit is set from init, so the top guard returns 1). */
    default:
        break;
    }

    return 0;
}

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
    ds2431_start(DS2431_READ_MEMORY, dst);
}

/**
 * @brief Advance the non-blocking Read Memory operation
 * @return 1 when finished (successfully or aborted), 0 while running
 */
uint8_t ds2431_read_memory_poll(void) {
    if (!ds2431_poll()) {
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
    if (!ds2431_start(DS2431_WRITE_SCRATCHPAD, 0)) {
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
uint8_t ds2431_write_scratchpad_poll(void) { return ds2431_poll(); }

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
    ds2431_start(DS2431_READ_SCRATCHPAD, buf);
    ctx.flags |= DS2431_FLAG_ECHO;
}

/**
 * @brief Advance the non-blocking scratchpad frame read
 * @return 1 when finished (successfully or aborted), 0 while running
 */
uint8_t ds2431_read_scratchpad_poll(void) {
    if (!ds2431_poll()) {
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
    ds2431_start(DS2431_COPY_SCRATCHPAD, 0);
}

/**
 * @brief Advance the non-blocking Copy Scratchpad operation
 * @return 1 when finished (successfully or aborted), 0 while running
 */
uint8_t ds2431_copy_scratchpad_poll(void) { return ds2431_poll(); }

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