/* ============================================================
 *  test_ds2431.c - DS2431 EEPROM driver tests
 *
 *  Covers the non-blocking DS2431 operations on top of the shared
 *  1-Wire layer:
 *   - Read Memory (0xF0): command build (Skip/Match ROM), read data
 *     path, presence abort, pass-size and range guarding
 *   - Write Scratchpad (0x0F): command build, latch of TA + ES
 *   - Read Scratchpad (0xAA): 11-byte frame data path, TA/ES echo
 *     verification (pass + mismatch), read-back data verification, raw read
 *     without a pending write
 *   - Copy Scratchpad (0x55): t_PROG 11ms hold-off, requires a pending
 *     write
 *   - full write cycle end-to-end (write -> verify -> copy)
 *
 *  Command-table verification: the shared feed DMA sources the CCR3
 *  reloads from &ctx.u.cmd[1] (the first pulse is applied to CCR3 by
 *  software), and the mock's feed log records exactly those transfers -
 *  so log entry k holds the slot k+1 pulse. The log is wiped at the
 *  start of the *next* hardware operation, so for operations that end
 *  with a read or hold-off stage the command bytes are asserted between
 *  the write and that stage (drive_ops with 2 operations).
 * ============================================================ */

#include "ds2431.h"
#include "ds2431_test_access.h"
#include "hw_model.h"
#include "mock_target.h"
#include "onewire.h"
#include "unity.h"
#include <string.h>

#define ONE ONEWIRE_ONE_PULSE
#define ZERO ONEWIRE_ZERO_PULSE

/*-------------------------------------------------------------
 *  Shared helpers
 * -----------------------------------------------------------*/

static uint16_t de_capture_present(uint32_t idx) {
    return idx == 0 ? 510u : 700u;
}

static uint16_t de_capture_absent(uint32_t idx) {
    (void)idx;
    return 100u;
}

static uint16_t de_read_pulses[DS2431_SCRATCH_FRAME_CRC * DS2431_BITS_PER_BYTE];
static uint16_t de_read_capture(uint32_t idx) { return de_read_pulses[idx]; }

/* Encode len bytes (LSB-first) into the per-bit pulse buffer. */
static void set_read_bytes(const uint8_t* data, uint8_t len) {
    for (uint8_t i = 0; i < len; i++) {
        for (uint8_t b = 0; b < DS2431_BITS_PER_BYTE; b++) {
            de_read_pulses[i * DS2431_BITS_PER_BYTE + b] =
                ((data[i] >> b) & 1u) ? ONE : ZERO;
        }
    }
}

/* CRC-16 the DS2431 appends to a Read Scratchpad reply: the bitwise
 * complement of the Maxim CRC-16 over the command (0xAA) + 3 + n reply bytes. */
static uint16_t reply_crc16(const uint8_t* frame, uint8_t n) {
    uint8_t buf[1 + 3 + DS2431_SCRATCH_SIZE];
    buf[0] = DS2431_READ_SCRATCHPAD;
    memcpy(&buf[1], frame, (size_t)(3u + n));
    return (uint16_t)(~onewire_crc16(buf, (uint16_t)(1u + 3u + n)));
}

/* Feed a Read Scratchpad reply of n data bytes: the frame + inverted CRC-16
 * followed by 1s up to the 13-byte wire window (the device stops driving after
 * the CRC, so the driver reads logic 1 in the unused slots). */
static void set_scratchpad_reply_n(const uint8_t* frame, uint8_t n) {
    uint8_t wire[DS2431_SCRATCH_FRAME_CRC];
    memset(wire, 0xFF, sizeof(wire));
    memcpy(wire, frame, (size_t)(3u + n));
    uint16_t crc = reply_crc16(frame, n);
    wire[3u + n] = (uint8_t)(crc & 0xffu);
    wire[3u + n + 1u] = (uint8_t)(crc >> 8);
    set_read_bytes(wire, DS2431_SCRATCH_FRAME_CRC);
}

/* Feed a full 8-byte-window reply (13-byte wire, ES = 7). */
static void set_scratchpad_reply(const uint8_t* frame11) {
    set_scratchpad_reply_n(frame11, DS2431_SCRATCH_SIZE);
}

/* Step exactly one hardware operation (reset/write/read/hold-off): the
 * capture channel re-routes a frame read (> OW_PORT_CAPTURE_BUF_SIZE
 * transfers) to the read pulse source, everything else stays on the
 * presence source the test installed. */
