/**
 * @file demo4.c
 * @brief Command demo: ROM, power supply, TH/TL and the EEPROM (v1.3.0 API)
 *
 * Runs the non-blocking device search to discover every sensor, selects the
 * first device (Match ROM), then drives the non-blocking command transactions
 * one by one with the same poll discipline as the measurement path:
 * Read Power Supply (0xB4), raw Read Scratchpad (0xBE), Write Scratchpad
 * TH/TL (0x4E), Copy Scratchpad (0x48) to the EEPROM, Recall EEPROM (0xB8)
 * and the single-device Read ROM (0x33). Every command owns TIM1/DMA while it
 * runs and hands the timer back to the measurement state machine, which then
 * reports the selected sensor's temperature. All low-level bus operations live
 * in the shared 1-Wire layer (onewire.h/onewire.c), and the command engine
 * builds on it; this example only uses the public high-level interface.
 */

#include "app.h"
#include "ds18b20.h"

// ======== Config: maximum devices reported by the startup bus scan ========
#ifndef DS18B20_SEARCH_MAX_DEVICES
#define DS18B20_SEARCH_MAX_DEVICES 8u
#endif

#define SCRATCHPAD_BYTES 9u // TH/TL/CFG plus temperature bytes and the CRC

// Alarm thresholds in the DS18B20 8-bit sign-extended temperature code:
// 0x19 = +25C, 0x0F = +15C, 0x05 = +5C, 0x02 = +2C
#define TH_HOT 0x19
#define TL_HOT 0x0F
#define TH_MILD 0x05
#define TL_MILD 0x02

// Ordered command sequence; STEP_MEASURE is the steady-state terminal phase.
typedef enum {
    STEP_POWER = 0,
    STEP_SCRATCH_BASELINE,
    STEP_SET_ALARM,
    STEP_SCRATCH_AFTER_SET,
    STEP_COPY,
    STEP_SET_ALARM_AGAIN,
    STEP_SCRATCH_AFTER_SECOND,
    STEP_RECALL,
    STEP_SCRATCH_AFTER_RECALL,
    STEP_READ_ROM,
    STEP_MEASURE
} step_t;

static step_t step = STEP_MEASURE;
static uint8_t search_running = 1; // 1 until the non-blocking bus scan finishes
static uint8_t cmd_running = 0; // 1 while a command transaction is in flight
static uint8_t is_parasite = 0; // Power Supply result buffer (valid until done)
static uint8_t scratchpad[SCRATCHPAD_BYTES]; // Scratchpad result buffer
static uint8_t rom[DS18B20_ROM_BYTES]; // Read ROM result buffer

/**
 * @brief Device search callback - prints the ROM in hex
 * @param[in] found_rom Pointer to the 8-byte ROM address (LSB first)
 * @return 0 to continue the search
 */
static uint8_t device_found_sink(const uint8_t* found_rom) {
    uart_write_str("  ROM: ");
    for (uint8_t i = 0; i < DS18B20_ROM_BYTES; i++) {
        uart_write_hex(found_rom[i]);
        if (i != DS18B20_ROM_BYTES - 1) uart_tx_enqueue_byte(' ');
    }
    uart_write_str("\r\n");
    return 0;
}

/**
 * @brief Print the scratchpad result: bytes, CRC verdict, TH/TL and resolution
 * @param[in] label Line prefix printed before the raw bytes
 */
static void print_scratchpad(const char* label) {
    uart_write_str(label);
    for (uint8_t i = 0; i < SCRATCHPAD_BYTES; i++) {
        uart_write_hex(scratchpad[i]);
        if (i != SCRATCHPAD_BYTES - 1) uart_tx_enqueue_byte(' ');
    }
    uart_write_str(scratchpad[8] == ds18b20_crc8(scratchpad, 8) ? "  CRC ok"
                                                                : "  CRC fail");
    uart_write_str("  TH=0x");
    uart_write_hex(scratchpad[2]);
    uart_write_str(" TL=0x");
    uart_write_hex(scratchpad[3]);
    uart_write_str("  res=");
    uart_write_int(ds18b20_get_resolution());
    uart_write_str("bit\r\n");
}

