/* DS18B20 measurement module (include-only part of src/ds18b20.c).
 * Not a translation unit on its own: compile src/ds18b20.c, which
 * resolves the DS18B20_DRIVER_BUILD gate. This part owns conv_cmd,
 * read_cmd and the DS18B20_ST_* measurement state machine (ds18b20_poll). */
#ifndef DS18B20_DRIVER_BUILD
#error "ds18b20_measure.c is an include-only driver part; compile src/ds18b20.c"
#endif

/**
 * @defgroup DS18B20_Measurement_Internal DS18B20 Measurement State Machine
 * @{
 */

/**
 * @brief Finish the current scan-mode device read
 * @note Called after every per-device report in scan mode. Advances to the
 *       next device (CONTINUE, skipping a fresh conversion) or, after the last
 *       device, returns to IDLE. In single-device mode it only returns to IDLE.
 * @note The driver never starts a round on its own: at IDLE it waits for
 *       ds18b20_start_measure(), so the measurement cadence is the
 *       application's decision.
 */
static void scan_finish_or_next(void) {
    if (!ctx.scan_mode) {
        // Parasite power: hold the strong pull-up while the driver is parked
        // so the device capacitors stay charged for the next measurement
        // cycle. ds18b20_start_measure() releases it again in START.
        if (ctx.parasite) {
            onewire_strong_pullup(1);
        }
        return;
    }
    ctx.scan_index++;
    if (ctx.scan_index < dev_count) {
        ctx.current_state = DS18B20_ST_CONTINUE;
        /* DECODE armed nothing, so without a running timer no UIF would ever
         * drive the CONTINUE state again (single-device rounds get their UIF
         * from ds18b20_start_measure()). Arm a short scheduling delay: its UIF
         * is the bridge to CONTINUE, which then arms the real bus reset. */
        onewire_start_timer(SCAN_DEVICE_GAP_US, SCAN_DEVICE_GAP_RCR);
    } else {
        ctx.current_state = DS18B20_ST_IDLE;
        // Parasite power: keep the strong pull-up engaged while parked.
        if (ctx.parasite) {
            onewire_strong_pullup(1);
        }
    }
}

/**
 * @brief Start one measurement cycle (non-blocking)
 * @see ds18b20_start_measure() in ds18b20.h
 * @note Schedules the bus reset the state machine needs to leave IDLE; the
 *       cycle itself is then advanced by ds18b20_poll(). Ignored while a
 *       measurement cycle, a device search, a resolution change or a command
 *       transaction owns the timer.
 */
