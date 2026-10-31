/**
 * @file main.c
 * @brief Peak and low temperature recorder: DS18B20 sensors + DS2431 EEPROM
 *
 * One explicit bus-owner state machine uses the two drivers strictly in
 * sequence, because both share the TIM1/DMA/GPIO resources and neither may be
 * polled while the other owns the timer:
 *
 *   1. DS18B20 device search (fills the driver's scan-mode device table);
 *   2. a full-family search to locate the DS2431 (the search engine can only
 *      run one family filter at a time, so this is a second pass);
 *   3. Read Memory of the four EEPROM blocks below. The records are then kept
 *      in RAM; the EEPROM is only written again when a record actually changes;
 *   4. measurement rounds: one broadcast Convert T for all sensors (scan
 *      mode), each result reported through ds18b20_complete(). If a scan round
 *      fails, the application switches to the sequential Match ROM path;
 *   5. the round maximum is compared with the stored peak, the round minimum
 *      with the stored low. Every change becomes a write job, executed one
 *      after another: Write Scratchpad -> Read Scratchpad (echo + data verify)
 *      -> Copy Scratchpad -> Read Memory (read-back compare).
 *
 * EEPROM map (DS2431, 8-byte blocks, all records self-validating):
 *
 *   16..23  peak record   offset 20 = peak temperature, whole degrees (int8)
 *   24..31  low record    offset 28 = low temperature,  whole degrees (int8)
 *   32..39  peak sensor ROM (last byte = ROM CRC)
 *   40..47  low sensor ROM  (last byte = ROM CRC)
 *   48..127 unused
 *
 * Record block (same layout for peak and low, distinguished by the magic):
 *
 *   +0  magic   0x54 peak / 0x4C low
 *   +1  version 0x02
 *   +2  flags   bit0 = record valid, bit1 = sensor ROM block written
 *   +3  counter update count of this record (wraps)
 *   +4  whole degrees (int8)
 *   +5  tenths of a degree, signed (-9..+9, same sign as the degrees)
 *   +6  sensor index in the device table when the record was set
 *   +7  CRC-8 (Dallas/Maxim) over +0..+6
 *
 * ROM blocks are written before the record that refers to them, so a power
 * loss can never leave a record claiming a ROM that is not there. A record
 * only moves in its own direction: the peak grows, the low falls, so after
 * warm-up new records are rare and EEPROM endurance is a non-issue.
 *
 * Non-blocking: every stage is a poll() loop paced by app_millis(); there is
 * no delay, no wait and no interrupt anywhere in the application.
 */

#include "app.h"
#include "ds2413.h"
#include "ds2431.h"
#include <string.h>

/* --- EEPROM map ---------------------------------------------------------- */

#define REC_BYTES       DS2431_SCRATCH_SIZE
#define REC_CRC_LEN     7u
#define DS18_FAMILY     0x28u

#define SLOT_COUNT      2u
#define SLOT_PEAK       0u
#define SLOT_LOW        1u

#define REC_PEAK_ADDR   16u
#define REC_LOW_ADDR    24u
#define ROM_PEAK_ADDR   32u
#define ROM_LOW_ADDR    40u

#define REC_MAGIC_PEAK  0x54u
#define REC_MAGIC_LOW   0x4Cu
#define REC_VERSION     0x02u
#define REC_FLAG_VALID  0x01u
#define REC_FLAG_ROM    0x02u

#define BOOT_READS      4u
#define JOB_MAX         4u

static const uint16_t rec_addr[SLOT_COUNT] = {REC_PEAK_ADDR, REC_LOW_ADDR};
static const uint16_t rom_addr[SLOT_COUNT] = {ROM_PEAK_ADDR, ROM_LOW_ADDR};
static const uint16_t boot_addr[BOOT_READS] = {REC_PEAK_ADDR, REC_LOW_ADDR,
                                               ROM_PEAK_ADDR, ROM_LOW_ADDR};
static const char* const slot_name[SLOT_COUNT] = {"peak", "low"};

/* --- timing -------------------------------------------------------------- */

