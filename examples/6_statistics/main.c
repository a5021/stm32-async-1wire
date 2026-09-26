/**
 * @file main.c
 * @brief Multi-sensor example with signal statistics collection
 *
 * Builds on the `3_round_robin` sequential architecture but replaces the
 * resolution cycling with a statistics dump: after STATS_DUMP_SWEEPS sweeps
 * - one sweep being one pass over every device on the bus - the accumulated
 * pulse-width histogram, per-sensor min/max, and error counters are printed
 * via UART, then the counters are reset for the next batch.
 *
 * The driver measures only when asked (ds18b20_start_measure()), so this demo
 * needs no time base at all: it requests the next cycle as soon as the dump
 * is out of the way, which makes the conversion time itself the cadence (and
 * collects statistics as fast as the bus allows).
 *
 * Requires OW_STATS_ENABLE=1 at build time (auto-added by `make APP=6_statistics`):
 *   On a parasite-powered bus add -DOW_PARASITE_POWER=1 so the driver engages
 *   the strong pull-up during the conversion window.
 *
 * The period is counted in sweeps, not in wall-clock time: the example
 * deliberately runs with no time base, so the conversion time is the cadence
 * and how long a batch takes depends on how many devices are on the bus. The
 * `---` separator printed at each sweep boundary makes the progress visible, so
 * the wait is observable rather than silent.
 *
 * The dump period is defined here and nowhere else. `make APP=6_statistics`
 * and the CMake example target enable OW_STATS_ENABLE and nothing more, so
 * this default is reachable and a caller can still override it with
 * -DSTATS_DUMP_SWEEPS=N in EXT.
 */

#include "app.h"
#include "ds18b20.h"
#include "ow_stats.h"

/* ======== Configuration ======== */
#ifndef DS18B20_SEARCH_MAX_DEVICES
#define DS18B20_SEARCH_MAX_DEVICES 8u
#endif

/* Sweeps between stats dumps. One sweep is one pass over every device, so a
 * batch covers STATS_DUMP_SWEEPS * device_count measurements and yields
 * STATS_DUMP_SWEEPS samples per sensor. */
#ifndef STATS_DUMP_SWEEPS
#define STATS_DUMP_SWEEPS 10u
#endif

/* ======== Device table ======== */
static uint8_t found_roms[DS18B20_SEARCH_MAX_DEVICES][8];
static uint8_t found_count = 0;
static uint8_t select_index = 0;
static uint8_t search_running = 1;

/* ======== Non-blocking stats dump state ======== */
static uint8_t dump_busy = 0; /**< 1 while ow_stats_dump_poll() is running */
static uint32_t sweep_count = 0; /**< Completed sweeps since the last reset */
static uint8_t measure_pending = 0; /**< 1 = the demo wants the next cycle started */

/* ======== Search callback ======== */
static uint8_t device_found_sink(const uint8_t* rom) {
    for (uint8_t i = 0; i < DS18B20_ROM_BYTES; i++) {
        found_roms[found_count][i] = rom[i];
    }
    found_count++;
    uart_write_str("  ROM: ");
    for (uint8_t i = 0; i < DS18B20_ROM_BYTES; i++) {
        uart_write_hex(rom[i]);
        if (i != DS18B20_ROM_BYTES - 1) uart_tx_enqueue_byte(' ');
    }
    uart_write_str("\r\n");
    return 0;
}

static void report_search_result(void) {
    if (found_count == 0) {
        uart_write_str("No devices on the 1-Wire bus.\r\n");
    } else {
        uart_write_str("Found ");
        uart_write_int(found_count);
        uart_write_str(" device(s).\r\n");
        /* Say when the first dump is due, and how many samples it will carry.
         * The period is counted in sweeps, not seconds, so the wall-clock time
         * depends on the fleet - stating the sample count makes the wait
         * checkable against the `t=` total the dump reports. */
        uart_write_str("  stats dump every ");
        uart_write_int(STATS_DUMP_SWEEPS);
        uart_write_str(" sweeps (");
        uart_write_int((int32_t)STATS_DUMP_SWEEPS * (uint32_t)found_count);
        uart_write_str(" samples with ");
        uart_write_int(found_count);
        uart_write_str(" devices)\r\n");
        select_index = 0;
        ds18b20_select(found_roms[select_index]);
        ds18b20_start_measure(); // Request the first measurement cycle
    }
}

