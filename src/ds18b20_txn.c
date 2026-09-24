/* DS18B20 command-transaction module (include-only part of src/ds18b20.c).
 * Not a translation unit on its own: compile src/ds18b20.c, which
 * resolves the DS18B20_DRIVER_BUILD gate. This part owns txn_ctx (the
 * matching static lives in the core file) plus detect_buf, and implements
 * the non-blocking command transactions (Read ROM, Read/Write Scratchpad,
 * Copy/Recall EEPROM, alarm thresholds, parasite detection). */
#ifndef DS18B20_DRIVER_BUILD
#error "ds18b20_txn.c is an include-only driver part; compile src/ds18b20.c"
#endif

/** @brief Receive buffer for the parasite-mode detection answer byte */
static uint8_t detect_buf;

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
 *       The trailing ONEWIRE_RELEASE_PULSE that the 1-Wire layer consumes as
 *       the final DMA transfer (hardware bus release) is written at the slot
 *       index of the mode actually used, not always at the end of the buffer.
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
    /* B1: guarantee the trailing ONEWIRE_RELEASE_PULSE that the 1-Wire layer
     * reads as its final DMA transfer into CCR3, even though the command
     * write only ever fills slots 0 .. slots - 1 (see build_res_pulses for
     * the same pattern). */
    txn_ctx.pulses[txn_ctx.slots] = ONEWIRE_RELEASE_PULSE;
}

/**
 * @brief Decode the captured read pulses into txn_ctx.raw
 * @note Reads ctx.pulse (written by the read DMA), never aliased with raw:
 *       the union invariant of decode_scratchpad() does not apply here.
 */
__STATIC_FORCEINLINE void txn_decode_read(void) {
    onewire_decode_pulses(txn_ctx.raw, ctx.pulse, txn_ctx.read_bytes);
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
        ow_port_kick();
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
        if (!onewire_present(ctx.capture)) {
            txn_ctx.phase = DS18B20_TXN_DONE;
            break;
        }
        if (ctx.parasite) {
            onewire_strong_pullup(1);
        }
        onewire_write_slots(txn_ctx.pulses, txn_ctx.slots);
        txn_ctx.phase = DS18B20_TXN_WRITE;
        break;

    case DS18B20_TXN_WRITE:
        // Command write completed: read the response back if the command has
        // one, otherwise wait the required hold-off or finish immediately.
        if (txn_ctx.read_bytes) {
            if (ctx.parasite) {
                onewire_strong_pullup(0);
            }
            onewire_read_data(ctx.pulse, txn_ctx.read_bytes);
            txn_ctx.phase = DS18B20_TXN_READ;
        } else if (txn_ctx.wait_us) {
            // Parasite power: Copy Scratchpad / Recall E² draw their supply
            // from the bus while the EEPROM programs - drive HIGH actively
            // for the hold-off window.
            if (ctx.parasite) {
                onewire_strong_pullup(1);
            }
            onewire_start_timer(txn_ctx.wait_us, 0);
            txn_ctx.phase = DS18B20_TXN_WAIT;
        } else {
            if (ctx.parasite) {
                onewire_strong_pullup(0);
            }
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
        // Hold-off completed (Copy Scratchpad / Recall EEPROM): release the
        // strong pull-up unconditionally (idempotent) so a parasite flag
        // cleared mid-window cannot leave the bus actively driven.
        onewire_strong_pullup(0);
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
    onewire_strong_pullup(0);
    txn_ctx.phase = DS18B20_TXN_RESET;
    onewire_reset(ctx.capture); // Schedule the first hardware operation
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
 * @note The copy draws its supply from VDD on externally powered devices;
 *       parasite-powered devices are supplied by the strong pull-up, which
 *       the driver engages for the t_COPY hold-off window when
 *       ds18b20_set_parasite(1) is set. The driver waits the datasheet
 *       t_COPY hold-off (10ms) before finishing.
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
 * @note After recall, the scratchpad holds the EEPROM-stored configuration
 *       (TH/TL/CFG, including the conversion-resolution bits). The driver's
 *       tracked ctx.resolution is NOT updated by this call: Recall is a
 *       write-only command with no data returned. If the EEPROM resolution
 *       may differ from ctx.resolution, follow this with
 *       ds18b20_read_scratchpad() / ds18b20_read_scratchpad_poll() to
 *       resynchronise ctx.resolution before the next conversion.
 */
void ds18b20_recall_eeprom(void) {
    txn_start(DS18B20_RECALL_EEPROM, 0, 0, 0, 0, DS18B20_EEPROM_WAIT_US, 0);
}

/**
 * @brief Advance the non-blocking Recall EEPROM transaction
 * @return 1 when finished (successfully or aborted), 0 while running
 * @warning ctx.resolution is NOT updated on success. Recall is a write-only
 *          command; the device does not return the restored config. To keep
 *          ctx.resolution in sync with a possibly-different EEPROM resolution,
 *          call ds18b20_read_scratchpad_poll() after this returns 1 and
 *          ds18b20_last_command_ok() is set.
 */
uint8_t ds18b20_recall_eeprom_poll(void) {
    if (!txn_poll()) {
        return 0;
    }
    /* Nothing to decode: Recall returns no data, so there is no scratchpad
     * frame to parse here. The caller is responsible for resynchronising
     * ctx.resolution via ds18b20_read_scratchpad_poll() if needed. */
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
 * @brief Declare the bus as parasite-powered
 * @param[in] parasite 1 = devices are powered over the data line, 0 =
 *                     external VDD supply (default)
 * @note In parasite mode the driver engages the strong pull-up (bus pin
 *       switched to push-pull HIGH) during every temperature conversion wait
 *       and EEPROM programming hold-off, then releases the line again. The
 *       flag is read at the start of each window, so call this once after
 *       ds18b20_init() - or between measurement cycles - and it applies to
 *       all subsequent operations. The detection helper
 *       ds18b20_detect_parasite() reports the wiring and stores it back into
 *       this flag on success; this setter tells the driver how to behave.
 */
void ds18b20_set_parasite(uint8_t parasite) {
    ctx.parasite = parasite ? 1u : 0u;
}

/**
 * @brief Current parasite-power configuration of the driver
 * @return 1 when the strong pull-up will be engaged during conversion and
 *         EEPROM programming windows, 0 for external VDD supply
 */
uint8_t ds18b20_parasite_mode(void) { return ctx.parasite; }

/**
 * @brief Detect the bus wiring and configure parasite mode automatically
 * @note Issues a Read Power Supply command and stores the decoded answer in
 *       ctx.parasite on success (see ds18b20_detect_parasite_poll()).
 */
void ds18b20_detect_parasite(void) { txn_start(DS18B20_READ_POWER_SUPPLY, &detect_buf, 0, 0, 1, 0, 0); }

uint8_t ds18b20_detect_parasite_poll(void) {
    if (!txn_poll()) {
        return 0;
    }
    if (txn_ctx.ok) {
        // The sensor drives one bit: 0 = parasite power, 1 = external power.
        ctx.parasite = (txn_ctx.raw[0] & 0x01) ? 0u : 1u;
    }
    return 1;
}

/**
 * @}
 */
