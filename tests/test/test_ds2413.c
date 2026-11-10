/* ============================================================
 *  test_ds2413.c - DS2413 dual-channel switch driver tests
 *
 *  Covers the non-blocking PIO operations on top of the shared
 *  1-Wire layer:
 *   - PIO Access Read (0xF5): Skip/Match ROM command build, reply
 *     validation (complement, 0xFF), pin vs latch normalization
 *   - PIO Access Write (0x5A): state + complement bytes, 0xAA ack,
 *     rejection, state masking
 *   - presence abort, busy guard, poll without an operation
 *
 *  Command-table verification: the shared feed DMA sources the CCR3
 *  reloads from &ctx.u.cmd[1] (slot 0 is applied by software), and the
 *  mock feed log records exactly those transfers.
 * ============================================================ */

#include "ds2413.h"
#include "ds2413_test_access.h"
#include "hw_model.h"
#include "mock_target.h"
#include "onewire.h"
#include "ow_port.h"
#include "unity.h"
#include <string.h>

#define ONE ONEWIRE_ONE_PULSE
#define ZERO ONEWIRE_ZERO_PULSE
#define BITS DS2413_BITS_PER_BYTE

static const uint8_t k_rom[DS2413_ROM_BYTES] = {
    0x3A,
    0x9F,
    0x47,
    0x03,
    0x00,
    0x00,
    0x00,
    0x3A,
};

static uint16_t de_capture_present(uint32_t idx) {
    return idx == 0 ? 510u : 700u;
}

static uint16_t de_capture_absent(uint32_t idx) {
    (void)idx;
    return 100u;
}

static uint16_t de_read_pulses[DS2413_BITS_PER_BYTE];
static uint16_t de_read_capture(uint32_t idx) { return de_read_pulses[idx]; }

static void set_read_byte(uint8_t b) {
    for (uint8_t i = 0; i < BITS; i++) {
        de_read_pulses[i] = ((b >> i) & 1u) ? ONE : ZERO;
    }
}

/* Step exactly one hardware operation. A reply read (> 2 captures) switches
 * the capture channel to the reply source; reset stays on presence. */
static void run_current_op(void) {
    if (mock_tim1.CR1 & TIM_CR1_CEN) {
        if ((mock_dma1_ch4.CCR & DMA_CCR_EN) &&
            MOCK_CAP_TRANSFERS(mock_dma1_ch4.CNDTR) > OW_PORT_CAPTURE_BUF_SIZE) {
            hw_set_capture_source(de_read_capture);
        }
        TEST_ASSERT_TRUE(hw_run_until_uif(256));
    }
}

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

static void drive_ops(uint8_t (*poll)(void), uint8_t nops) {
    uint16_t guard = 0;
    uint8_t done = 0;
    while (done < nops && guard <= 500) {
        if (poll()) {
            done = nops;
        } else {
            run_current_op();
            done++;
        }
        guard++;
    }
    TEST_ASSERT_TRUE(guard <= 500);
}

/* Assert the last completed command write transmitted exactly these bytes. */
static void assert_cmd_bytes(const uint8_t* bytes, uint8_t nbytes) {
    const hw_ccr3_feed_log_t* log = hw_ccr3_feed_log();
    const uint16_t slots = (uint16_t)(nbytes * BITS);
    TEST_ASSERT_TRUE(slots <= 128u);
    TEST_ASSERT_EQUAL_UINT16(slots, log->count);
    for (uint8_t i = 0; i < nbytes; i++) {
        for (uint8_t b = 0; b < BITS; b++) {
            const uint16_t want = ((bytes[i] >> b) & 1u) ? ONE : ZERO;
            const uint8_t s = (uint8_t)(i * BITS + b);
            if (s == 0) {
                TEST_ASSERT_EQUAL_UINT16(want, ds2413_test_get_cmd_pulse(0));
            } else {
                TEST_ASSERT_EQUAL_UINT16(want, log->values[s - 1]);
            }
        }
    }
    TEST_ASSERT_EQUAL_UINT16(ONEWIRE_RELEASE_PULSE, log->values[slots - 1]);
}

/*-------------------------------------------------------------
 *  1. Initial state
 * -----------------------------------------------------------*/

void test_ds2413_init_state(void) {
    TEST_ASSERT_EQUAL_UINT8(1, ds2413_read_pio_poll());
    TEST_ASSERT_EQUAL_UINT8(0, ds2413_last_command_ok());
    TEST_ASSERT_EQUAL_UINT8(0, ds2413_last_pio());
    TEST_ASSERT_EQUAL_UINT8(0, ds2413_last_latches());
}