static void run_current_op(void) {
    if (mock_tim1.CR1 & TIM_CR1_CEN) {
        if ((mock_dma1_ch4.CCR & DMA_CCR_EN) &&
            MOCK_CAP_TRANSFERS(mock_dma1_ch4.CNDTR) > OW_PORT_CAPTURE_BUF_SIZE) {
            hw_set_capture_source(de_read_capture);
        }
        TEST_ASSERT_TRUE(hw_run_until_uif(256));
    }
}

/* Drive poll() until the operation finishes; guard against a wedged loop. */
static void drive(uint8_t (*poll)(void)) {
    uint16_t guard = 0;
    for (;;) {
        if (poll()) {
            break;
        }
        run_current_op();
        if (++guard > 500) {
            break;
        }
    }
    TEST_ASSERT_TRUE(guard <= 500);
}

/* Drive poll() until `nops` hardware operations have completed. The
 * operation itself may still be running when this returns - use it to
 * step the txn engine to a stage boundary (e.g. past the command write)
 * before a trailing stage wipes the model's feed log. */
static void drive_ops(uint8_t (*poll)(void), uint8_t nops) {
    uint16_t guard = 0;
    uint8_t done = 0;
    while (done < nops && guard <= 500) {
        if (poll()) {
            done = nops; /* finished early (presence abort) */
        } else {
            run_current_op(); /* exactly one hardware operation */
            done++;
        }
        guard++;
    }
    TEST_ASSERT_TRUE(guard <= 500);
}

/* Assert the last completed command write transmitted exactly these bytes
 * (Skip/Match ROM prefix + command + TA + payload), LSB-first. Valid when
 * the command write is the last completed hardware operation, so the model
 * still holds its feed log: slot 0 is covered from the driver's table (it
 * is preloaded into CCR3 by software, not fed), slots 1..slots from the
 * feed log (the final transfer rides ONEWIRE_RELEASE_PULSE). */
static void assert_cmd_bytes(const uint8_t* bytes, uint8_t nbytes) {
    const hw_ccr3_feed_log_t* log = hw_ccr3_feed_log();
    const uint16_t slots = (uint16_t)(nbytes * DS2431_BITS_PER_BYTE);
    TEST_ASSERT_TRUE(slots <= 128u);
    TEST_ASSERT_EQUAL_UINT16(slots, log->count);
    for (uint8_t i = 0; i < nbytes; i++) {
        for (uint8_t b = 0; b < DS2431_BITS_PER_BYTE; b++) {
            const uint16_t want = ((bytes[i] >> b) & 1u) ? ONE : ZERO;
            const uint8_t s = (uint8_t)(i * DS2431_BITS_PER_BYTE + b);
            if (s == 0) {
                TEST_ASSERT_EQUAL_UINT16(want, ds2431_test_get_cmd_pulse(0));
            } else {
                TEST_ASSERT_EQUAL_UINT16(want, log->values[s - 1]);
            }
        }
    }
    /* The trailing release pulse rode the final feed transfer. */
    TEST_ASSERT_EQUAL_UINT16(ONEWIRE_RELEASE_PULSE, log->values[slots - 1]);
}

/*-------------------------------------------------------------
 *  1. Driver initial state
 * -----------------------------------------------------------*/

void test_ds2431_init_state(void) {
    /* Poll before any operation reports "finished" (nothing to run) with the
     * success flag clear. */
    TEST_ASSERT_EQUAL_UINT8(1, ds2431_read_memory_poll());
    TEST_ASSERT_EQUAL_UINT8(0, ds2431_last_command_ok());
}

/*-------------------------------------------------------------
 *  1b. CRC-16 primitive
 * -----------------------------------------------------------*/

void test_crc16_maxim_vector(void) {
    /* CRC-16/ARC check value: Maxim 1-Wire CRC-16 of "123456789" = 0xBB3D. */
    const uint8_t v[] = "123456789";
    TEST_ASSERT_EQUAL_HEX16(0xBB3D, onewire_crc16(v, 9));
    /* Empty input returns the initial value (0). */
    TEST_ASSERT_EQUAL_HEX16(0x0000, onewire_crc16(v, 0));
}

/*-------------------------------------------------------------
 *  2. Read Memory
 * -----------------------------------------------------------*/

