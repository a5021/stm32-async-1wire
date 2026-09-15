/* DS18B20 device-search module (include-only part of src/ds18b20.c).
 * Not a translation unit on its own: compile src/ds18b20.c, which
 * resolves the DS18B20_DRIVER_BUILD gate. This part owns dev_roms,
 * dev_count and search_user_sink, and implements the non-blocking device
 * search plus the scanned-device table accessors. */
#ifndef DS18B20_DRIVER_BUILD
#error "ds18b20_search.c is an include-only driver part; compile src/ds18b20.c"
#endif

/** @brief ROM table of the discovered devices (filled by the device search). */
static uint8_t dev_roms[DS18B20_MAX_DEVICES][DS18B20_ROM_BYTES];
/** @brief Number of devices currently stored in dev_roms. */
static uint8_t dev_count;

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
 * @note Ownership guards: the search, the measurement state machine, command
 *       transactions and resolution changes all share TIM1/DMA, so a new search
 *       may only be started while all of them are idle; a running search
 *       rejects a new start.
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
    if (!res_ctx.finished) {
        return; // a resolution change owns the timer
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
    if (!res_ctx.finished) {
        return; // a resolution change owns the timer
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