/*-------------------------------------------------------------
 *  2. PIO Access Read
 * -----------------------------------------------------------*/

void test_read_pio_skip_rom(void) {
    static const uint8_t k_cmd[] = {0xCC, 0xF5};
    uint8_t out = 0xFF;
    set_read_byte(0xE1); /* pin PIOA high; both latch bits 0 (driving low) */
    hw_set_capture_source(de_capture_present);

    TEST_ASSERT_TRUE(ds2413_read_pio(&out));
    drive_ops(ds2413_read_pio_poll, 3); /* reset + command */
    assert_cmd_bytes(k_cmd, sizeof(k_cmd));
    drive(ds2413_read_pio_poll);

    TEST_ASSERT_EQUAL_UINT8(DS2413_PIOA, out);
    TEST_ASSERT_EQUAL_UINT8(DS2413_PIOA, ds2413_last_pio());
    TEST_ASSERT_EQUAL_UINT8(0, ds2413_last_latches());
    TEST_ASSERT_TRUE(ds2413_last_command_ok());
}

void test_read_pio_match_rom(void) {
    uint8_t out = 0;
    uint8_t cmd[1 + DS2413_ROM_BYTES + 1];
    cmd[0] = 0x55;
    memcpy(&cmd[1], k_rom, DS2413_ROM_BYTES);
    cmd[1 + DS2413_ROM_BYTES] = 0xF5;

    set_read_byte(0x96); /* PIOA latch high, PIOB pin high */
    hw_set_capture_source(de_capture_present);
    ds2413_select(k_rom);

    TEST_ASSERT_TRUE(ds2413_read_pio(&out));
    drive_ops(ds2413_read_pio_poll, 3);
    assert_cmd_bytes(cmd, sizeof(cmd));
    drive(ds2413_read_pio_poll);

    TEST_ASSERT_EQUAL_UINT8(DS2413_PIOB, out);
    TEST_ASSERT_EQUAL_UINT8(DS2413_PIOB, ds2413_last_pio());
    TEST_ASSERT_EQUAL_UINT8(DS2413_PIOA, ds2413_last_latches());
    TEST_ASSERT_TRUE(ds2413_last_command_ok());
}

void test_read_pio_pin_and_latch_differ(void) {
    uint8_t out = 0xFF;
    set_read_byte(0x69); /* PIOA pin high, PIOB latch high; opposite halves */
    hw_set_capture_source(de_capture_present);

    TEST_ASSERT_TRUE(ds2413_read_pio(&out));
    drive(ds2413_read_pio_poll);

    TEST_ASSERT_EQUAL_UINT8(DS2413_PIOA, out);
    TEST_ASSERT_EQUAL_UINT8(DS2413_PIOA, ds2413_last_pio());
    TEST_ASSERT_EQUAL_UINT8(DS2413_PIOB, ds2413_last_latches());
    TEST_ASSERT_TRUE(ds2413_last_command_ok());
}

void test_read_pio_no_response(void) {
    uint8_t out = 0x5A;
    set_read_byte(DS2413_NO_RESPONSE);
    hw_set_capture_source(de_capture_present);

    TEST_ASSERT_TRUE(ds2413_read_pio(&out));
    drive(ds2413_read_pio_poll);

    TEST_ASSERT_EQUAL_UINT8(0x5A, out);
    TEST_ASSERT_FALSE(ds2413_last_command_ok());
    TEST_ASSERT_EQUAL_UINT8(0, ds2413_last_pio());
}

void test_read_pio_bad_complement(void) {
    uint8_t out = 0;
    set_read_byte(0x12); /* low nibble 2, high nibble 1 (not ~2) */
    hw_set_capture_source(de_capture_present);

    TEST_ASSERT_TRUE(ds2413_read_pio(&out));
    drive(ds2413_read_pio_poll);

    TEST_ASSERT_EQUAL_UINT8(0, out);
    TEST_ASSERT_FALSE(ds2413_last_command_ok());
}

void test_read_pio_requires_destination(void) {
    TEST_ASSERT_FALSE(ds2413_read_pio(NULL));
}

/*-------------------------------------------------------------
 *  3. PIO Access Write
 * -----------------------------------------------------------*/

void test_write_pio_skip_rom(void) {
    static const uint8_t k_cmd[] = {0xCC, 0x5A, 0xFD, 0x02};
    set_read_byte(DS2413_ACK);
    hw_set_capture_source(de_capture_present);

    TEST_ASSERT_TRUE(ds2413_write_pio(DS2413_PIOA));
    drive_ops(ds2413_write_pio_poll, 3);
    assert_cmd_bytes(k_cmd, sizeof(k_cmd));
    drive(ds2413_write_pio_poll);

    TEST_ASSERT_TRUE(ds2413_last_command_ok());
}