void test_read_memory_skip_rom(void) {
    static const uint8_t k_bytes[] = {0xCC, 0xF0, 0x40, 0x00};
    uint8_t buf[8];
    const uint8_t want[8] = {0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08};
    set_read_bytes(want, 8);
    hw_set_capture_source(de_capture_present);
    ds2431_read_memory(buf, 0x40, 8);
    drive_ops(ds2431_read_memory_poll, 2); /* reset + command write */
    assert_cmd_bytes(k_bytes, sizeof(k_bytes));
    drive(ds2431_read_memory_poll); /* finish the read */
    TEST_ASSERT_EQUAL_UINT8(1, ds2431_last_command_ok());
    for (uint8_t i = 0; i < 8; i++) {
        TEST_ASSERT_EQUAL_UINT8(want[i], buf[i]);
    }
}

void test_read_memory_match_rom(void) {
    const uint8_t rom[DS2431_ROM_BYTES] = {0x2D, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07};
    static const uint8_t k_bytes[] = {0x55, 0x2D, 0x01, 0x02, 0x03, 0x04,
                                      0x05, 0x06, 0x07, 0xF0, 0x05, 0x00};
    uint8_t buf[4];
    const uint8_t want[4] = {0xAB, 0xCD, 0xEF, 0x01};
    ds2431_select(rom);
    set_read_bytes(want, 4);
    hw_set_capture_source(de_capture_present);
    ds2431_read_memory(buf, 0x05, 4);
    drive_ops(ds2431_read_memory_poll, 2); /* reset + command write */
    assert_cmd_bytes(k_bytes, sizeof(k_bytes));
    drive(ds2431_read_memory_poll); /* finish the read */
    TEST_ASSERT_EQUAL_UINT8(1, ds2431_last_command_ok());
    for (uint8_t i = 0; i < 4; i++) {
        TEST_ASSERT_EQUAL_UINT8(want[i], buf[i]);
    }
}

void test_read_memory_no_presence_aborts(void) {
    uint8_t buf[8] = {0};
    hw_set_capture_source(de_capture_absent);
    ds2431_read_memory(buf, 0x00, 8);
    drive(ds2431_read_memory_poll);
    TEST_ASSERT_EQUAL_UINT8(0, ds2431_last_command_ok());
    TEST_ASSERT_EQUAL_UINT8(0, hw_ccr3_feed_log()->count);
    for (uint8_t i = 0; i < 8; i++) {
        TEST_ASSERT_EQUAL_UINT8(0, buf[i]);
    }
}

void test_read_memory_out_of_range_ignored(void) {
    uint8_t buf[8] = {0};
    hw_set_capture_source(de_capture_present);
    ds2431_read_memory(buf, 0x00, 9); /* len > DS2431_READ_PASS */
    drive(ds2431_read_memory_poll);
    TEST_ASSERT_EQUAL_UINT8(0, hw_ccr3_feed_log()->count);

    ds2431_read_memory(buf, 0x7F, 2); /* addr + len > EEPROM_SIZE */
    drive(ds2431_read_memory_poll);
    TEST_ASSERT_EQUAL_UINT8(0, hw_ccr3_feed_log()->count);
}

void test_read_memory_two_passes(void) {
    /* A 16-byte region is read in two DS2431_READ_PASS-byte passes. */
    uint8_t buf[16];
    uint8_t addr = 0x00;
    for (uint8_t pass = 0; pass < 2; pass++) {
        const uint8_t want[8] = {(uint8_t)(pass * 8), (uint8_t)(pass * 8 + 1),
                                 (uint8_t)(pass * 8 + 2), (uint8_t)(pass * 8 + 3),
                                 (uint8_t)(pass * 8 + 4), (uint8_t)(pass * 8 + 5),
                                 (uint8_t)(pass * 8 + 6), (uint8_t)(pass * 8 + 7)};
        set_read_bytes(want, 8);
        hw_set_capture_source(de_capture_present);
        ds2431_read_memory(&buf[pass * 8], addr, 8);
        drive(ds2431_read_memory_poll);
        TEST_ASSERT_EQUAL_UINT8(1, ds2431_last_command_ok());
        for (uint8_t i = 0; i < 8; i++) {
            TEST_ASSERT_EQUAL_UINT8(want[i], buf[pass * 8 + i]);
        }
        addr = (uint8_t)(addr + 8);
    }
}

/*-------------------------------------------------------------
 *  3. Write Scratchpad
 * -----------------------------------------------------------*/

void test_write_scratchpad_skip_rom(void) {
    static const uint8_t k_bytes[] = {0xCC, 0x0F, 0x40, 0x00,
                                      0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88};
    hw_set_capture_source(de_capture_present);
    ds2431_write_scratchpad(0x40, &k_bytes[4], 8);
    drive(ds2431_write_scratchpad_poll);
    TEST_ASSERT_EQUAL_UINT8(1, ds2431_last_command_ok());
    assert_cmd_bytes(k_bytes, sizeof(k_bytes));
}

