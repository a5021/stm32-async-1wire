/*
 * libFuzzer harness for the DS18B20 command-transaction engine
 * (ds18b20_read_rom / _read_scratchpad / _set_alarm_thresholds /
 * _copy_scratchpad / _recall_eeprom / _detect_parasite + *_poll).
 *
 * Compiled as single-TU via ds18b20_test_access.c (src/ds18b20.c +
 * src/onewire.c); the strong callback spies in tests/mock/ds18b20_test_spy.c
 * are not needed here (transactions report via ds18b20_last_command_ok()).
 *
 * Input layout:
 *   data[0] & 0x07 -> command index 0..5:
 *       0 Read ROM (0x33, bare, reads 8 bytes)
 *       1 Read Scratchpad (0xBE, reads 9 bytes; adapts resolution)
 *       2 Write Scratchpad thresholds (0x4E, no response)
 *       3 Copy Scratchpad (0x48, timed hold-off)
 *       4 Recall EEPROM (0xB8, timed hold-off)
 *       5 Read Power Supply (0xB4, reads 1 byte; sets parasite flag)
 *   data[0] & 0x08 -> Match ROM addressing (Skip ROM otherwise)
 *   data[0] & 0x10 -> parasite power (ds18b20_set_parasite(1))
 * plus one presence bit (bit 0 of the next stream byte) and the read
 * response bytes when the command reads data back.
 *
 * The capture source decodes the running operation from mock_tim1.RCR:
 *   rcr == 0      -> reset op (idx0 reset pulse, idx1 presence)
 *   rcr ==  7     -> 1-byte read (Read Power Supply)
 *   rcr == 63     -> 8-byte read (Read ROM)
 *   rcr == 71     -> 9-byte read (Read Scratchpad)
 *   (command writes and timed hold-offs never capture)
 *
 * Properties (each checked with abort()):
 * 1. Termination: every transaction finishes within POLL_GUARD polls,
 *    leaving the driver parked at IDLE with the txn finished.
 * 2. Reference model: ds18b20_last_command_ok(), the final ctx.resolution,
 *    the parasite flag and the copied out-buffer equal a from-scratch model
 *    of the transaction rules (presence -> abort without OK; Read Scratchpad
 *    -> resolution adapted only on a valid CRC; Read Power Supply -> parasite
 *    flag from the answer bit).
 * 3. Determinism: the same input reproduces the same outcome.
 */

#include "ds18b20.h"
#include "hw_model.h"
#include "mock_target.h"
#include "onewire.h"
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/* Single-TU: compile the driver + shared layer through the accessor TU so
 * the fuzz input drives the exact production state machines. */
#include "../mock/ds18b20_test_access.c"
#include "../mock/ds18b20_test_access.h"
#include <stdio.h>

#define POLL_GUARD 5000u
#define MAX_READ_BYTES 9u

static const uint8_t* g_data;
static size_t g_size;
static size_t g_cursor;

static uint8_t g_cmd;
static uint8_t g_addr_mode;
static uint8_t g_parasite;

/* Capture record consumed by the reference model in the driver's order. */
static uint8_t g_presence;
static uint8_t g_presence_seen;
static uint8_t g_cur_read[MAX_READ_BYTES];
static uint8_t g_read_seen;

static uint8_t next_bit(void) {
    if (g_size == 0u) {
        return 0u;
    }
    uint8_t b = g_data[g_cursor % g_size];
    g_cursor++;
    return (uint8_t)(b & 1u);
}

static uint8_t next_byte(void) {
    if (g_size == 0u) {
        return 0u;
    }
    uint8_t b = g_data[g_cursor % g_size];
    g_cursor++;
    return b;
}

static uint16_t fuzz_cap(uint32_t idx) {
    uint8_t rcr = (uint8_t)mock_tim1.RCR;
    if (rcr == 0) {
        /* Reset op: 2 captures in the single slot. */
        if (idx == 0u) {
            return 500u; /* valid reset pulse */
        }
        uint8_t p = next_bit();
        g_presence = p;
        g_presence_seen = 1u;
        return p ? 700u : 100u;
    }
    if (rcr > (uint8_t)(MAX_READ_BYTES * 8u - 1u)) {
        return 0u;
    }
    /* Read op: (RCR + 1) slots, one bit per slot (LSB first). */
    uint8_t bytes = (uint8_t)((rcr >> 3) + 1u);
    if (idx == 0u) {
        for (uint8_t b = 0; b < bytes; b++) {
            g_cur_read[b] = next_byte();
        }
        g_read_seen = 1u;
    }
    if (idx >= (uint32_t)bytes * 8u) {
        return 0u;
    }
    uint8_t bit = (uint8_t)((g_cur_read[idx >> 3] >> (idx & 7u)) & 1u);
    return bit ? (uint16_t)ONEWIRE_ONE_PULSE : (uint16_t)ONEWIRE_ZERO_PULSE;
}