/* One broadcast conversion plus N Match ROM reads. */
#define SCAN_TIMEOUT_MS    4000u
/* One Match ROM conversion plus its scratchpad read. */
#define SEQ_TIMEOUT_MS     2000u
/* Extra window to let a failing scan round drain before switching modes. */
#define DRAIN_TIMEOUT_MS   2000u
/* Any single DS2431 command finishes well inside this window. */
#define EEPROM_TIMEOUT_MS  1000u
/* Search of a 16-device bus is a few tens of milliseconds. */
#define SEARCH_TIMEOUT_MS  2000u
/* Pause between measurement rounds. */
#define CYCLE_PERIOD_MS    2000u
/* Give up on EEPROM persistence after this many failed write jobs. */
#define WRITE_FAIL_LIMIT   3u

#ifndef PEAK_RESET
#define PEAK_RESET 0
#endif

typedef enum {
    ST_SEARCH_DS18 = 0,
    ST_SEARCH_ALL,
    ST_BOOT_READ,
    ST_MEASURE_START,
    ST_MEASURE_WAIT,
    ST_WRITE_SCRATCH,
    ST_WRITE_VERIFY,
    ST_WRITE_COPY,
    ST_WRITE_READBACK,
    ST_WAIT,
    ST_STOP
} state_t;

typedef struct {
    uint8_t valid;      /* a record was read or committed */
    uint8_t counter;    /* update counter of this record */
    int16_t temp;       /* tenths of a degree */
    uint8_t idx;        /* sensor index in the device table */
    uint8_t rom_valid;
    uint8_t rom[8];     /* ROM of the sensor the record refers to */
} slot_t;

typedef struct {
    uint8_t is_rom;     /* 1 = ROM block, 0 = record block */
    uint8_t slot;
    uint16_t addr;
    uint8_t image[REC_BYTES];
} job_t;

static state_t state = ST_SEARCH_DS18;

/* Bus devices */
static uint8_t ds18_n;
static uint8_t ds18_sink_n;
static uint8_t ds2431_rom[8];
static uint8_t have_ds2431;
static uint8_t ds2431_n;
static uint8_t ds2413_n;
static uint8_t other_n;

/* Measurement round */
static uint8_t seq_mode;
static uint8_t seq_cur;
static uint8_t reported;
static int16_t temps[DS18B20_MAX_DEVICES];
static uint8_t temps_ok[DS18B20_MAX_DEVICES];
static uint32_t cycle_no;
static uint8_t scan_drain;

/* Records and ROMs as known to the application */
static slot_t slots[SLOT_COUNT];

/* Boot read-out: blocks in the order of boot_addr[] */
static uint8_t boot_buf[BOOT_READS * REC_BYTES];
static uint8_t boot_ok[BOOT_READS];
static uint8_t boot_step;

/* Write jobs of the current cycle, executed in order */
static job_t jobs[JOB_MAX];
static uint8_t job_n;
static uint8_t job_i;
static uint8_t verify_buf[REC_BYTES];
static uint8_t scratch_frame[DS2431_SCRATCH_FRAME];
static uint8_t write_fails;
static uint8_t persist_off;

/* Timing */
static uint32_t deadline_ms;
static uint32_t next_ms;
static uint32_t drain_deadline_ms;

/* -------------------------------------------------------------------------- */
/* Helpers                                                                    */
/* -------------------------------------------------------------------------- */

static uint8_t time_reached(uint32_t when)
{
    return (uint8_t)((int32_t)(app_millis() - when) >= 0);
}

static uint8_t expired(void)
{
    return time_reached(deadline_ms);
}

static void go_wait(void)
{
    next_ms = app_millis() + CYCLE_PERIOD_MS;
    state = ST_WAIT;
}

static void print_rom(const uint8_t* rom)
{
    for (uint8_t i = 0; i < 8u; i++) {
        uart_write_hex(rom[i]);
        if (i != 7u) {
            uart_tx_enqueue_byte(' ');
        }
    }
}

/** @brief Print a tenths-of-a-degree value as "-5.3" / "27.4" / "-0.4" */
static void print_temp(int16_t t)
{
    int16_t whole = (int16_t)(t / 10);
    int16_t frac = (int16_t)(t % 10);

    if (frac < 0) {
        frac = (int16_t)-frac;
    }
    if (t < 0 && whole == 0) {
        uart_write_str("-");
    }
    uart_write_int(whole);
    uart_write_str(".");
    uart_write_int(frac);
}