void test_write_pio_match_rom(void) {
    uint8_t cmd[1 + DS2413_ROM_BYTES + 3];
    cmd[0] = 0x55;
    memcpy(&cmd[1], k_rom, DS2413_ROM_BYTES);
    cmd[1 + DS2413_ROM_BYTES] = 0x5A;
    cmd[2 + DS2413_ROM_BYTES] = 0xFE; /* PIOB, bits 2-7 = 1 */
    cmd[3 + DS2413_ROM_BYTES] = 0x01;

    set_read_byte(DS2413_ACK);
    hw_set_capture_source(de_capture_present);
    ds2413_select(k_rom);

    TEST_ASSERT_TRUE(ds2413_write_pio(DS2413_PIOB));
    drive_ops(ds2413_write_pio_poll, 3);
    assert_cmd_bytes(cmd, sizeof(cmd));
    drive(ds2413_write_pio_poll);

    TEST_ASSERT_TRUE(ds2413_last_command_ok());
}

void test_write_pio_masks_unused_bits(void) {
    static const uint8_t k_cmd[] = {0xCC, 0x5A, 0xFE, 0x01};
    set_read_byte(DS2413_ACK);
    hw_set_capture_source(de_capture_present);

    TEST_ASSERT_TRUE(ds2413_write_pio(0xFE)); /* only PIOB survives */
    drive_ops(ds2413_write_pio_poll, 3);
    assert_cmd_bytes(k_cmd, sizeof(k_cmd));
    drive(ds2413_write_pio_poll);
}

void test_write_pio_rejected_ack(void) {
    set_read_byte(DS2413_NO_RESPONSE);
    hw_set_capture_source(de_capture_present);

    TEST_ASSERT_TRUE(ds2413_write_pio(DS2413_PIOA));
    drive(ds2413_write_pio_poll);

    TEST_ASSERT_FALSE(ds2413_last_command_ok());
}

/*-------------------------------------------------------------
 *  4. Abort and ownership
 * -----------------------------------------------------------*/

void test_read_pio_presence_abort(void) {
    uint8_t out = 0x5A;
    hw_set_capture_source(de_capture_absent);

    TEST_ASSERT_TRUE(ds2413_read_pio(&out));
    drive(ds2413_read_pio_poll);

    TEST_ASSERT_EQUAL_UINT8(0x5A, out);
    TEST_ASSERT_FALSE(ds2413_last_command_ok());
    TEST_ASSERT_EQUAL_UINT8(0, hw_ccr3_feed_log()->count);
}

void test_busy_guard_rejects_second_start(void) {
    uint8_t out = 0;
    set_read_byte(0xE1);
    hw_set_capture_source(de_capture_present);

    TEST_ASSERT_TRUE(ds2413_read_pio(&out));
    TEST_ASSERT_FALSE(ds2413_write_pio(DS2413_PIOA));
    TEST_ASSERT_FALSE(ds2413_read_pio(&out));
    drive(ds2413_read_pio_poll);
    TEST_ASSERT_TRUE(ds2413_last_command_ok());
}

void test_select_ignored_while_busy(void) {
    uint8_t out = 0;
    static const uint8_t other[DS2413_ROM_BYTES] = {0x3A, 1, 2, 3, 4, 5, 6, 7};
    set_read_byte(0xE1);
    hw_set_capture_source(de_capture_present);

    ds2413_select(k_rom);
    TEST_ASSERT_TRUE(ds2413_read_pio(&out));
    ds2413_select(other); /* ignored: the operation already built its address */
    drive(ds2413_read_pio_poll);
    TEST_ASSERT_TRUE(ds2413_last_command_ok());
}

/*-------------------------------------------------------------
 *  Runner
 * -----------------------------------------------------------*/

void run_test_ds2413(void) {
    TEST_RUN(test_ds2413_init_state);
    TEST_RUN(test_read_pio_skip_rom);
    TEST_RUN(test_read_pio_match_rom);
    TEST_RUN(test_read_pio_pin_and_latch_differ);
    TEST_RUN(test_read_pio_no_response);
    TEST_RUN(test_read_pio_bad_complement);
    TEST_RUN(test_read_pio_requires_destination);
    TEST_RUN(test_write_pio_skip_rom);
    TEST_RUN(test_write_pio_match_rom);
    TEST_RUN(test_write_pio_masks_unused_bits);
    TEST_RUN(test_write_pio_rejected_ack);
    TEST_RUN(test_read_pio_presence_abort);
    TEST_RUN(test_busy_guard_rejects_second_start);
    TEST_RUN(test_select_ignored_while_busy);
}