/* Bytes the command reads back on the bus (Read ROM 8, Read Scratchpad 9,
 * Read Power Supply 1; the rest read nothing) and bytes the command copies
 * into the caller's result buffer (Read Power Supply consumes its byte
 * internally as the parasite flag and reports nothing). */
static uint8_t user_out_bytes(uint8_t cmd) {
    switch (cmd) {
    case 0u:
        return 8u;
    case 1u:
        return 9u;
    default:
        return 0u;
    }
}

/* ---- Reference model ---- */

typedef struct {
    uint8_t ok;
    uint8_t resolution;
    uint8_t parasite;
    uint8_t out_len;
    uint8_t out[MAX_READ_BYTES];
} txn_model_t;

static void model_run(txn_model_t* m) {
    memset(m, 0, sizeof(*m));
    m->resolution = DS18B20_RES_DEFAULT;
    m->parasite = g_parasite;

    if (!g_presence_seen) {
        fprintf(stderr, "model: no presence record\n");
        abort(); /* the transaction always schedules its reset */
    }
    if (g_presence == 0u) {
        m->ok = 0u;
        return;
    }
    m->ok = 1u;
    switch (g_cmd) {
    case 0u: /* Read ROM */
        if (!g_read_seen) {
            fprintf(stderr, "model: no read record (cmd0)\n");
            abort(); /* the driver read data the record cannot explain */
        }
        m->out_len = 8u;
        memcpy(m->out, g_cur_read, 8u);
        break;
    case 1u: /* Read Scratchpad */
        if (!g_read_seen) {
            fprintf(stderr, "model: no read record (cmd1)\n");
            abort();
        }
        m->out_len = 9u;
        memcpy(m->out, g_cur_read, 9u);
        if (g_cur_read[8] == onewire_crc8(g_cur_read, DS18B20_CRC8_BYTES)) {
            m->resolution = (uint8_t)(DS18B20_RES_MIN + ((g_cur_read[4] >> 5) & 0x3u));
        }
        break;
    case 2u: /* Write Scratchpad thresholds */
        break;
    case 3u: /* Copy Scratchpad */
    case 4u: /* Recall EEPROM */
        break;
    case 5u: /* Read Power Supply */
        if (!g_read_seen) {
            fprintf(stderr, "model: no read record (cmd5)\n");
            abort();
        }
        m->parasite = (uint8_t)((g_cur_read[0] & 0x01u) == 0u);
        break;
    default:
        fprintf(stderr, "model: bad cmd %u\n", (unsigned)g_cmd);
        abort();
    }
}

/* ---- Drive one transaction and record the driver outcome ---- */

typedef struct {
    uint8_t ok;
    uint8_t resolution;
    uint8_t parasite;
    uint8_t out_len;
    uint8_t out[MAX_READ_BYTES];
} txn_result_t;

static uint8_t g_out[MAX_READ_BYTES];

static void txn_setup(void) {
    hw_reset_all();
    ds18b20_test_register_buffers();
    hw_set_capture_source(fuzz_cap);
    ds18b20_test_reset_ctx();
    ds18b20_test_reset_search();
    ds18b20_test_reset_txn();
    ds18b20_test_reset_resolution();
    ds18b20_set_parasite(0);
    g_presence_seen = 0u;
    g_read_seen = 0u;
    memset(g_cur_read, 0x00, sizeof(g_cur_read));
    g_cursor = 0;
    memset(g_out, 0xAA, sizeof(g_out));

    if (g_parasite) {
        ds18b20_set_parasite(1);
    }
    ds18b20_test_set_address_mode(g_addr_mode);

    switch (g_cmd) {
    case 0u:
        ds18b20_read_rom(g_out);
        break;
    case 1u:
        ds18b20_read_scratchpad(g_out);
        break;
    case 2u:
        ds18b20_set_alarm_thresholds(0x55, 0x2A);
        break;
    case 3u:
        ds18b20_copy_scratchpad();
        break;
    case 4u:
        ds18b20_recall_eeprom();
        break;
    case 5u:
        ds18b20_detect_parasite();
        break;
    default:
        abort();
    }
}

