/*
 * libFuzzer harness for the DS2431 command-transaction engine
 * (ds2431_read_memory / _write_scratchpad / _read_scratchpad / _copy_scratchpad
 * + *_poll).
 *
 * Compiled as single-TU via ds2431_test_access.c (src/ds2431.c +
 * src/onewire.c); the transactions report through ds2431_last_command_ok(),
 * ds2431_parasite_mode() and the caller's result buffer.
 *
 * Input layout:
 *   data[0] & 0x03 -> command index 0..3:
 *       0 Read Memory  (0xF0, reads len 1..8 bytes)
 *       1 Write Scratchpad (0x0F, no response; latches a pending write cycle)
 *       2 Read Scratchpad (0xAA, reads an 13-byte reply: 11-byte frame +
 *          inverted CRC-16, which must verify; optional TA/ES echo check when
 *          the pending-write state is armed)
 *       3 Copy Scratchpad (0x55, timed t_PROG hold-off; requires a pending
 *          write cycle, which this harness always arms)
 *   data[0] & 0x04 -> Match ROM addressing (Skip ROM otherwise)
 *   data[0] & 0x08 -> parasite power (ds2431_set_parasite(1))
 *   data[0] & 0x10 -> (cmd 2) arm the pending-write state so the frame read
 *                     echo-verifies TA/ES
 * plus the stream bytes: the per-command parameters (address, length, payload,
 * pending TA/ES) are consumed first, then one presence bit and the read
 * response bytes via the capture source.
 *
 * The capture source decodes the running operation from mock_tim1.RCR:
 *   rcr == 0      -> reset op (idx0 reset pulse, idx1 presence)
 *   rcr == 7..63  -> len-byte Read Memory read (len = (rcr>>3)+1)
 *   rcr == 103    -> 13-byte Read Scratchpad reply read (11-byte frame + CRC-16)
 *   (command writes and the Copy Scratchpad hold-off never capture)
 *
 * Properties (each checked with abort()):
 * 1. Termination: every transaction finishes within POLL_GUARD polls, leaving
 *    the driver finished with the timer handed back.
 * 2. Reference model: ds2431_last_command_ok(), the parasite flag and the
 *    copied out-buffer equal a from-scratch model of the transaction rules
 *    (no presence -> abort without OK and an untouched buffer; scratchpad
 *    frame read -> OK only when the trailing CRC-16 verifies and the TA/ES
 *    echo matches the latched write).
 * 3. Determinism: the same input reproduces the same outcome.
 */

#include "ds2431.h"
#include "hw_model.h"
#include "mock_target.h"
#include "onewire.h"
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/* Single-TU: compile the driver + shared layer through the accessor TU so
 * the fuzz input drives the exact production state machines. */
#include "../mock/ds2431_test_access.c"
#include "../mock/ds2431_test_access.h"
#include <stdio.h>

#define POLL_GUARD 5000u
#define MAX_READ_BYTES 13u /* Read Scratchpad reads the 11-byte frame + CRC-16 */
#define MAX_PAYLOAD 8u

static const uint8_t* g_data;
static size_t g_size;
static size_t g_cursor;

static uint8_t g_cmd;
static uint8_t g_addr_mode;
static uint8_t g_parasite;
static uint8_t g_pending_arm;

/* Per-command parameters latched by the setup from the stream. */
static uint8_t g_len;
static uint8_t g_alo;
static uint8_t g_ahi;
static uint8_t g_es;
static uint8_t g_pending;
static uint8_t g_payload[MAX_PAYLOAD];

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

/* Bytes the command copies into the caller's result buffer (Read Memory len,
 * Read Scratchpad 11; the rest report nothing). */
static uint8_t user_out_bytes(uint8_t cmd) {
    if (cmd == 0u) {
        return g_len;
    }
    if (cmd == 2u) {
        return DS2431_SCRATCH_FRAME;
    }
    return 0u;
}

/* ---- Reference model ---- */

typedef struct {
    uint8_t ok;
    uint8_t parasite;
    uint8_t out_len;
    uint8_t out[MAX_READ_BYTES];
} ds_model_t;