/* ======== Measurement complete callback ======== */
void ds18b20_complete(int16_t temp) {
    /* Print temperature or error */
    for (uint8_t i = 0; i < DS18B20_ROM_BYTES; i++) {
        uart_write_hex(found_roms[select_index][i]);
        if (i != DS18B20_ROM_BYTES - 1) uart_tx_enqueue_byte(' ');
    }
    uart_write_str(": ");

    if (temp == DS18B20_TEMP_ERROR_NO_SENSOR) {
        uart_write_str("no sensor");
    } else if (temp == DS18B20_TEMP_ERROR_CRC_FAIL) {
        uart_write_str("CRC fail");
    } else if (temp == DS18B20_TEMP_ERROR_GENERIC) {
        uart_write_str("error");
    } else {
        int whole = temp / 10;
        int frac = temp % 10;
        if (frac < 0) frac = -frac;
        if (whole == 0 && temp < 0) {
            uart_write_str("-0");
        } else {
            uart_write_int(whole);
        }
        uart_write_str(".");
        uart_write_int(frac);
        uart_write_str(" C");
    }
    uart_write_str("\r\n");

    /* Round-robin to next device */
    if (found_count > 1) {
        select_index = (uint8_t)((select_index + 1u) % found_count);
        ds18b20_select(found_roms[select_index]);
        if (select_index == 0) {
            uart_write_str("---\r\n"); /* sweep boundary marker */
        }
    }

    /* Count the sweep here, outside the found_count > 1 branch above, and not
     * on the marker inside it: with a single device that branch never runs, so
     * counting there would mean the batch never completes on a one-sensor bus
     * - which it did before, because the period used to be counted in raw
     * measurements. Testing select_index after the advance covers both cases,
     * since with one device it is always 0. Kept adjacent to the marker above
     * so the visible separator and the counter cannot drift apart. */
    if (select_index == 0) {
        sweep_count++;
    }

    /* Stats: tick every measurement so total_cycles stays a count of
     * measurements, but trigger the dump per sweep, not per measurement. */
    (void)ow_stats_tick();
    if (sweep_count >= STATS_DUMP_SWEEPS && !dump_busy) {
        ow_stats_dump_start();
        dump_busy = 1;
    } else {
        /* No dump in the way: ask for the next cycle straight away. The
         * driver is parked after this callback, so it stays idle until the
         * main loop calls ds18b20_start_measure(). */
        measure_pending = 1;
    }
}

/* ======== Main ======== */
int main(void) {
    app_init();

    uart_write_str("DS18B20 6_statistics (stats) starting...\r\n");

    ow_stats_init();
    ds18b20_init();
#if OW_PARASITE_POWER
    ds18b20_set_parasite(1);
#endif
    ds18b20_search_start(device_found_sink, DS18B20_SEARCH_MAX_DEVICES);

    for (;;) {
        if (dump_busy) {
            if (ow_stats_dump_poll()) {
                dump_busy = 0;
                ow_stats_reset();
                /* The sweep counter lives in the example, not in the module, so
                 * ow_stats_reset() does not clear it. Without this it stays at
                 * the threshold, every following measurement starts another dump,
                 * and the example degenerates into a dump after each single
                 * device - which is what it did on hardware before this line
                 * existed, showing "stats [1 c]" back to back. */
                sweep_count = 0;
                // Resume measuring now that the bus is free again.
                measure_pending = 1;
            }
        } else if (search_running) {
            if (ds18b20_search_poll()) {
                search_running = 0;
                found_count = ds18b20_search_count();
                report_search_result();
            }
        } else {
            ds18b20_poll();
            if (measure_pending) {
                measure_pending = 0;
                ds18b20_start_measure();
            }
        }
        uart_poll_tx();
    }
}