static void txn_run(txn_result_t* out) {
    memset(out, 0, sizeof(*out));
    uint8_t done = 0u;
    uint16_t guard = 0;
    while (done == 0u) {
        switch (g_cmd) {
        case 0u:
            done = ds18b20_read_rom_poll();
            break;
        case 1u:
            done = ds18b20_read_scratchpad_poll();
            break;
        case 2u:
            done = ds18b20_set_alarm_thresholds_poll();
            break;
        case 3u:
            done = ds18b20_copy_scratchpad_poll();
            break;
        case 4u:
            done = ds18b20_recall_eeprom_poll();
            break;
        case 5u:
            done = ds18b20_detect_parasite_poll();
            break;
        default:
            abort();
        }
        if (done != 0u) {
            break;
        }
        if (mock_tim1.CR1 & TIM_CR1_CEN) {
            hw_run_until_uif(120); /* longest op = Match ROM command write */
        } else if ((mock_tim1.SR & TIM_SR_UIF) != 0u) {
            mock_tim1.SR = 0; /* stale completion, consumed by the next poll */
        }
        if (++guard > POLL_GUARD) {
            fprintf(stderr, "P1 livelock: state=%u CEN=%u done=%u pres=%u read=%u\n",
                    (unsigned)ds18b20_test_get_state(), (unsigned)(mock_tim1.CR1 & TIM_CR1_CEN),
                    (unsigned)done, (unsigned)g_presence_seen, (unsigned)g_read_seen);
            abort(); /* property 1: livelock */
        }
    }

    /* Transaction must end parked at IDLE with the finished flag set. */
    if (ds18b20_test_get_state() != DS18B20_ST_IDLE) {
        fprintf(stderr, "txn end: state=%u\n", (unsigned)ds18b20_test_get_state());
        abort();
    }
    if (ds18b20_test_get_txn_finished() == 0u) {
        fprintf(stderr, "txn end: not finished\n");
        abort();
    }

    out->ok = ds18b20_last_command_ok();
    out->resolution = ds18b20_get_resolution();
    out->parasite = ds18b20_parasite_mode();
    out->out_len = user_out_bytes(g_cmd);
    if (out->ok != 0u) {
        memcpy(out->out, g_out, out->out_len);
    } else {
        for (uint8_t i = 0; i < out->out_len; i++) {
            if (g_out[i] != 0xAA) {
                fprintf(stderr, "txn: aborted touched out[%u]=%02X\n", (unsigned)i, (unsigned)g_out[i]);
                abort(); /* aborted transactions must not touch the buffer */
            }
        }
    }
}

static void run_case(txn_result_t* out) {
    txn_setup();
    txn_run(out);

    /* Property 2: match the reference model. */
    txn_model_t model;
    model_run(&model);
    if (out->ok != model.ok) {
        fprintf(stderr, "P2 ok: driver=%u model=%u\n", (unsigned)out->ok, (unsigned)model.ok);
        abort();
    }
    if (out->resolution != model.resolution) {
        fprintf(stderr, "P2 res: driver=%u model=%u\n", (unsigned)out->resolution, (unsigned)model.resolution);
        abort();
    }
    if (out->parasite != model.parasite) {
        fprintf(stderr, "P2 para: driver=%u model=%u\n", (unsigned)out->parasite, (unsigned)model.parasite);
        abort();
    }
    if (out->ok != 0u && out->out_len != model.out_len) {
        fprintf(stderr, "P2 len: driver=%u model=%u\n", (unsigned)out->out_len, (unsigned)model.out_len);
        abort();
    }
    for (uint8_t i = 0; i < model.out_len; i++) {
        if (out->out[i] != model.out[i]) {
            fprintf(stderr, "P2 out[%u]: driver=%02X model=%02X\n", (unsigned)i, (unsigned)out->out[i], (unsigned)model.out[i]);
            abort();
        }
    }
}