void test_write_scratchpad_match_rom(void) {
    const uint8_t rom[DS2431_ROM_BYTES] = {0x2D, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07};
    static const uint8_t k_bytes[] = {0x55, 0x2D, 0x01, 0x02, 0x03, 0x04,
                                      0x05, 0x06, 0x07, 0x0F, 0x00, 0x00, 0xDE};
    const uint8_t data[1] = {0xDE};
    ds2431_select(rom);
    hw_set_capture_source(de_capture_present);
    ds2431_write_scratchpad(0x00, data, 1);
    drive(ds2431_write_scratchpad_poll);
    TEST_ASSERT_EQUAL_UINT8(1, ds2431_last_command_ok());
    assert_cmd_bytes(k_bytes, sizeof(k_bytes));
}

void test_write_scratchpad_no_presence_aborts(void) {
    hw_set_capture_source(de_capture_absent);
    const uint8_t data[8] = {0};
    ds2431_write_scratchpad(0x40, data, 8);
    drive(ds2431_write_scratchpad_poll);
    TEST_ASSERT_EQUAL_UINT8(0, ds2431_last_command_ok());
    TEST_ASSERT_EQUAL_UINT8(0, hw_ccr3_feed_log()->count);
}

void test_write_scratchpad_out_of_range_ignored(void) {
    const uint8_t data[8] = {0};
    hw_set_capture_source(de_capture_present);
    ds2431_write_scratchpad(0x40, data, 9); /* len > scratchpad size */
    drive(ds2431_write_scratchpad_poll);
    TEST_ASSERT_EQUAL_UINT8(0, hw_ccr3_feed_log()->count);

    ds2431_write_scratchpad(0x7F, data, 4); /* addr + len > EEPROM_SIZE */
    drive(ds2431_write_scratchpad_poll);
    TEST_ASSERT_EQUAL_UINT8(0, hw_ccr3_feed_log()->count);
}

void test_write_scratchpad_short_write(void) {
    /* A shorter-than-block write leaves the rest of the 8-byte scratchpad
     * stale. The device accepts it and latches TA + ES, but sets PF and
     * blocks Copy Scratchpad (datasheet: a valid copy needs 8 bytes from a
     * row boundary). ES for a 4-byte write at 0x08 is
     * (0x08 + 4 - 1) & 0x07 = 3. */
    static const uint8_t k_bytes[] = {0xCC, 0x0F, 0x08, 0x00, 0x0A, 0x0B, 0x0C, 0x0D};
    hw_set_capture_source(de_capture_present);
    ds2431_write_scratchpad(0x08, &k_bytes[4], 4);
    drive(ds2431_write_scratchpad_poll);
    TEST_ASSERT_EQUAL_UINT8(1, ds2431_last_command_ok());
    assert_cmd_bytes(k_bytes, sizeof(k_bytes));
}

void test_write_scratchpad_returns_status(void) {
    /* The start call reports accept (1) / reject (0) instead of silently
     * swallowing an out-of-range or misaligned request. */
    const uint8_t data[8] = {1, 2, 3, 4, 5, 6, 7, 8};
    hw_set_capture_source(de_capture_present);
    TEST_ASSERT_EQUAL_UINT8(1, ds2431_write_scratchpad(0x40, data, 8));
    drive(ds2431_write_scratchpad_poll);
    TEST_ASSERT_EQUAL_UINT8(1, ds2431_last_command_ok());

    TEST_ASSERT_EQUAL_UINT8(0, ds2431_write_scratchpad(0x40, 0, 8)); /* NULL data */
    TEST_ASSERT_EQUAL_UINT8(0, ds2431_write_scratchpad(0x40, data, 0)); /* len too small */
    TEST_ASSERT_EQUAL_UINT8(0, ds2431_write_scratchpad(0x40, data, 9)); /* len too large */
    TEST_ASSERT_EQUAL_UINT8(0, ds2431_write_scratchpad(0x7F, data, 4)); /* addr+len beyond EEPROM */
}

void test_write_scratchpad_misaligned_rejected(void) {
    const uint8_t data[8] = {0};
    hw_set_capture_source(de_capture_present);
    TEST_ASSERT_EQUAL_UINT8(0, ds2431_write_scratchpad(0x43, data, 4)); /* not 8-byte aligned */
    drive(ds2431_write_scratchpad_poll);
    TEST_ASSERT_EQUAL_UINT8(0, hw_ccr3_feed_log()->count); /* nothing scheduled */
    TEST_ASSERT_EQUAL_UINT8(0, ds2431_last_command_ok());
}