/**
 * @brief Start the command transaction for a step (prints its banner first)
 * @param[in] s Step to start
 */
static void start_step(step_t s) {
    switch (s) {
    case STEP_POWER:
        uart_write_str("Read Power Supply (0xB4):\r\n");
        ds18b20_read_power_supply(&is_parasite);
        break;
    case STEP_SCRATCH_BASELINE:
        uart_write_str("Read Scratchpad (0xBE) - baseline:\r\n");
        ds18b20_read_scratchpad(scratchpad);
        break;
    case STEP_SET_ALARM:
        uart_write_str("Write Scratchpad (0x4E): TH=0x");
        uart_write_hex(TH_HOT);
        uart_write_str(" TL=0x");
        uart_write_hex(TL_HOT);
        uart_write_str("\r\n");
        ds18b20_set_alarm_thresholds(TH_HOT, TL_HOT);
        break;
    case STEP_SCRATCH_AFTER_SET:
        uart_write_str("Scratchpad after threshold write:\r\n");
        ds18b20_read_scratchpad(scratchpad);
        break;
    case STEP_COPY:
        uart_write_str("Copy Scratchpad (0x48) -> EEPROM (10ms hold-off):\r\n");
        ds18b20_copy_scratchpad();
        break;
    case STEP_SET_ALARM_AGAIN:
        uart_write_str("Write Scratchpad (0x4E): TH=0x");
        uart_write_hex(TH_MILD);
        uart_write_str(" TL=0x");
        uart_write_hex(TL_MILD);
        uart_write_str(" (volatile only)\r\n");
        ds18b20_set_alarm_thresholds(TH_MILD, TL_MILD);
        break;
    case STEP_SCRATCH_AFTER_SECOND:
        uart_write_str("Scratchpad after second write:\r\n");
        ds18b20_read_scratchpad(scratchpad);
        break;
    case STEP_RECALL:
        uart_write_str("Recall EEPROM (0xB8) -> scratchpad (10ms hold-off):\r\n");
        ds18b20_recall_eeprom();
        break;
    case STEP_SCRATCH_AFTER_RECALL:
        uart_write_str("Scratchpad after Recall (EEPROM copy restored):\r\n");
        ds18b20_read_scratchpad(scratchpad);
        break;
    case STEP_READ_ROM:
        uart_write_str("Read ROM (0x33) - single-device command:\r\n");
        ds18b20_read_rom(rom);
        break;
    case STEP_MEASURE:
        break;
    }
}

/**
 * @brief Advance the active command transaction for a step by one operation
 * @param[in] s Active step
 * @return 1 when the transaction finished, 0 while still running
 */
static uint8_t poll_step(step_t s) {
    switch (s) {
    case STEP_POWER:
        return ds18b20_read_power_supply_poll();
    case STEP_SCRATCH_BASELINE:
    case STEP_SCRATCH_AFTER_SET:
    case STEP_SCRATCH_AFTER_SECOND:
    case STEP_SCRATCH_AFTER_RECALL:
        return ds18b20_read_scratchpad_poll();
    case STEP_SET_ALARM:
    case STEP_SET_ALARM_AGAIN:
        return ds18b20_set_alarm_thresholds_poll();
    case STEP_COPY:
        return ds18b20_copy_scratchpad_poll();
    case STEP_RECALL:
        return ds18b20_recall_eeprom_poll();
    case STEP_READ_ROM:
        return ds18b20_read_rom_poll();
    case STEP_MEASURE:
        return 1;
    }
    return 0;
}

/**
 * @brief Print the result of a finished command step
 * @param[in] s Finished step
 */