/** @brief Tenths of a degree from a record block (bytes +4 and +5) */
static int16_t decode_temp(const uint8_t* image)
{
    return (int16_t)((int16_t)(int8_t)image[4] * 10 + (int16_t)(int8_t)image[5]);
}

static void slot_report(uint8_t slot)
{
    const slot_t* s = &slots[slot];

    uart_write_str(slot_name[slot]);
    uart_write_str(": #");
    uart_write_int(s->counter);
    uart_write_str(" ");
    print_temp(s->temp);
    uart_write_str(" C, sensor ");
    uart_write_int(s->idx);
    if (s->rom_valid) {
        uart_write_str(" [");
        print_rom(s->rom);
        uart_write_str("]");
    } else {
        uart_write_str(" (rom not stored)");
    }
    uart_write_str("\r\n");
}

/* -------------------------------------------------------------------------- */
/* Device searches                                                            */
/* -------------------------------------------------------------------------- */

static uint8_t ds18_sink(const uint8_t* rom)
{
    uart_write_str("  [");
    uart_write_int(ds18_sink_n);
    uart_write_str("] ");
    print_rom(rom);
    uart_write_str("\r\n");
    ds18_sink_n++;
    return 0u;
}

static uint8_t all_sink(const uint8_t* rom)
{
    switch (rom[0]) {
    case DS18_FAMILY:
        break; /* already in the driver table from the first pass */

    case DS2431_FAMILY_CODE:
        ds2431_n++;
        if (!have_ds2431) {
            for (uint8_t i = 0; i < 8u; i++) {
                ds2431_rom[i] = rom[i];
            }
            have_ds2431 = 1u;
            uart_write_str("  DS2431 ");
            print_rom(rom);
            uart_write_str("\r\n");
        }
        break;

    case DS2413_FAMILY_CODE:
        ds2413_n++;
        uart_write_str("  DS2413 ");
        print_rom(rom);
        uart_write_str(" (driver not implemented)\r\n");
        break;

    default:
        other_n++;
        uart_write_str("  other ");
        print_rom(rom);
        uart_write_str("\r\n");
        break;
    }
    return 0u;
}

/* -------------------------------------------------------------------------- */
/* Boot: load and validate the records                                        */
/* -------------------------------------------------------------------------- */

static void boot_load_record(uint8_t slot)
{
    const uint8_t* b = &boot_buf[slot * REC_BYTES];
    uint8_t magic = (slot == SLOT_PEAK) ? REC_MAGIC_PEAK : REC_MAGIC_LOW;
    const char* why = 0;

    if (!boot_ok[slot]) {
        why = "unreadable";
    } else if (b[0] != magic || b[1] != REC_VERSION) {
        why = "blank or unknown layout";
    } else if (ds18b20_crc8(b, REC_CRC_LEN) != b[REC_CRC_LEN]) {
        why = "CRC mismatch";
    } else if ((b[2] & REC_FLAG_VALID) == 0u) {
        why = "marked invalid";
    } else if ((int8_t)b[5] < -9 || (int8_t)b[5] > 9) {
        why = "tenths out of range";
    }

    if (why != 0) {
        uart_write_str(slot_name[slot]);
        uart_write_str(": ");
        uart_write_str(why);
        uart_write_str(", will be initialised\r\n");
        return;
    }

    slots[slot].valid = 1u;
    slots[slot].counter = b[3];
    slots[slot].temp = decode_temp(b);
    slots[slot].idx = b[6];
}

static void boot_load_rom(uint8_t slot)
{
    const uint8_t* b = &boot_buf[(BOOT_READS / SLOT_COUNT + slot) * REC_BYTES];
    uint8_t k = (uint8_t)(BOOT_READS / SLOT_COUNT + slot);

    if (boot_ok[k] && b[0] == DS18_FAMILY && ds18b20_crc8(b, 7u) == b[7]) {
        memcpy(slots[slot].rom, b, 8u);
        slots[slot].rom_valid = 1u;
    }
}