/*-------------------------------------------------------------
 *  4. Read Scratchpad (frame + echo + CRC verification)
 * -----------------------------------------------------------*/

void test_read_scratchpad_echo_verified(void) {
    /* Write an 8-byte block at 0x40: the device latches TA = 0x40, ES = 7.
     * The matching frame echoes TA1 = 0x40, TA2 = 0, ES = 7 plus the data. */
    const uint8_t data[8] = {0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88};
    uint8_t frame[DS2431_SCRATCH_FRAME];
    uint8_t got[DS2431_SCRATCH_FRAME];
    hw_set_capture_source(de_capture_present);
    ds2431_write_scratchpad(0x40, data, 8);
    drive(ds2431_write_scratchpad_poll);
    TEST_ASSERT_EQUAL_UINT8(1, ds2431_last_command_ok());

    frame[0] = 0x40;
    frame[1] = 0x00;
    frame[2] = 0x07; /* ES = (0x40 + 8 - 1) & 0x07 */
    memcpy(&frame[3], data, 8);
    set_scratchpad_reply(frame);
    ds2431_read_scratchpad(got);
    drive_ops(ds2431_read_scratchpad_poll, 2); /* reset + command write */
    assert_cmd_bytes((const uint8_t[]){0xCC, 0xAA}, 2);
    drive(ds2431_read_scratchpad_poll); /* finish the frame read */
    TEST_ASSERT_EQUAL_UINT8(1, ds2431_last_command_ok());
    for (uint8_t i = 0; i < DS2431_SCRATCH_FRAME; i++) {
        TEST_ASSERT_EQUAL_UINT8(frame[i], got[i]);
    }
}

void test_read_scratchpad_echo_mismatch(void) {
    /* A device that echoes a different TA/ES did not get the write: the
     * success flag must be cleared. */
    const uint8_t data[8] = {0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88};
    uint8_t frame[DS2431_SCRATCH_FRAME];
    uint8_t got[DS2431_SCRATCH_FRAME];
    hw_set_capture_source(de_capture_present);
    ds2431_write_scratchpad(0x40, data, 8);
    drive(ds2431_write_scratchpad_poll);
    TEST_ASSERT_EQUAL_UINT8(1, ds2431_last_command_ok());

    frame[0] = 0x40;
    frame[1] = 0x00;
    frame[2] = 0x00; /* wrong ES: the latched value is 7 */
    memcpy(&frame[3], data, 8);
    set_scratchpad_reply(frame); /* valid CRC: the abort must be the echo check */
    ds2431_read_scratchpad(got);
    drive(ds2431_read_scratchpad_poll);
    TEST_ASSERT_EQUAL_UINT8(0, ds2431_last_command_ok());
}

void test_read_scratchpad_crc_mismatch(void) {
    /* A reply whose trailing CRC-16 does not cover the frame (a corrupted
     * scratchpad read) must clear the success flag even though TA/ES echo
     * matches. */
    const uint8_t data[8] = {0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88};
    uint8_t frame[DS2431_SCRATCH_FRAME];
    uint8_t got[DS2431_SCRATCH_FRAME];
    hw_set_capture_source(de_capture_present);
    ds2431_write_scratchpad(0x40, data, 8);
    drive(ds2431_write_scratchpad_poll);
    TEST_ASSERT_EQUAL_UINT8(1, ds2431_last_command_ok());

    frame[0] = 0x40;
    frame[1] = 0x00;
    frame[2] = 0x07;
    memcpy(&frame[3], data, 8);
    set_scratchpad_reply(frame);
    /* Corrupt one data bit in the captured reply: TA/ES echo still matches,
     * so only the CRC-16 can catch it. */
    de_read_pulses[3 * DS2431_BITS_PER_BYTE + 0] =
        (de_read_pulses[3 * DS2431_BITS_PER_BYTE + 0] == ONE) ? ZERO : ONE;

    ds2431_read_scratchpad(got);
    drive(ds2431_read_scratchpad_poll);
    TEST_ASSERT_EQUAL_UINT8(0, ds2431_last_command_ok());
}