static void model_run(ds_model_t* m) {
    memset(m, 0, sizeof(*m));
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
    case 0u: /* Read Memory */
        if (!g_read_seen) {
            fprintf(stderr, "model: no read record (cmd0)\n");
            abort(); /* the driver read data the record cannot explain */
        }
        m->out_len = g_len;
        memcpy(m->out, g_cur_read, g_len);
        break;
    case 1u: /* Write Scratchpad */
        break;
    case 2u: /* Read Scratchpad */
        if (!g_read_seen) {
            fprintf(stderr, "model: no read record (cmd2)\n");
            abort();
        }
        {
            /* The reply's trailing inverted CRC-16 must cover command + frame. */
            uint8_t crc_in[DS2431_SCRATCH_FRAME + 1];
            crc_in[0] = DS2431_READ_SCRATCHPAD;
            memcpy(&crc_in[1], g_cur_read, DS2431_SCRATCH_FRAME);
            uint16_t expected = (uint16_t)(~onewire_crc16(crc_in, DS2431_SCRATCH_FRAME + 1));
            uint16_t got = (uint16_t)((uint16_t)g_cur_read[DS2431_SCRATCH_FRAME] |
                                      ((uint16_t)g_cur_read[DS2431_SCRATCH_FRAME + 1] << 8));
            if (got != expected) {
                m->ok = 0u; /* CRC mismatch: the scratchpad read is corrupt */
                break;
            }
        }
        if (g_pending != 0u &&
            (g_cur_read[0] != g_alo || g_cur_read[1] != g_ahi ||
             (uint8_t)(g_cur_read[2] & 0x07u) != g_es)) {
            m->ok = 0u; /* echo mismatch: the write did not reach this device */
            break;
        }
        m->out_len = DS2431_SCRATCH_FRAME;
        memcpy(m->out, g_cur_read, DS2431_SCRATCH_FRAME);
        break;
    case 3u: /* Copy Scratchpad */
        break;
    default:
        fprintf(stderr, "model: bad cmd %u\n", (unsigned)g_cmd);
        abort();
    }
}

/* ---- Drive one transaction and record the driver outcome ---- */

typedef struct {
    uint8_t ok;
    uint8_t parasite;
    uint8_t out_len;
    uint8_t out[MAX_READ_BYTES];
} ds_result_t;

static uint8_t g_out[MAX_READ_BYTES];

static void txn_setup(void) {
    hw_reset_all();
    ds2431_test_register_buffers();
    hw_set_capture_source(fuzz_cap);
    ds2431_test_reset_ctx();
    ds2431_test_reset_search();
    ds2431_set_parasite(0);
    g_presence_seen = 0u;
    g_read_seen = 0u;
    memset(g_cur_read, 0x00, sizeof(g_cur_read));
    g_cursor = 0;
    memset(g_out, 0xAA, sizeof(g_out));

    /* Latch the per-command parameters from the stream first, so the capture
     * source (presence/read) consumes the bytes that follow. */
    g_pending = 0u;
    switch (g_cmd) {
    case 0u: {
        uint8_t addr = (uint8_t)(next_byte() % DS2431_EEPROM_SIZE);
        uint8_t len = (uint8_t)(1u + (next_byte() % DS2431_READ_PASS));
        if ((uint16_t)addr + len > DS2431_EEPROM_SIZE) {
            len = (uint8_t)(DS2431_EEPROM_SIZE - addr);
        }
        g_len = len;
        g_alo = addr;
        g_ahi = 0u;
        break;
    }
    case 1u: {
        uint8_t addr = (uint8_t)((next_byte() % 16u) * 8u);
        uint8_t len = (uint8_t)(1u + (next_byte() % DS2431_SCRATCH_SIZE));
        if ((uint16_t)addr + len > DS2431_EEPROM_SIZE) {
            len = (uint8_t)(DS2431_EEPROM_SIZE - addr);
        }
        g_len = len;
        g_alo = addr;
        g_ahi = 0u;
        g_es = (uint8_t)((addr + len - 1u) & 0x07u);
        for (uint8_t i = 0; i < len; i++) {
            g_payload[i] = next_byte();
        }
        break;
    }
    case 2u:
        if (g_pending_arm != 0u) {
            uint8_t addr = (uint8_t)(next_byte() % DS2431_EEPROM_SIZE);
            g_alo = addr;
            g_ahi = 0u;
            g_es = (uint8_t)(next_byte() & 0x07u);
            g_pending = 1u;
        }
        break;
    case 3u: {
        uint8_t addr = (uint8_t)((next_byte() % 16u) * 8u);
        g_alo = addr;
        g_ahi = 0u;
        g_es = (uint8_t)(next_byte() & 0x07u);
        g_pending = 1u;
        break;
    }
    default:
        abort();
    }

    /* Select addressing (Match ROM clears any pending state), then arm the
     * pending write cycle that Read Scratchpad / Copy Scratchpad need. */
    if (g_addr_mode != 0u) {
        static const uint8_t rom[DS2431_ROM_BYTES] = {0x01u, 0x23u, 0x45u, 0x67u,
                                                      0x89u, 0xABu, 0xCDu, 0xEFu};
        ds2431_select(rom);
    } else {
        ds2431_select(0);
    }

    if (g_parasite) {
        ds2431_set_parasite(1);
    }

    switch (g_cmd) {
    case 0u:
        ds2431_read_memory(g_out, (uint16_t)g_alo, g_len);
        break;
    case 1u:
        ds2431_write_scratchpad((uint16_t)g_alo, g_payload, g_len);
        break;
    case 2u:
        if (g_pending != 0u) {
            ds2431_test_set_pending(g_alo, g_ahi, g_es);
        }
        ds2431_read_scratchpad(g_out);
        break;
    case 3u:
        ds2431_test_set_pending(g_alo, g_ahi, g_es);
        ds2431_copy_scratchpad();
        break;
    default:
        abort();
    }
}