static void boot_done(void)
{
#if PEAK_RESET
    uart_write_str("PEAK_RESET=1: stored records ignored\r\n");
#else
    for (uint8_t s = 0; s < SLOT_COUNT; s++) {
        boot_load_record(s);
        boot_load_rom(s);
    }
    for (uint8_t s = 0; s < SLOT_COUNT; s++) {
        if (slots[s].valid) {
            slot_report(s);
        }
    }
#endif

    if (ds18_n != 0u) {
        go_wait();
    } else {
        uart_write_str("No DS18B20, nothing to measure.\r\n");
        state = ST_STOP;
    }
}

static void boot_read_start(void)
{
    ds2431_read_memory(&boot_buf[boot_step * REC_BYTES], boot_addr[boot_step], REC_BYTES);
    deadline_ms = app_millis() + EEPROM_TIMEOUT_MS;
}

/* -------------------------------------------------------------------------- */
/* Measurement                                                                */
/* -------------------------------------------------------------------------- */

/**
 * @brief DS18B20 weak callback: one device finished its conversion
 * @note The result is only stored here; the state machine prints it once the
 *       poll returns, so the UART work stays out of the driver call stack.
 */
void ds18b20_complete(int16_t temp)
{
    uint8_t idx = seq_mode ? seq_cur : ds18b20_scan_index();

    if (idx < DS18B20_MAX_DEVICES) {
        temps[idx] = temp;
        temps_ok[idx] = (uint8_t)(temp > DS18B20_TEMP_ERROR_CRC_FAIL);
    }
    reported++;
}

static void start_round(void)
{
    reported = 0u;
    scan_drain = 0u;
    cycle_no++;
    for (uint8_t i = 0; i < DS18B20_MAX_DEVICES; i++) {
        temps[i] = DS18B20_TEMP_ERROR_GENERIC;
        temps_ok[i] = 0u;
    }

    if (!seq_mode) {
        ds18b20_scan_start();
        ds18b20_start_measure();
        deadline_ms = app_millis() + SCAN_TIMEOUT_MS;
    } else {
        seq_cur = 0u;
        ds18b20_select(ds18b20_device_rom(seq_cur));
        ds18b20_start_measure();
        deadline_ms = app_millis() + SEQ_TIMEOUT_MS;
    }
    state = ST_MEASURE_WAIT;
}

/** @brief Index of the first sensor that failed, or ds18_n when all are fine */
static uint8_t first_bad(void)
{
    for (uint8_t i = 0; i < ds18_n; i++) {
        if (!temps_ok[i]) {
            return i;
        }
    }
    return ds18_n;
}

/** @brief Leave scan mode for the sequential Match ROM path */
static void switch_to_sequential(void)
{
    seq_mode = 1u;
    /* select() only acts at driver IDLE (the round has just drained), and it
     * clears the scan-mode flag, so no round can start on its own any more. */
    ds18b20_select(ds18b20_device_rom(0));
    uart_write_str("fallback: sequential Match ROM measurements\r\n");
    go_wait();
}

/* -------------------------------------------------------------------------- */
/* Write jobs                                                                 */
/* -------------------------------------------------------------------------- */

static void push_rom(uint8_t slot, const uint8_t* rom)
{
    job_t* j = &jobs[job_n++];

    j->is_rom = 1u;
    j->slot = slot;
    j->addr = rom_addr[slot];
    memcpy(j->image, rom, REC_BYTES);
}

static void push_record(uint8_t slot, int16_t temp, uint8_t idx)
{
    job_t* j = &jobs[job_n++];
    uint8_t* b = j->image;

    j->is_rom = 0u;
    j->slot = slot;
    j->addr = rec_addr[slot];
    b[0] = (slot == SLOT_PEAK) ? REC_MAGIC_PEAK : REC_MAGIC_LOW;
    b[1] = REC_VERSION;
    b[2] = REC_FLAG_VALID | REC_FLAG_ROM;
    b[3] = (uint8_t)(slots[slot].counter + 1u);
    b[4] = (uint8_t)(int8_t)(temp / 10);
    b[5] = (uint8_t)(int8_t)(temp % 10);
    b[6] = idx;
    b[7] = ds18b20_crc8(b, REC_CRC_LEN);
}