void test_read_scratchpad_payload_mismatch(void) {
    /* A byte corrupted on the way to the device is latched in the scratchpad:
     * the device computes the reply CRC-16 over what it stored and the TA/ES
     * echo still matches, so only the payload comparison can catch it before
     * Copy Scratchpad commits the corrupted byte. */
    const uint8_t data[8] = {0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88};
    uint8_t frame[DS2431_SCRATCH_FRAME];
    uint8_t got[DS2431_SCRATCH_FRAME];
    hw_set_capture_source(de_capture_present);
    ds2431_write_scratchpad(0x40, data, 8);
    drive(ds2431_write_scratchpad_poll);
    TEST_ASSERT_EQUAL_UINT8(1, ds2431_last_command_ok());

    frame[0] = 0x40;
    frame[1] = 0x00;
    frame[2] = 0x07;
    memcpy(&frame[3], data, 8);
    frame[3 + 2] ^= 0x01u; /* byte 2 arrived corrupted */
    set_scratchpad_reply(frame); /* CRC is valid over the corrupted content */
    ds2431_read_scratchpad(got);
    drive(ds2431_read_scratchpad_poll);
    TEST_ASSERT_EQUAL_UINT8(0, ds2431_last_command_ok());
}

void test_read_scratchpad_raw_without_pending_write(void) {
    /* Without a pending write there is no latch to compare against: the raw
     * frame is returned with the flag set from presence alone. */
    const uint8_t frame[DS2431_SCRATCH_FRAME] = {0x00, 0x00, 0x07,
                                                 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08};
    uint8_t got[DS2431_SCRATCH_FRAME];
    set_scratchpad_reply(frame);
    hw_set_capture_source(de_capture_present);
    ds2431_read_scratchpad(got);
    drive(ds2431_read_scratchpad_poll);
    TEST_ASSERT_EQUAL_UINT8(1, ds2431_last_command_ok());
    for (uint8_t i = 0; i < DS2431_SCRATCH_FRAME; i++) {
        TEST_ASSERT_EQUAL_UINT8(frame[i], got[i]);
    }
}

void test_read_scratchpad_short_reply(void) {
    /* The device returns (E[2:0] - T[2:0] + 1) data bytes, so a raw read of a
     * device whose scratchpad window is not full is shorter than 11 bytes: the
     * driver must verify the CRC over that shorter reply and zero the tail. */
    uint8_t frame[DS2431_SCRATCH_FRAME] = {0x48, 0x00, 0x03, /* TA1, TA2, ES = 3 */
                                           0x10, 0x20, 0x30, 0x40};
    uint8_t got[DS2431_SCRATCH_FRAME];
    set_scratchpad_reply_n(frame, 4); /* 3 + 4 = 7 reply bytes, then the CRC */
    hw_set_capture_source(de_capture_present);
    ds2431_read_scratchpad(got);
    drive(ds2431_read_scratchpad_poll);
    TEST_ASSERT_EQUAL_UINT8(1, ds2431_last_command_ok());
    for (uint8_t i = 0; i < 7; i++) {
        TEST_ASSERT_EQUAL_UINT8(frame[i], got[i]);
    }
    for (uint8_t i = 7; i < DS2431_SCRATCH_FRAME; i++) {
        TEST_ASSERT_EQUAL_UINT8(0, got[i]); /* unused tail is zeroed */
    }
}

void test_read_scratchpad_short_write_pf(void) {
    /* A short Write Scratchpad sets the device's PF flag: the latched data is
     * not a full block and Copy Scratchpad would refuse it, so the read-back
     * verification must not report success even though the echo and payload
     * match. */
    const uint8_t data[4] = {0x11, 0x22, 0x33, 0x44};
    uint8_t frame[DS2431_SCRATCH_FRAME];
    uint8_t got[DS2431_SCRATCH_FRAME];
    hw_set_capture_source(de_capture_present);
    ds2431_write_scratchpad(0x08, data, 4); /* ES = 3 */
    drive(ds2431_write_scratchpad_poll);
    TEST_ASSERT_EQUAL_UINT8(1, ds2431_last_command_ok());

    frame[0] = 0x08;
    frame[1] = 0x00;
    frame[2] = 0x23; /* PF (0x20) | ES = 3 */
    memcpy(&frame[3], data, 4);
    set_scratchpad_reply_n(frame, 4);
    ds2431_read_scratchpad(got);
    drive(ds2431_read_scratchpad_poll);
    TEST_ASSERT_EQUAL_UINT8(0, ds2431_last_command_ok()); /* PF rejects the write */
}

/*-------------------------------------------------------------
 *  5. Copy Scratchpad
 * -----------------------------------------------------------*/