int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    if (size < 1) {
        return 0;
    }
    g_data = data;
    g_size = size;
    g_cmd = (uint8_t)(data[0] % 6u);
    g_addr_mode = (uint8_t)((data[0] & 0x08u) != 0u);
    g_parasite = (uint8_t)((data[0] & 0x10u) != 0u);

    txn_result_t a;
    run_case(&a);

    /* Property 3: determinism — same stream, same outcome. */
    txn_result_t b;
    run_case(&b);

    if (a.ok != b.ok || a.resolution != b.resolution || a.parasite != b.parasite ||
        a.out_len != b.out_len) {
        fprintf(stderr, "P3 det: a=(%u,%u,%u,%u) b=(%u,%u,%u,%u)\n",
                (unsigned)a.ok, (unsigned)a.resolution, (unsigned)a.parasite, (unsigned)a.out_len,
                (unsigned)b.ok, (unsigned)b.resolution, (unsigned)b.parasite, (unsigned)b.out_len);
        abort();
    }
    for (uint8_t i = 0; i < a.out_len; i++) {
        if (a.out[i] != b.out[i]) {
            fprintf(stderr, "P3 det out[%u]: a=%02X b=%02X\n", (unsigned)i, (unsigned)a.out[i], (unsigned)b.out[i]);
            abort();
        }
    }
    return 0;
}

#ifndef FUZZING_BUILD_MODE_UNSAFE_FOR_PRODUCTION
static void txn_smoke(const uint8_t* in, size_t len) {
    /* Aborts on any property violation; the oracle inside runs twice. */
    (void)LLVMFuzzerTestOneInput(in, len);
}

int main(void) {
    uint8_t in[64];

    /* Read ROM: present device, 8 answer bytes. */
    memset(in, 0, sizeof(in));
    in[0] = 0x00; /* cmd 0, Skip ROM, external power */
    in[1] = 0x01; /* presence on */
    for (uint8_t i = 0; i < 8u; i++) {
        in[2 + i] = (uint8_t)(0x28u + i);
    }
    txn_smoke(in, 2u + 8u);

    /* Read ROM: absent device -> aborted, buffer untouched. */
    memset(in, 0, sizeof(in));
    in[0] = 0x00;
    in[1] = 0x00;
    txn_smoke(in, 2u);

    /* Read Scratchpad: valid CRC adapts the resolution to 10 bit. */
    memset(in, 0, sizeof(in));
    in[0] = 0x01; /* cmd 1 */
    in[1] = 0x01;
    {
        uint8_t sp[DS18B20_SCRATCHPAD_LEN];
        memset(sp, 0, sizeof(sp));
        sp[4] = 0x20; /* R1:R0 = 01 -> 10 bit */
        sp[5] = 0xFF;
        sp[7] = 0x10;
        sp[8] = onewire_crc8(sp, DS18B20_CRC8_BYTES);
        memcpy(in + 2u, sp, sizeof(sp));
    }
    txn_smoke(in, 2u + DS18B20_SCRATCHPAD_LEN);

    /* Read Power Supply: answer bit 0 -> parasite mode on. */
    memset(in, 0, sizeof(in));
    in[0] = 0x05 | 0x10; /* cmd 5, parasite declared off initially */
    in[1] = 0x01;
    in[2] = 0x00; /* raw bit 0 -> parasite */
    txn_smoke(in, 3u);

    /* Copy Scratchpad: hold-off, no data. */
    memset(in, 0, sizeof(in));
    in[0] = 0x03;
    in[1] = 0x01;
    txn_smoke(in, 2u);

    /* Differential walk over all commands. */
    {
        uint32_t seed = 0x9E3779B9u;
        uint8_t buf[40];
        for (unsigned iter = 0; iter < 20000u; iter++) {
            seed ^= seed << 13;
            seed ^= seed >> 17;
            seed ^= seed << 5;
            size_t len = 2u + (size_t)(seed % (uint32_t)(sizeof(buf) - 2u));
            for (size_t i = 0; i < len; i++) {
                seed ^= seed << 13;
                seed ^= seed >> 17;
                seed ^= seed << 5;
                buf[i] = (uint8_t)((seed >> 24) ^ (seed >> 16));
            }
            txn_smoke(buf, len);
        }
    }

    return 0;
}
#endif