/** @brief Queue the ROM (only when it differs) and the new record of a slot */
static void queue_extreme(uint8_t slot, int16_t temp, uint8_t idx)
{
    const uint8_t* rom = ds18b20_device_rom(idx);

    if (!slots[slot].rom_valid || memcmp(slots[slot].rom, rom, 8u) != 0) {
        push_rom(slot, rom);
    }
    push_record(slot, temp, idx);
}

/** @brief Apply a completed job to the RAM copy */
static void commit_job(const job_t* j)
{
    slot_t* s = &slots[j->slot];

    if (j->is_rom) {
        memcpy(s->rom, j->image, 8u);
        s->rom_valid = 1u;
    } else {
        s->valid = 1u;
        s->counter = j->image[3];
        s->temp = decode_temp(j->image);
        s->idx = j->image[6];
    }
}

static void write_abort(const char* why)
{
    uart_write_str("  write failed: ");
    uart_write_str(why);
    uart_write_str("\r\n");

    job_n = 0u;
    write_fails++;
    if (write_fails >= WRITE_FAIL_LIMIT) {
        persist_off = 1u;
        uart_write_str("  EEPROM persistence disabled for this session, "
                       "the records stay in RAM\r\n");
    }
    go_wait();
}

static void start_job(void)
{
    const job_t* j = &jobs[job_i];

    if (!ds2431_write_scratchpad(j->addr, j->image, REC_BYTES)) {
        write_abort("scratchpad write rejected");
        return;
    }
    deadline_ms = app_millis() + EEPROM_TIMEOUT_MS;
    state = ST_WRITE_SCRATCH;
}

static void job_done(void)
{
    const job_t* j = &jobs[job_i];

    commit_job(j);
    write_fails = 0u;

    if (j->is_rom) {
        uart_write_str("  ");
        uart_write_str(slot_name[j->slot]);
        uart_write_str(" ROM stored and verified\r\n");
    } else {
        uart_write_str("  ");
        slot_report(j->slot);
    }

    job_i++;
    if (job_i < job_n) {
        start_job();
    } else {
        job_n = 0u;
        go_wait();
    }
}

/** @brief Print a round extreme next to the record it is compared with */
static void print_extreme(const char* tag, int16_t t, uint8_t idx, uint8_t slot, uint8_t is_new)
{
    uart_write_str(tag);
    print_temp(t);
    uart_write_str(" C (s");
    uart_write_int(idx);
    uart_write_str(")");
    if (is_new) {
        uart_write_str(" -> NEW record");
        if (slots[slot].valid) {
            uart_write_str(" (was ");
            print_temp(slots[slot].temp);
            uart_write_str(")");
        }
    } else {
        uart_write_str(", record ");
        print_temp(slots[slot].temp);
        uart_write_str(" C #");
        uart_write_int(slots[slot].counter);
    }
}

static void evaluate(void)
{
    int16_t hi = 0;
    int16_t lo = 0;
    uint8_t hi_i = 0u;
    uint8_t lo_i = 0u;
    uint8_t have = 0u;

    for (uint8_t i = 0; i < ds18_n; i++) {
        if (!temps_ok[i]) {
            continue;
        }
        if (!have || temps[i] > hi) {
            hi = temps[i];
            hi_i = i;
        }
        if (!have || temps[i] < lo) {
            lo = temps[i];
            lo_i = i;
        }
        have = 1u;
    }

    uart_write_str("cycle ");
    uart_write_int(cycle_no);
    uart_write_str(": ");
    for (uint8_t i = 0; i < ds18_n; i++) {
        if (temps_ok[i]) {
            print_temp(temps[i]);
        } else {
            uart_write_str("--.-");
        }
        if (have && i == hi_i) {
            uart_write_str("* ");
        } else if (have && i == lo_i) {
            uart_write_str("v ");
        } else {
            uart_write_str(" ");
        }
    }

    if (!have) {
        uart_write_str("| no valid measurement\r\n");
        go_wait();
        return;
    }

    {
        uint8_t hi_new = (uint8_t)(!slots[SLOT_PEAK].valid || hi > slots[SLOT_PEAK].temp);
        uint8_t lo_new = (uint8_t)(!slots[SLOT_LOW].valid || lo < slots[SLOT_LOW].temp);

        print_extreme("| max ", hi, hi_i, SLOT_PEAK, hi_new);
        print_extreme(" | min ", lo, lo_i, SLOT_LOW, lo_new);
        uart_write_str("\r\n");

        if (!hi_new && !lo_new) {
            go_wait();
            return;
        }

        job_n = 0u;
        if (hi_new) {
            queue_extreme(SLOT_PEAK, hi, hi_i);
        }
        if (lo_new) {
            queue_extreme(SLOT_LOW, lo, lo_i);
        }

        if (persist_off || !have_ds2431) {
            for (uint8_t i = 0; i < job_n; i++) {
                commit_job(&jobs[i]);
            }
            job_n = 0u;
            uart_write_str("  EEPROM unavailable, records kept in RAM only\r\n");
            go_wait();
            return;
        }

        uart_write_str("  writing ");
        uart_write_int(job_n);
        uart_write_str(" block(s)\r\n");
        job_i = 0u;
        start_job();
    }
}