void test_copy_scratchpad_requires_pending_write(void) {
    /* Copy sends TA + ES from the last Write Scratchpad; without one there is
     * nothing to program, so it must be ignored. */
    hw_set_capture_source(de_capture_present);
    ds2431_copy_scratchpad();
    TEST_ASSERT_EQUAL_UINT8(0, hw_ccr3_feed_log()->count);
    TEST_ASSERT_EQUAL_UINT8(1, ds2431_copy_scratchpad_poll());
    TEST_ASSERT_EQUAL_UINT8(0, ds2431_last_command_ok());
}

void test_copy_scratchpad_waits_tprog(void) {
    const uint8_t data[8] = {0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88};
    static const uint8_t k_bytes[] = {0xCC, 0x55, 0x40, 0x00, 0x07};
    hw_set_capture_source(de_capture_present);
    ds2431_write_scratchpad(0x40, data, 8);
    drive(ds2431_write_scratchpad_poll);
    TEST_ASSERT_EQUAL_UINT8(1, ds2431_last_command_ok());

    ds2431_copy_scratchpad();
    drive_ops(ds2431_copy_scratchpad_poll, 2); /* reset + command write */
    assert_cmd_bytes(k_bytes, sizeof(k_bytes));
    drive_ops(ds2431_copy_scratchpad_poll, 1); /* run the t_PROG hold-off timer */
    /* The hold-off timer is 11ms @ 1us/tick. */
    TEST_ASSERT_EQUAL_UINT32(DS2431_TPROG_US, mock_tim1.ARR);
    TEST_ASSERT_EQUAL_UINT32(0, mock_tim1.RCR);
    /* After the hold-off the device reports the copy outcome on the bus. */
    {
        const uint8_t status = 0xAA;
        set_read_bytes(&status, 1);
    }
    drive(ds2431_copy_scratchpad_poll);
    TEST_ASSERT_EQUAL_UINT8(1, ds2431_last_command_ok());
}

void test_copy_scratchpad_reports_refusal(void) {
    /* The device drives logic 1 when it refuses the copy (PF set, invalid
     * address or copy-protected page): the driver must not report success. */
    const uint8_t data[8] = {0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88};
    hw_set_capture_source(de_capture_present);
    ds2431_write_scratchpad(0x40, data, 8);
    drive(ds2431_write_scratchpad_poll);
    TEST_ASSERT_EQUAL_UINT8(1, ds2431_last_command_ok());

    ds2431_copy_scratchpad();
    drive_ops(ds2431_copy_scratchpad_poll, 2); /* reset + command write */
    {
        const uint8_t status = 0xFF; /* device refused */
        set_read_bytes(&status, 1);
    }
    drive(ds2431_copy_scratchpad_poll);
    TEST_ASSERT_EQUAL_UINT8(0, ds2431_last_command_ok());
}

void test_copy_scratchpad_no_presence_aborts(void) {
    const uint8_t data[8] = {0};
    hw_set_capture_source(de_capture_present);
    ds2431_write_scratchpad(0x20, data, 8);
    drive(ds2431_write_scratchpad_poll);

    hw_set_capture_source(de_capture_absent);
    ds2431_copy_scratchpad();
    drive(ds2431_copy_scratchpad_poll);
    TEST_ASSERT_EQUAL_UINT8(0, ds2431_last_command_ok());
}

/*-------------------------------------------------------------
 *  6. Full write cycle (parasite) end-to-end
 * -----------------------------------------------------------*/