static uint8_t poll_cmd(void) {
    switch (g_cmd) {
    case 0u:
        return ds2431_read_memory_poll();
    case 1u:
        return ds2431_write_scratchpad_poll();
    case 2u:
        return ds2431_read_scratchpad_poll();
    case 3u:
        return ds2431_copy_scratchpad_poll();
    default:
        abort();
    }
}

static void txn_run(ds_result_t* out) {
    memset(out, 0, sizeof(*out));
    uint8_t done = 0u;
    uint16_t guard = 0;
    while (done == 0u) {
        done = poll_cmd();
        if (done != 0u) {
            break;
        }
        if (mock_tim1.CR1 & TIM_CR1_CEN) {
            hw_run_until_uif(256); /* longest op = Match ROM command write (160) */
        } else if ((mock_tim1.SR & TIM_SR_UIF) != 0u) {
            mock_tim1.SR = 0; /* stale completion, consumed by the next poll */
        }
        if (++guard > POLL_GUARD) {
            fprintf(stderr, "P1 livelock: cmd=%u CEN=%u done=%u pres=%u read=%u\n",
                    (unsigned)g_cmd, (unsigned)(mock_tim1.CR1 & TIM_CR1_CEN), (unsigned)done,
                    (unsigned)g_presence_seen, (unsigned)g_read_seen);
            abort(); /* property 1: livelock */
        }
    }

    /* The transaction must end finished with the timer handed back. */
    if (ds2431_test_is_finished() == 0u) {
        fprintf(stderr, "txn end: not finished\n");
        abort();
    }

    out->ok = ds2431_last_command_ok();
    out->parasite = ds2431_parasite_mode();
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

static void run_case(ds_result_t* out) {
    txn_setup();
    txn_run(out);

    /* Property 2: match the reference model. */
    ds_model_t model;
    model_run(&model);
    if (out->ok != model.ok) {
        fprintf(stderr, "P2 ok: driver=%u model=%u\n", (unsigned)out->ok, (unsigned)model.ok);
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
    g_cmd = (uint8_t)(data[0] % 4u);
    g_addr_mode = (uint8_t)((data[0] & 0x04u) != 0u);
    g_parasite = (uint8_t)((data[0] & 0x08u) != 0u);
    g_pending_arm = (uint8_t)((data[0] & 0x10u) != 0u);

    ds_result_t a;
    run_case(&a);

    /* Property 3: determinism — same stream, same outcome. */
    ds_result_t b;
    run_case(&b);

    if (a.ok != b.ok || a.parasite != b.parasite || a.out_len != b.out_len) {
        fprintf(stderr, "P3 det: a=(%u,%u,%u) b=(%u,%u,%u)\n", (unsigned)a.ok, (unsigned)a.parasite,
                (unsigned)a.out_len, (unsigned)b.ok, (unsigned)b.parasite, (unsigned)b.out_len);
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
/* Append the DS2431's inverted CRC-16 after an 11-byte frame at @p frame. */
static void set_reply_crc(uint8_t* frame) {
    uint8_t crc_in[DS2431_SCRATCH_FRAME + 1];
    crc_in[0] = DS2431_READ_SCRATCHPAD;
    memcpy(&crc_in[1], frame, DS2431_SCRATCH_FRAME);
    uint16_t crc = (uint16_t)(~onewire_crc16(crc_in, DS2431_SCRATCH_FRAME + 1));
    frame[DS2431_SCRATCH_FRAME] = (uint8_t)(crc & 0xffu);
    frame[DS2431_SCRATCH_FRAME + 1] = (uint8_t)(crc >> 8);
}

static void txn_smoke(const uint8_t* in, size_t len) {
    /* Aborts on any property violation; the oracle inside runs twice. */
    (void)LLVMFuzzerTestOneInput(in, len);
}

int main(void) {
    uint8_t in[80];

    /* Read Memory: present device, 4 answer bytes. */
    memset(in, 0, sizeof(in));
    in[0] = 0x00; /* cmd 0, Skip ROM, external power */
    in[1] = 0x10; /* addr 0x10 */
    in[2] = 0x03; /* len = 1 + 3%8 = 4 */
    in[3] = 0x01; /* presence on */
    for (uint8_t i = 0; i < 4u; i++) {
        in[4 + i] = (uint8_t)(0xC0u + i);
    }
    txn_smoke(in, 4u + 4u);

    /* Read Memory: absent device -> aborted, buffer untouched. */
    memset(in, 0, sizeof(in));
    in[0] = 0x00;
    in[1] = 0x10;
    in[2] = 0x03;
    in[3] = 0x00; /* presence off */
    txn_smoke(in, 4u);

    /* Write Scratchpad: present, no read-back. */
    memset(in, 0, sizeof(in));
    in[0] = 0x01; /* cmd 1 */
    in[1] = 0x02; /* addr = 2*8 = 0x10 */
    in[2] = 0x07; /* len = 1 + 7%8 = 8 */
    in[3] = 0xDE;
    in[4] = 0xAD;
    in[5] = 0xBE;
    in[6] = 0xEF;
    in[7] = 0x11;
    in[8] = 0x22;
    in[9] = 0x33;
    in[10] = 0x44;
    in[11] = 0x01; /* presence on */
    txn_smoke(in, 12u);

    /* Read Scratchpad: pending armed, matching TA/ES echo, valid CRC -> OK. */
    memset(in, 0, sizeof(in));
    in[0] = 0x02 | 0x10; /* cmd 2, arm pending */
    in[1] = 0x10; /* pending addr */
    in[2] = 0x05; /* pending ES = 5 */
    in[3] = 0x01; /* presence on */
    in[4] = 0x10; /* frame TA1 matches */
    in[5] = 0x00; /* frame TA2 matches */
    in[6] = 0x05; /* frame ES matches */
    for (uint8_t i = 7u; i < 7u + 8u; i++) {
        in[i] = (uint8_t)(0xA0u + i);
    }
    set_reply_crc(&in[4]); /* append the matching inverted CRC-16 */
    txn_smoke(in, 4u + DS2431_SCRATCH_FRAME_CRC);

    /* Read Scratchpad: pending armed, mismatching echo (valid CRC) -> no OK. */
    memset(in, 0, sizeof(in));
    in[0] = 0x02 | 0x10;
    in[1] = 0x10;
    in[2] = 0x05;
    in[3] = 0x01;
    in[4] = 0x20; /* frame TA1 differs */
    in[5] = 0x00;
    in[6] = 0x05;
    for (uint8_t i = 7u; i < 7u + 8u; i++) {
        in[i] = (uint8_t)(0xA0u + i);
    }
    set_reply_crc(&in[4]);
    txn_smoke(in, 4u + DS2431_SCRATCH_FRAME_CRC);

    /* Read Scratchpad: matching echo but corrupted CRC -> no OK. */
    memset(in, 0, sizeof(in));
    in[0] = 0x02 | 0x10;
    in[1] = 0x10;
    in[2] = 0x05;
    in[3] = 0x01;
    in[4] = 0x10;
    in[5] = 0x00;
    in[6] = 0x05;
    for (uint8_t i = 7u; i < 7u + 8u; i++) {
        in[i] = (uint8_t)(0xA0u + i);
    }
    set_reply_crc(&in[4]);
    in[4 + DS2431_SCRATCH_FRAME] ^= 0xFFu; /* break the trailing CRC */
    txn_smoke(in, 4u + DS2431_SCRATCH_FRAME_CRC);

    /* Copy Scratchpad: parasite, timed hold-off, no data. */
    memset(in, 0, sizeof(in));
    in[0] = 0x03 | 0x08; /* cmd 3, parasite */
    in[1] = 0x04;
    in[2] = 0x00;
    in[3] = 0x01; /* presence on */
    txn_smoke(in, 4u);

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
