/**
 * @file main.c
 * @brief DS2431 EEPROM demo: one Read/Write/Read/Copy cycle, then a repeating search
 *
 * Demonstrates the non-blocking DS2431 driver. The shared platform layer
 * (app.h/app.c) hides the UART, the clock and the polled millisecond clock.
 *
 * On the first search the demo runs the full EEPROM write cycle on every
 * DS2431 the family-filtered search finds: Read Memory (baseline), Write
 * Scratchpad, Read Scratchpad (verify TA/ES echo), Copy Scratchpad (t_PROG
 * hold-off), then Read Memory again to confirm the data landed in EEPROM. The
 * write cycle is deliberately one-shot: once every device has been served the
 * demo announces itself and then keeps re-running the search once a second,
 * so the bench goes on streaming its `Found N device(s)` census and ROMs
 * instead of falling silent. That steady identification stream is what the
 * HIL harness scans the COM ports for - a firmware that has gone idle carries
 * nothing to find - and it also keeps the demo readable on a terminal.
 *
 * Every operation runs and leaves the bus idle when done, so a command never
 * starts another on its own. All low-level bus operations live in the shared
 * 1-Wire layer (onewire.h/onewire.c); this example only uses the public
 * high-level interface.
 */

#include "app.h"
#include "ds2431.h"

#define DS2431_SEARCH_MAX_DEVICES 8u

/* Delay between the identification searches that follow the one-shot EEPROM
 * cycle. Kept below the harness's shortest COM-port scan pass (2 s) so a scan
 * always catches a burst of census and ROM lines. */
#define IDENTIFY_INTERVAL_MS 1000u

typedef enum {
    STEP_SEARCH = 0,
    STEP_READ_BASELINE,
    STEP_WRITE_SCRATCHPAD,
    STEP_READ_SCRATCHPAD,
    STEP_COPY_SCRATCHPAD,
    STEP_READ_BACK,
    STEP_DONE
} step_t;

static step_t step = STEP_SEARCH;
static uint8_t search_running = 1;
static uint8_t cmd_running = 0;
static uint8_t demo_done = 0;
static uint32_t next_search_ms = 0;
static uint8_t found_roms[DS2431_SEARCH_MAX_DEVICES][DS2431_ROM_BYTES];
static uint8_t rom_count = 0;
static uint8_t dev_idx = 0;

static uint8_t eeprom_data[DS2431_EEPROM_SIZE];
static uint8_t scratch_frame[DS2431_SCRATCH_FRAME];
static uint8_t write_data[DS2431_SCRATCH_SIZE] = {0xDE, 0xAD, 0xBE, 0xEF, 0x01, 0x02, 0x03, 0x04};

static void print_rom(const uint8_t* rom) {
    for (uint8_t i = 0; i < DS2431_ROM_BYTES; i++) {
        uart_write_hex(rom[i]);
        if (i != DS2431_ROM_BYTES - 1) uart_tx_enqueue_byte(' ');
    }
}

static void print_bytes(const char* label, const uint8_t* data, uint8_t len) {
    uart_write_str(label);
    for (uint8_t i = 0; i < len; i++) {
        uart_write_hex(data[i]);
        if (i != len - 1) uart_tx_enqueue_byte(' ');
    }
    uart_write_str("\r\n");
}

static uint8_t device_found_sink(const uint8_t* found_rom) {
    if (rom_count < DS2431_SEARCH_MAX_DEVICES) {
        for (uint8_t i = 0; i < DS2431_ROM_BYTES; i++) {
            found_roms[rom_count][i] = found_rom[i];
        }
        rom_count++;
    }
    uart_write_str("  ROM: ");
    print_rom(found_rom);
    uart_write_str("\r\n");
    return 0;
}

static void start_step(step_t s) {
    switch (s) {
    case STEP_READ_BASELINE:
        uart_write_str("Read Memory (0xF0) - baseline:\r\n");
        ds2431_read_memory(eeprom_data, 0, DS2431_SCRATCH_SIZE);
        break;
    case STEP_WRITE_SCRATCHPAD:
        uart_write_str("Write Scratchpad (0x0F): ");
        print_bytes("", write_data, DS2431_SCRATCH_SIZE);
        ds2431_write_scratchpad(0, write_data, DS2431_SCRATCH_SIZE);
        break;
    case STEP_READ_SCRATCHPAD:
        uart_write_str("Read Scratchpad (0xAA):\r\n");
        ds2431_read_scratchpad(scratch_frame);
        break;
    case STEP_COPY_SCRATCHPAD:
        uart_write_str("Copy Scratchpad (0x55) -> EEPROM (t_PROG hold-off):\r\n");
        ds2431_copy_scratchpad();
        break;
    case STEP_READ_BACK:
        uart_write_str("Read Memory (0xF0) - after copy:\r\n");
        ds2431_read_memory(eeprom_data, 0, DS2431_SCRATCH_SIZE);
        break;
    case STEP_SEARCH:
    case STEP_DONE:
        break;
    }
}