/* -------------------------------------------------------------------------- */
/* Main                                                                       */
/* -------------------------------------------------------------------------- */

int main(void)
{
    app_init();

    uart_write_str("\r\n9_multi: DS18B20 peak/low recorder, DS2431 at offset 20\r\n");
    uart_write_str("Legend: * = cycle maximum, v = cycle minimum\r\n");

    /* Both inits run before any bus operation, so the second onewire_init()
     * only re-applies the same idle configuration. */
    ds18b20_init();
    ds2431_init();
#if OW_PARASITE_POWER
    ds18b20_set_parasite(1);
    ds2431_set_parasite(1);
    uart_write_str("Parasite power: strong pull-up enabled\r\n");
#endif

    uart_write_str("Search DS18B20 devices...\r\n");
    ds18b20_search_start(ds18_sink, DS18B20_MAX_DEVICES);
    deadline_ms = app_millis() + SEARCH_TIMEOUT_MS;

    for (;;) {
        switch (state) {
        case ST_SEARCH_DS18:
            if (ds18b20_search_poll()) {
                ds18_n = ds18b20_device_count();
                uart_write_str("DS18B20 devices: ");
                uart_write_int(ds18_n);
                uart_write_str("\r\nSearch DS2431 devices...\r\n");
                onewire_search_start(all_sink, 16u, ONEWIRE_SEARCH_ROM, 0u);
                deadline_ms = app_millis() + SEARCH_TIMEOUT_MS;
                state = ST_SEARCH_ALL;
            } else if (expired()) {
                uart_write_str("DS18B20 search timeout.\r\n");
                state = ST_STOP;
            }
            break;

        case ST_SEARCH_ALL:
            if (onewire_search_poll()) {
                uart_write_str("DS2431 devices: ");
                uart_write_int(ds2431_n);
                uart_write_str(", DS2413: ");
                uart_write_int(ds2413_n);
                uart_write_str(", other: ");
                uart_write_int(other_n);
                uart_write_str("\r\n");
                if (have_ds2431) {
                    ds2431_select(ds2431_rom);
                    uart_write_str("Read records (0x10, 0x18) and sensor ROMs (0x20, 0x28)\r\n");
                    boot_step = 0u;
                    boot_read_start();
                    state = ST_BOOT_READ;
                } else {
                    uart_write_str("No DS2431: records kept in RAM only.\r\n");
                    boot_done();
                }
            } else if (expired()) {
                uart_write_str("DS2431 search timeout.\r\n");
                state = ST_STOP;
            }
            break;

        case ST_BOOT_READ:
            if (ds2431_read_memory_poll()) {
                boot_ok[boot_step] = ds2431_last_command_ok();
                boot_step++;
                if (boot_step < BOOT_READS) {
                    boot_read_start();
                } else {
                    boot_done();
                }
            } else if (expired()) {
                uart_write_str("DS2431 read timeout: stored records ignored\r\n");
                have_ds2431 = 0u;
                boot_done();
            }
            break;

        case ST_MEASURE_START:
            start_round();
            break;

        case ST_MEASURE_WAIT:
            ds18b20_poll();

            if (!seq_mode) {
                if (!scan_drain) {
                    if (reported >= ds18_n) {
                        uint8_t bad = first_bad();
                        if (bad >= ds18_n) {
                            evaluate();
                            break;
                        }
                        uart_write_str("scan: sensor ");
                        uart_write_int(bad);
                        uart_write_str(" reported an error\r\n");
                        scan_drain = 1u;
                        drain_deadline_ms = app_millis() + DRAIN_TIMEOUT_MS;
                    } else if (expired()) {
                        uart_write_str("scan: round timeout after ");
                        uart_write_int((int)reported);
                        uart_write_str(" of ");
                        uart_write_int((int)ds18_n);
                        uart_write_str(" results\r\n");
                        scan_drain = 1u;
                        drain_deadline_ms = app_millis() + DRAIN_TIMEOUT_MS;
                    }
                } else if (reported >= ds18_n) {
                    switch_to_sequential();
                } else if (time_reached(drain_deadline_ms)) {
                    uart_write_str("scan: driver did not finish the round, "
                                   "retrying scan next cycle\r\n");
                    go_wait();
                }
                break;
            }

            /* Sequential match-ROM path: one report per started conversion. */
            if (reported > seq_cur) {
                uart_write_str("  s");
                uart_write_int(seq_cur);
                uart_write_str(": ");
                if (temps_ok[seq_cur]) {
                    print_temp(temps[seq_cur]);
                    uart_write_str(" C\r\n");
                } else {
                    uart_write_str("no sensor / CRC error\r\n");
                }
                seq_cur++;
                if (seq_cur < ds18_n) {
                    ds18b20_select(ds18b20_device_rom(seq_cur));
                    ds18b20_start_measure();
                    deadline_ms = app_millis() + SEQ_TIMEOUT_MS;
                } else {
                    evaluate();
                }
            } else if (expired()) {
                uart_write_str("  s");
                uart_write_int(seq_cur);
                uart_write_str(": measurement timeout\r\n");
                temps_ok[seq_cur] = 0u;
                seq_cur++;
                if (seq_cur < ds18_n) {
                    ds18b20_select(ds18b20_device_rom(seq_cur));
                    ds18b20_start_measure();
                    deadline_ms = app_millis() + SEQ_TIMEOUT_MS;
                } else {
                    evaluate();
                }
            }
            break;

        case ST_WRITE_SCRATCH:
            if (ds2431_write_scratchpad_poll()) {
                if (!ds2431_last_command_ok()) {
                    write_abort("scratchpad write not acknowledged");
                } else {
                    ds2431_read_scratchpad(scratch_frame);
                    deadline_ms = app_millis() + EEPROM_TIMEOUT_MS;
                    state = ST_WRITE_VERIFY;
                }
            } else if (expired()) {
                write_abort("scratchpad write timeout");
            }
            break;

        case ST_WRITE_VERIFY:
            if (ds2431_read_scratchpad_poll()) {
                if (!ds2431_last_command_ok()) {
                    write_abort("scratchpad echo/data mismatch");
                } else {
                    ds2431_copy_scratchpad();
                    deadline_ms = app_millis() + EEPROM_TIMEOUT_MS;
                    state = ST_WRITE_COPY;
                }
            } else if (expired()) {
                write_abort("scratchpad read timeout");
            }
            break;

        case ST_WRITE_COPY:
            if (ds2431_copy_scratchpad_poll()) {
                if (!ds2431_last_command_ok()) {
                    write_abort("copy refused by the device");
                } else {
                    ds2431_read_memory(verify_buf, jobs[job_i].addr, REC_BYTES);
                    deadline_ms = app_millis() + EEPROM_TIMEOUT_MS;
                    state = ST_WRITE_READBACK;
                }
            } else if (expired()) {
                write_abort("copy timeout");
            }
            break;

        case ST_WRITE_READBACK:
            if (ds2431_read_memory_poll()) {
                if (!ds2431_last_command_ok()) {
                    write_abort("read-back failed");
                } else if (memcmp(verify_buf, jobs[job_i].image, REC_BYTES) != 0) {
                    write_abort("read-back differs from the write");
                } else {
                    job_done();
                }
            } else if (expired()) {
                write_abort("read-back timeout");
            }
            break;

        case ST_WAIT:
            if (ds18_n != 0u && time_reached(next_ms)) {
                state = ST_MEASURE_START;
            }
            break;

        case ST_STOP:
            break;
        }

        uart_poll_tx();
    }
}