static void finish_step(step_t s) {
    if (!ds18b20_last_command_ok()) {
        uart_write_str("  no device present; command aborted\r\n");
        return;
    }
    switch (s) {
    case STEP_POWER:
        uart_write_str("  power supply: ");
        uart_write_str(is_parasite ? "parasite" : "external (VDD)");
        uart_write_str("\r\n");
        break;
    case STEP_SCRATCH_BASELINE:
    case STEP_SCRATCH_AFTER_SET:
    case STEP_SCRATCH_AFTER_SECOND:
    case STEP_SCRATCH_AFTER_RECALL:
        print_scratchpad("  ");
        break;
    case STEP_SET_ALARM:
        break;
    case STEP_COPY:
        uart_write_str("  copied to EEPROM\r\n");
        break;
    case STEP_SET_ALARM_AGAIN:
        break;
    case STEP_RECALL:
        uart_write_str("  EEPROM copy loaded back\r\n");
        break;
    case STEP_READ_ROM:
        uart_write_str("  ROM: ");
        for (uint8_t i = 0; i < DS18B20_ROM_BYTES; i++) {
            uart_write_hex(rom[i]);
            if (i != DS18B20_ROM_BYTES - 1) uart_tx_enqueue_byte(' ');
        }
        uart_write_str(rom[7] == ds18b20_crc8(rom, 7)
                           ? "  CRC ok"
                           : "  CRC fail (valid only with one device on the bus)");
        uart_write_str("\r\n");
        break;
    case STEP_MEASURE:
        break;
    }
}

/**
 * @brief Weak implementation for DS18B20 measurement completion callback
 * @param[in] temp Temperature value in tenths of degrees Celsius, or error code
 * @note The selected device is measured, so one result arrives per cycle.
 */
void ds18b20_complete(int16_t temp) {
    if (temp == DS18B20_TEMP_ERROR_NO_SENSOR) {
        uart_write_str("no sensor detected.\r\n");
    } else if (temp == DS18B20_TEMP_ERROR_CRC_FAIL) {
        uart_write_str("CRC check failed.\r\n");
    } else if (temp == DS18B20_TEMP_ERROR_GENERIC) {
        uart_write_str("generic failure.\r\n");
    } else {
        int whole = temp / 10; // Get whole degrees (temp is in tenths)
        int frac = temp % 10; // Get fractional part (tenths)
        if (frac < 0) frac = -frac; // Ensure fractional part is positive
        if (whole == 0 && temp < 0) {
            uart_write_str("-0"); // Handle -0.5C case
        } else {
            uart_write_int(whole); // Display whole part
        }
        uart_write_str("."); // Decimal point
        uart_write_int(frac); // Display fractional part
        uart_write_str(" C\r\n"); // Units
    }
}

/**
 * @brief Main application entry point
 * @note Fully non-blocking: the search, every command transaction and the
 *       measurement advance by one hardware operation per poll call from the
 *       main loop.
 */
int main(void) {
    app_init(); // System clock, UART and LED GPIO - single setup call

    uart_write_str("DS18B20 demo4 starting...\r\n");
    uart_write_str("Searching 1-Wire bus...\r\n");
    ds18b20_init(); // Initialize DS18B20 driver (non-blocking)
    ds18b20_search_start(device_found_sink, DS18B20_SEARCH_MAX_DEVICES);

    for (;;) { // Main event loop (non-blocking, cooperative multitasking)
        if (search_running) {
            // Advance the non-blocking device search by one hardware operation
            if (ds18b20_search_poll()) {
                search_running = 0;
                uart_write_str("Found ");
                uart_write_int(ds18b20_device_count());
                uart_write_str(" device(s).\r\n");
                if (ds18b20_device_count() > 0) {
                    // Address the first sensor and run the command sequence
                    ds18b20_select(ds18b20_device_rom(0));
                    step = STEP_POWER;
                    start_step(step);
                    cmd_running = 1;
                } else {
                    step = STEP_MEASURE;
                }
            }
        } else if (step == STEP_MEASURE) {
            ds18b20_poll(); // Steady state: measure the selected sensor
        } else if (!cmd_running) {
            start_step(step); // Launch the next command transaction
            cmd_running = 1;
        } else if (poll_step(step)) {
            cmd_running = 0;
            finish_step(step);
            step = (step_t)(step + 1);
            if (step == STEP_MEASURE) {
                uart_write_str("Command demo done. Measuring selected device:\r\n");
            }
        }
        uart_poll_tx(); // Poll UART transmission - feeds hardware from buffer
        // Other non-blocking tasks can be added here
    }
}