void test_write_cycle_e2e(void) {
    const uint8_t data[8] = {0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF, 0x00, 0x11};
    uint8_t frame[DS2431_SCRATCH_FRAME];
    uint8_t got[DS2431_SCRATCH_FRAME];
    uint8_t buf[8];
    hw_set_capture_source(de_capture_present);
    ds2431_set_parasite(1);

    /* 1. Write Scratchpad. The write is the last DMA stage, so the command
     * is verified after the drive completes. */
    ds2431_write_scratchpad(0x28, data, 8);
    drive(ds2431_write_scratchpad_poll);
    TEST_ASSERT_EQUAL_UINT8(1, ds2431_last_command_ok());
    TEST_ASSERT_EQUAL_UINT8(1, ds2431_parasite_mode());
    {
        static const uint8_t k_write[] = {0xCC, 0x0F, 0x28, 0x00,
                                          0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF, 0x00, 0x11};
        assert_cmd_bytes(k_write, sizeof(k_write));
    }

    /* 2. Read Scratchpad, feed the matching echo: verify the block data. */
    frame[0] = 0x28;
    frame[1] = 0x00;
    frame[2] = 0x07; /* ES = (0x28 + 8 - 1) & 0x07 */
    memcpy(&frame[3], data, 8);
    set_scratchpad_reply(frame);
    ds2431_read_scratchpad(got);
    drive_ops(ds2431_read_scratchpad_poll, 2);
    assert_cmd_bytes((const uint8_t[]){0xCC, 0xAA}, 2);
    drive(ds2431_read_scratchpad_poll);
    TEST_ASSERT_EQUAL_UINT8(1, ds2431_last_command_ok());
    for (uint8_t i = 0; i < DS2431_SCRATCH_FRAME; i++) {
        TEST_ASSERT_EQUAL_UINT8(frame[i], got[i]);
    }

    /* 3. Copy Scratchpad to the EEPROM (t_PROG hold-off). Restore the
     * presence capture source first: the previous frame read left the model
     * feeding the read pulses, and a reset must observe real presence. */
    hw_set_capture_source(de_capture_present);
    ds2431_copy_scratchpad();
    drive_ops(ds2431_copy_scratchpad_poll, 2);
    assert_cmd_bytes((const uint8_t[]){0xCC, 0x55, 0x28, 0x00, 0x07}, 5);
    {
        const uint8_t status = 0xAA; /* the held-off program completed */
        set_read_bytes(&status, 1);
    }
    drive(ds2431_copy_scratchpad_poll);
    TEST_ASSERT_EQUAL_UINT8(1, ds2431_last_command_ok());

    /* 3b. A scratchpad read after the copy still verifies: Copy must not
     * disturb the latched write payload the read-back comparison uses. */
    set_scratchpad_reply(frame);
    hw_set_capture_source(de_capture_present); /* the copy-status read re-routed it */
    ds2431_read_scratchpad(got);
    drive_ops(ds2431_read_scratchpad_poll, 2);
    drive(ds2431_read_scratchpad_poll);
    TEST_ASSERT_EQUAL_UINT8(1, ds2431_last_command_ok());

    /* 4. Read the block back from the EEPROM (the harness models the EEPROM
     * contents as the same fed pulses). */
    set_read_bytes(data, 8);
    hw_set_capture_source(de_capture_present);
    ds2431_read_memory(buf, 0x28, 8);
    drive(ds2431_read_memory_poll);
    TEST_ASSERT_EQUAL_UINT8(1, ds2431_last_command_ok());
    for (uint8_t i = 0; i < 8; i++) {
        TEST_ASSERT_EQUAL_UINT8(data[i], buf[i]);
    }
}

/*-------------------------------------------------------------
 *  Run all DS2431 tests
 * -----------------------------------------------------------*/

void run_test_ds2431(void) {
    TEST_RUN(test_ds2431_init_state);
    TEST_RUN(test_crc16_maxim_vector);
    TEST_RUN(test_read_memory_skip_rom);
    TEST_RUN(test_read_memory_match_rom);
    TEST_RUN(test_read_memory_no_presence_aborts);
    TEST_RUN(test_read_memory_out_of_range_ignored);
    TEST_RUN(test_read_memory_two_passes);
    TEST_RUN(test_write_scratchpad_skip_rom);
    TEST_RUN(test_write_scratchpad_match_rom);
    TEST_RUN(test_write_scratchpad_no_presence_aborts);
    TEST_RUN(test_write_scratchpad_out_of_range_ignored);
    TEST_RUN(test_write_scratchpad_short_write);
    TEST_RUN(test_write_scratchpad_returns_status);
    TEST_RUN(test_write_scratchpad_misaligned_rejected);
    TEST_RUN(test_read_scratchpad_echo_verified);
    TEST_RUN(test_read_scratchpad_echo_mismatch);
    TEST_RUN(test_read_scratchpad_crc_mismatch);
    TEST_RUN(test_read_scratchpad_payload_mismatch);
    TEST_RUN(test_read_scratchpad_raw_without_pending_write);
    TEST_RUN(test_read_scratchpad_short_reply);
    TEST_RUN(test_read_scratchpad_short_write_pf);
    TEST_RUN(test_copy_scratchpad_requires_pending_write);
    TEST_RUN(test_copy_scratchpad_waits_tprog);
    TEST_RUN(test_copy_scratchpad_reports_refusal);
    TEST_RUN(test_copy_scratchpad_no_presence_aborts);
    TEST_RUN(test_write_cycle_e2e);
}