static uint8_t poll_step(step_t s) {
    switch (s) {
    case STEP_READ_BASELINE:
    case STEP_READ_BACK:
        return ds2431_read_memory_poll();
    case STEP_WRITE_SCRATCHPAD:
        return ds2431_write_scratchpad_poll();
    case STEP_READ_SCRATCHPAD:
        return ds2431_read_scratchpad_poll();
    case STEP_COPY_SCRATCHPAD:
        return ds2431_copy_scratchpad_poll();
    case STEP_SEARCH:
    case STEP_DONE:
        return 1;
    }
    return 1;
}

static void finish_step(step_t s) {
    if (!ds2431_last_command_ok()) {
        uart_write_str("  no device present; operation aborted\r\n");
        return;
    }
    switch (s) {
    case STEP_READ_BASELINE:
        print_bytes("  EEPROM: ", eeprom_data, DS2431_SCRATCH_SIZE);
        break;
    case STEP_WRITE_SCRATCHPAD:
        uart_write_str("  scratchpad written\r\n");
        break;
    case STEP_READ_SCRATCHPAD:
        print_bytes("  frame: ", scratch_frame, DS2431_SCRATCH_FRAME);
        break;
    case STEP_COPY_SCRATCHPAD:
        uart_write_str("  copied to EEPROM\r\n");
        break;
    case STEP_READ_BACK:
        print_bytes("  EEPROM: ", eeprom_data, DS2431_SCRATCH_SIZE);
        break;
    case STEP_SEARCH:
    case STEP_DONE:
        break;
    }
}

/* Reset the per-round identification state and start a family-filtered search.
 * Safe to call between rounds: the search engine only runs while the one-wire
 * layer is idle, and by this point the previous command has left the driver in
 * DONE. */
static void start_search(void) {
    rom_count = 0;
    dev_idx = 0;
    step = STEP_SEARCH;
    uart_write_str("Searching 1-Wire bus...\r\n");
    onewire_search_start(device_found_sink, DS2431_SEARCH_MAX_DEVICES, 0xF0, DS2431_FAMILY_CODE);
    search_running = 1;
}

int main(void) {
    app_init();

    uart_write_str("DS2431 8_ds2431 starting...\r\n");
    ds2431_init();
#if OW_PARASITE_POWER
    ds2431_set_parasite(1);
#endif
    start_search();

    for (;;) {
        if (search_running) {
            if (onewire_search_poll()) {
                search_running = 0;
                uart_write_str("Found ");
                uart_write_int(onewire_search_count());
                uart_write_str(" device(s).\r\n");
                if (!demo_done && rom_count != 0u) {
                    // The one-shot EEPROM cycle: serve the first device here,
                    // the READ_BACK branch walks on to the rest.
                    uart_write_str("Selected ROM: ");
                    print_rom(found_roms[0]);
                    uart_write_str("\r\n");
                    ds2431_select(found_roms[0]);
                    step = STEP_READ_BASELINE;
                    start_step(step);
                    cmd_running = 1;
                } else {
                    if (rom_count == 0u) {
                        uart_write_str("No DS2431 found.\r\n");
                    }
                    // Identification only: report this round's census, then
                    // search again after the interval.
                    next_search_ms = app_millis() + IDENTIFY_INTERVAL_MS;
                    step = STEP_DONE;
                }
            }
        } else if (step == STEP_DONE) {
            if (!demo_done) {
                demo_done = 1;
                uart_write_str("Demo done.\r\n");
            }
            if ((int32_t)(app_millis() - next_search_ms) >= 0) {
                start_search();
            }
        } else if (!cmd_running) {
            start_step(step);
            cmd_running = 1;
        } else if (poll_step(step)) {
            cmd_running = 0;
            finish_step(step);
            if (step == STEP_READ_BACK) {
                // One device's EEPROM cycle is over; move on to the next
                // found DS2431, or finish once every device has been served.
                // select() is accepted here: the previous command completed,
                // leaving the driver in DONE, and the search is no longer
                // active.
                dev_idx++;
                if (dev_idx >= rom_count) {
                    step = STEP_DONE;
                    next_search_ms = app_millis() + IDENTIFY_INTERVAL_MS;
                } else {
                    uart_write_str("Selected ROM: ");
                    print_rom(found_roms[dev_idx]);
                    uart_write_str("\r\n");
                    ds2431_select(found_roms[dev_idx]);
                    step = STEP_READ_BASELINE;
                }
            } else {
                step = (step_t)(step + 1);
            }
        }
        uart_poll_tx();
    }
}