void ds18b20_start_measure(void) {
    if (ctx.current_state != DS18B20_ST_IDLE) {
        return; // a measurement cycle is in progress
    }
    if (onewire_search_active() || !res_ctx.finished || !txn_ctx.finished) {
        return; // the search, a resolution change or a command owns the timer
    }
    onewire_kick();
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
 * @brief Index of the device whose result ds18b20_complete() just reported
 * @see ds18b20_scan_index() in ds18b20.h
 */
uint8_t ds18b20_scan_index(void) { return ctx.scan_index; }

static ow_pulse_t conv_cmd[DS18B20_DMA_TRANSFERS + 1];
static ow_pulse_t read_cmd[DS18B20_DMA_TRANSFERS + 1];

/* B1 guard: same trailing bus-release invariant as addr_cmd/txn_ctx/res_ctx —
 * the 1-Wire layer's CCR3-feed DMA reads cmd[DS18B20_DMA_TRANSFERS] as the
 * final ONEWIRE_RELEASE_PULSE. Keep both Skip-ROM buffers at +1 for uniformity. */
_Static_assert(sizeof(conv_cmd) >= DS18B20_DMA_TRANSFERS + 1,
               "conv_cmd must be DS18B20_DMA_TRANSFERS + 1 to hold the trailing "
               "bus-release pulse consumed by the 1-Wire layer");
_Static_assert(sizeof(read_cmd) >= DS18B20_DMA_TRANSFERS + 1,
               "read_cmd must be DS18B20_DMA_TRANSFERS + 1 to hold the trailing "
               "bus-release pulse consumed by the 1-Wire layer");

/* Lifetime note: conv_cmd/read_cmd are shared static buffers reused on every
 * build_skip_cmd() call. This is safe only because issue_command() is invoked
 * exclusively from the CONVERT/REQUEST states after onewire_bus_done() has
 * confirmed that the timer/DMA of the previous 1-Wire operation is idle, and
 * the ownership guards (ds18b20_select/search/resolution reject while busy)
 * prevent any concurrent re-entry that could interleave a second build while
 * the CCR3-feed DMA is still reading the table. In other words the rewrite
 * happens strictly between DMA bursts, never during one — the invariant is
 * implicit in the call site, hence documented here at the same level of
 * detail as the B1 guards for the other pulse buffers. */
static void build_skip_cmd(ow_pulse_t* dst, uint8_t cmd_byte) {
    onewire_encode_byte(dst, 0xCC);
    onewire_encode_byte(dst + 8, cmd_byte);
    dst[DS18B20_DMA_TRANSFERS] = ONEWIRE_RELEASE_PULSE;
}

/**
 * @brief Check presence and issue a DS18B20 command (shared by CONVERT and
 *        REQUEST states)
 * @param[in] cmd_byte Command byte to send (after the recycled Match ROM
 *                     prefix in address mode, or via the Skip-ROM table in
 *                     broadcast mode)
 * @param[in] next_state State to transition to on success
 */
static void issue_command(uint8_t cmd_byte, ds18b20_state_t next_state) {
    if (!onewire_present(ctx.capture)) {
        // Return to IDLE before the callback so a re-selection from inside
        // ds18b20_complete() is accepted (ds18b20_select() only acts at IDLE).
        ctx.current_state = DS18B20_ST_IDLE;
        // Turn the busy indicator off: busy(1) was set in START and this early
        // exit skips the DECODE state where busy(0) is normally cleared.
        ds18b20_busy(0);
        ds18b20_complete(DS18B20_TEMP_ERROR_NO_SENSOR);
        // Parasite power: hold the strong pull-up while parked.
        if (ctx.parasite) {
            onewire_strong_pullup(1);
        }
        return;
    }
    if (ctx.address_mode) {
        build_addr_cmd(cmd_byte);
        onewire_write_slots(ctx.addr_cmd, DS18B20_MATCH_SLOTS);
    } else {
        ow_pulse_t* skip_tbl = (cmd_byte == DS18B20_CONVERT_T) ? conv_cmd : read_cmd;
        build_skip_cmd(skip_tbl, cmd_byte);
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
    // This is the non-blocking way to detect when timed operations finish.
    // ow_port_bus_done() *consumes* the update flag, so it must be called
    // exactly once per poll and its result reused below.
    uint8_t done = ow_port_bus_done();

#if OW_PORT_LOW_POWER
    // Sleep through a long stage of the measurement cycle (temperature
    // conversion, scratchpad read) instead of spinning on it. The wake-up is
    // the timer's own update event via SEVONPEND - no ISR, no NVIC interrupt
    // - so this is invisible to the application: it just calls ds18b20_poll()
    // and the call returns when the stage is done.
    if (!done && ow_port_long_wait_pending()) {
        ow_port_sleep_until_done();
        done = ow_port_bus_done(); /* consume the event that woke us */
    }
#endif

    if (!done) return;

    // State machine to manage 1-Wire communication sequence
    switch (ctx.current_state) {
    case DS18B20_ST_IDLE:
        // A parked driver advances only on the UIF raised by
        // ds18b20_start_measure(); without such a request the bus stays idle.
        ctx.current_state = DS18B20_ST_START;
        /* fallthrough to START state immediately */
        __attribute__((fallthrough));

    case DS18B20_ST_START:
        // Turn on LED to indicate measurement in progress
        ds18b20_busy(1);
        // Parasite power: release the strong pull-up so the reset pulse can
        // drive the line LOW; it is re-engaged for the conversion window.
        if (ctx.parasite) {
            onewire_strong_pullup(0);
        }
        // Initiate 1-Wire bus reset sequence
        onewire_reset(ctx.capture);
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
        // Parasite power: the Convert T command is master-only (the slave does
        // not pull the line LOW during it), so keep the strong pull-up engaged
        // while the command is transmitted. This feeds the slave through the
        // command phase; the conversion window below re-asserts it anyway.
        if (ctx.parasite) {
            onewire_strong_pullup(1);
        }
        issue_command(DS18B20_CONVERT_T, DS18B20_ST_WAIT);
        break;

    case DS18B20_ST_WAIT:
        // Parasite power: the sensors draw their supply from the bus line
        // during the whole conversion, so drive the line HIGH actively before
        // the wait starts (engaging here and starting the timer in the same
        // transition keeps wait and supply aligned regardless of poll latency).
        if (ctx.parasite) {
            onewire_strong_pullup(1);
        }
        // Start timer for the conversion wait (93.75ms @ 9-bit .. 750ms @ 12-bit)
        wait_conversion();
        ctx.current_state = DS18B20_ST_CONTINUE;
        break;

    case DS18B20_ST_CONTINUE:
        // Release the strong pull-up BEFORE the reset pulse pulls the line
        // low: the conversion is complete, the devices no longer need the
        // parasite supply and the bus must be free again.
        onewire_strong_pullup(0);
        // Initiate second 1-Wire bus reset sequence
        onewire_reset(ctx.capture);
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
        if (ctx.parasite) {
            onewire_strong_pullup(1);
        }
        issue_command(DS18B20_READ_SCRATCHPAD, DS18B20_ST_READ);
        break;

    case DS18B20_ST_READ:
        if (ctx.parasite) {
            onewire_strong_pullup(0);
        }
        onewire_read_data(ctx.pulse, DS18B20_SCRATCHPAD_LEN);
        ctx.current_state = DS18B20_ST_DECODE;
        break;

    case DS18B20_ST_DECODE: // Process received data and report temperature
        /* Snapshot pulse widths before decode_scratchpad() overwrites them
         * via the union alias (scratchpad[n] == pulse[n]). */
        ow_stats_capture_pulse(ctx.pulse, DS18B20_SCRATCHPAD_BITS,
                               ctx.address_mode ? ctx.selected_rom : (const uint8_t*)0);
        // Decode captured pulse durations into scratchpad bytes
        decode_scratchpad();
        // Turn off LED to indicate measurement complete
        ds18b20_busy(0);

        // In single-device mode the callback runs at IDLE, so a re-selection
        // from inside ds18b20_complete() is accepted there. Scan mode keeps its
        // own per-device addressing and stays in DECODE: a select() from the
        // scan callback is rejected, and it reports every device before
        // returning to IDLE at the round end.
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
                ow_stats_count_error(DS18B20_TEMP_ERROR_NO_SENSOR,
                                     ctx.selected_rom);
                scan_finish_or_next();
                break;
            }
        }

        // Validate reserved bytes per DS18B20 specification:
        // Byte 5 must be 0xFF, Byte 7 must be 0x10.
        // This catches all-zero, all-0xFF, and bus fault conditions.
        if (ctx.scratchpad[5] != 0xFF || ctx.scratchpad[7] != 0x10) {
            ds18b20_complete(DS18B20_TEMP_ERROR_CRC_FAIL);
            ow_stats_count_error(DS18B20_TEMP_ERROR_CRC_FAIL,
                                 ctx.selected_rom);
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
            ow_stats_count_error(DS18B20_TEMP_ERROR_CRC_FAIL,
                                 ctx.selected_rom);
        }

        // Next scan-mode device (CONTINUE, no fresh conversion) or, after the
        // last device, back to IDLE. In single-device mode this only returns to
        // IDLE, where the driver waits for ds18b20_start_measure().
        scan_finish_or_next();
        break;

    default:
        // Unexpected state - report generic error
        ctx.current_state = DS18B20_ST_IDLE;
        ds18b20_complete(DS18B20_TEMP_ERROR_GENERIC);
        ow_stats_count_error(DS18B20_TEMP_ERROR_GENERIC,
                             (const uint8_t*)0);
        break;
    }
}

/**
 * @}
 */
