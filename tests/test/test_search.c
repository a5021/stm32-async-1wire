/* ============================================================
 *  test_search.c - Single-Device Search Integration Test
 *
 *  Runs the full non-blocking Maxim Search ROM (0xF0) state
 *  machine against a simulated single DS18B20. The capture
 *  source answers every id/cmp pair for one known ROM, so the
 *  search must walk all 64 bits through the merged write+read
 *  operations, validate CRC/family and report exactly one
 *  device through the sink callback.
 * ============================================================ */

#include "ds18b20.h"
#include "ds18b20_test_access.h"
#include "hw_model.h"
#include "stm32f1xx.h"
#include "unity.h"
#include <string.h>

#define ONE 5u
#define ZERO 60u

static uint8_t g_rom[8];
static uint8_t g_rom_b[8];
static uint8_t g_found_roms[4][8];
static uint8_t g_found_count;
static uint8_t g_wr_bit; /* bit whose pair the next merged write+read returns */
static uint8_t g_pass; /* search pass counter (reset by each presence reset) */

static uint8_t sink(const uint8_t* rom) {
    memcpy(g_found_roms[g_found_count++], rom, 8);
    return 0;
}

/* Infer the running operation from the mock timer and answer its captures. */
static uint16_t search_capture_src(uint32_t idx) {
    uint8_t rcr = (uint8_t)mock_tim1.RCR;
    if (rcr == 0) {
        return idx == 0 ? 510u : 700u; /* reset + presence pulse */
    }
    if (rcr == 1) { /* first read pair: bit 1 */
        uint8_t b = (g_rom[0] >> 0) & 1u;
        return (idx == 0) ? (b ? ONE : ZERO) : (b ? ZERO : ONE);
    }
    /* merged write+read capturing bit g_wr_bit (idx0 = write edge, ignored) */
    uint8_t byte = (g_wr_bit - 1u) / 8u;
    uint8_t bit = (g_wr_bit - 1u) % 8u;
    uint8_t b = (g_rom[byte] >> bit) & 1u;
    if (idx == 0) return 0u;
    if (idx == 1) return b ? ONE : ZERO;
    g_wr_bit++; /* last capture of this op: next op answers the next bit */
    return b ? ZERO : ONE;
}

/*-------------------------------------------------------------
 *  Search must find exactly one device with the exact ROM.
 * -----------------------------------------------------------*/
void test_search_finds_single_device(void) {
    uint8_t serial[7] = {0x28, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06};
    memcpy(g_rom, serial, 7);
    g_rom[7] = ds18b20_crc8(g_rom, 7);
    TEST_ASSERT_EQUAL_UINT8(0, ds18b20_crc8(g_rom, 8)); /* self-check construction */

    g_found_count = 0;
    g_wr_bit = 2;
    hw_set_capture_source(search_capture_src);
    ds18b20_search_start(sink, 1);

    uint16_t guard = 0;
    for (;;) {
        if (ds18b20_search_poll()) {
            break;
        }
        if (mock_tim1.CR1 & TIM_CR1_CEN) {
            uint8_t ok = hw_run_until_uif(100);
            TEST_ASSERT_TRUE(ok);
        }
        if (++guard > 500) {
            break;
        }
    }
    TEST_ASSERT_TRUE(guard <= 500);

    TEST_ASSERT_EQUAL_UINT8(1, ds18b20_search_count());
    TEST_ASSERT_EQUAL_UINT8(1, g_found_count);
    for (int i = 0; i < 8; i++) {
        TEST_ASSERT_EQUAL_HEX8(g_rom[i], g_found_roms[0][i]);
    }
}

/*-------------------------------------------------------------
 *  The 0xF0 Search ROM command is DMA-fed (8 slot pulses) and
 *  must end with a trailing 0 in the CCR1 feed so the bus is
 *  released HIGH even before the next read_pair is scheduled.
 *  After the whole search completes (DONE phase, EGR=UG hand-
 *  over) the bus must still be idle HIGH.
 * -----------------------------------------------------------*/
void test_search_command_feed_release(void) {
    uint8_t serial[7] = {0x28, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06};
    memcpy(g_rom, serial, 7);
    g_rom[7] = ds18b20_crc8(g_rom, 7);

    g_found_count = 0;
    g_wr_bit = 2;
    hw_set_capture_source(search_capture_src);
    ds18b20_search_start(sink, 1);

    uint16_t guard = 0;
    uint8_t f0_feed_checked = 0;
    for (;;) {
        if (ds18b20_search_poll()) {
            break;
        }
        if (mock_tim1.CR1 & TIM_CR1_CEN) {
            uint8_t ok = hw_run_until_uif(100);
            TEST_ASSERT_TRUE(ok);
            if (!f0_feed_checked) {
                const hw_ccr1_feed_log_t* log = hw_ccr1_feed_log();
                if (log->count == 8) { /* 0xF0 command: exactly 8 slot pulses */
                    f0_feed_checked = 1;
                    TEST_ASSERT_EQUAL_UINT16(0, log->values[7]); /* trailing release zero */
                    TEST_ASSERT_TRUE(log->values[0] != 0);
                }
            }
        }
        if (++guard > 500) {
            break;
        }
    }
    TEST_ASSERT_TRUE(guard <= 500);
    TEST_ASSERT_TRUE(f0_feed_checked);

    TEST_ASSERT_EQUAL_UINT8(1, ds18b20_search_count());
    TEST_ASSERT_EQUAL_UINT8(1, g_found_count);

    /* DONE phase (EGR=UG timer handover) leaves the bus idle HIGH. */
    TEST_ASSERT_EQUAL_UINT16(0, hw_effective_ccr1());
    TEST_ASSERT_FALSE(mock_tim1.CR1 & TIM_CR1_CEN);
}
void test_search_finds_different_serial(void) {
    uint8_t serial[7] = {0x28, 0xA1, 0xB2, 0xC3, 0xD4, 0xE5, 0xF6};
    memcpy(g_rom, serial, 7);
    g_rom[7] = ds18b20_crc8(g_rom, 7);

    g_found_count = 0;
    g_wr_bit = 2;
    hw_set_capture_source(search_capture_src);
    ds18b20_search_start(sink, 1);

    uint16_t guard = 0;
    for (;;) {
        if (ds18b20_search_poll()) {
            break;
        }
        if (mock_tim1.CR1 & TIM_CR1_CEN) {
            uint8_t ok = hw_run_until_uif(100);
            TEST_ASSERT_TRUE(ok);
        }
        if (++guard > 500) {
            break;
        }
    }
    TEST_ASSERT_TRUE(guard <= 500);

    TEST_ASSERT_EQUAL_UINT8(1, ds18b20_search_count());
    TEST_ASSERT_EQUAL_UINT8(1, g_found_count);
    for (int i = 0; i < 8; i++) {
        TEST_ASSERT_EQUAL_HEX8(g_rom[i], g_found_roms[0][i]);
    }
}

/*-------------------------------------------------------------
 *  A bad-ROM family code (not 0x28) is filtered out: search
 *  completes but reports zero devices.
 * -----------------------------------------------------------*/
void test_search_filters_non_ds18b20_family(void) {
    uint8_t serial[7] = {0x10, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06};
    memcpy(g_rom, serial, 7);
    g_rom[7] = ds18b20_crc8(g_rom, 7);

    g_found_count = 0;
    g_wr_bit = 2;
    hw_set_capture_source(search_capture_src);
    ds18b20_search_start(sink, 1);

    uint16_t guard = 0;
    for (;;) {
        if (ds18b20_search_poll()) {
            break;
        }
        if (mock_tim1.CR1 & TIM_CR1_CEN) {
            uint8_t ok = hw_run_until_uif(100);
            TEST_ASSERT_TRUE(ok);
        }
        if (++guard > 500) {
            break;
        }
    }
    TEST_ASSERT_TRUE(guard <= 500);

    TEST_ASSERT_EQUAL_UINT8(0, ds18b20_search_count());
    TEST_ASSERT_EQUAL_UINT8(0, g_found_count);
}

static uint16_t no_presence_src(uint32_t i) {
    (void)i;
    return 100u;
}

/*-------------------------------------------------------------
 *  No device on the bus (no presence pulse): search finishes
 *  immediately without finding anything.
 * -----------------------------------------------------------*/
void test_search_no_device_no_presence(void) {
    hw_set_capture_source(no_presence_src);

    g_found_count = 0;
    g_wr_bit = 2;
    ds18b20_search_start(sink, 1);

    uint16_t guard = 0;
    for (;;) {
        if (ds18b20_search_poll()) {
            break;
        }
        if (mock_tim1.CR1 & TIM_CR1_CEN) {
            uint8_t ok = hw_run_until_uif(100);
            TEST_ASSERT_TRUE(ok);
        }
        if (++guard > 500) {
            break;
        }
    }
    TEST_ASSERT_TRUE(guard <= 500);

    TEST_ASSERT_EQUAL_UINT8(0, ds18b20_search_count());
    TEST_ASSERT_EQUAL_UINT8(0, g_found_count);
}

/*-------------------------------------------------------------
 *  write_then_read arms the merged op: 3-slot timer pass, PWM
 *  WITHOUT OC1PE (so the CCR4 DMA reload is immediate), capture
 *  DMA for 3 edges and reload DMA feeding {ONE,ONE,0}.
 * -----------------------------------------------------------*/
void test_write_then_read_configures_registers(void) {
    hw_reset_all();
    test_bus_write_then_read(0);

    TEST_ASSERT_EQUAL_UINT32(2, mock_tim1.RCR);
    TEST_ASSERT_EQUAL_UINT32(ZERO, mock_tim1.CCR1); /* write bit = 0 */
    TEST_ASSERT_EQUAL_UINT32(ONE + ZERO, mock_tim1.CCR4);

    TEST_ASSERT_BITS_LOW(TIM_CCMR1_OC1PE, mock_tim1.CCMR1);
    TEST_ASSERT_BITS_HIGH(TIM_DIER_CC2DE | TIM_DIER_CC4DE, mock_tim1.DIER);
    TEST_ASSERT_BITS_HIGH(TIM_CCER_CC1E | TIM_CCER_CC2E, mock_tim1.CCER);
    TEST_ASSERT_BITS_HIGH(TIM_CR1_OPM | TIM_CR1_CEN, mock_tim1.CR1);

    TEST_ASSERT_EQUAL_UINT32(3, mock_dma1_ch3.CNDTR);
    TEST_ASSERT_EQUAL_UINT32(3, mock_dma1_ch4.CNDTR);
    TEST_ASSERT_BITS_HIGH(DMA_CCR_EN | DMA_CCR_MINC, mock_dma1_ch3.CCR);
    TEST_ASSERT_BITS_HIGH(DMA_CCR_EN | DMA_CCR_DIR | DMA_CCR_MINC, mock_dma1_ch4.CCR);
}

/*-------------------------------------------------------------
 *  Two devices differing only at bit 9 are both found. Pass 1
 *  takes the '0' branch at the discrepancy (romA), pass 2 the
 *  '1' branch (romB). At the discrepancy the bus returns id=0,
 *  cmp=0; everywhere else both devices agree with romA, so the
 *  single-rom bit lookup still applies.
 * -----------------------------------------------------------*/
static uint16_t two_dev_capture_src(uint32_t idx) {
    uint8_t rcr = (uint8_t)mock_tim1.RCR;
    if (rcr == 0) {
        if (idx == 0) g_pass++; /* each reset starts a new search pass */
        return idx == 0 ? 510u : 700u; /* reset + presence pulse */
    }
    if (rcr == 1) { /* first read pair: bit 1 of this pass (shared family) */
        g_wr_bit = 2;
        uint8_t b = (g_rom[0] >> 0) & 1u;
        return (idx == 0) ? (b ? ONE : ZERO) : (b ? ZERO : ONE);
    }
    /* merged write+read capturing bit g_wr_bit (idx0 = write edge, ignored) */
    uint8_t b;
    if (g_wr_bit == 9) {
        b = 2u; /* discrepancy: id=0, cmp=0 -> both ZERO */
    } else if (g_wr_bit < 9) {
        uint8_t byte = (g_wr_bit - 1u) / 8u;
        uint8_t bit = (g_wr_bit - 1u) % 8u;
        b = (g_rom[byte] >> bit) & 1u; /* family byte: identical in both */
    } else {
        /* after the discrepancy only the chosen device answers; pass 1 picked
         * the '0' branch (romA), pass 2 the '1' branch (romB) */
        const uint8_t* rom = (g_pass == 1) ? g_rom : g_rom_b;
        uint8_t byte = (g_wr_bit - 1u) / 8u;
        uint8_t bit = (g_wr_bit - 1u) % 8u;
        b = (rom[byte] >> bit) & 1u;
    }
    if (idx == 0) return 0u;
    if (idx == 1) return b == 2u ? ZERO : (b ? ONE : ZERO);
    g_wr_bit++; /* last capture of this op: next op answers the next bit */
    return b == 2u ? ZERO : (b ? ZERO : ONE);
}

void test_search_two_devices_found(void) {
    uint8_t romA[8] = {DS18B20_FAMILY_CODE, 0x00, 0x11, 0x22, 0x33, 0x44, 0x55, 0x00};
    uint8_t romB[8] = {DS18B20_FAMILY_CODE, 0x01, 0x11, 0x22, 0x33, 0x44, 0x55, 0x00};
    romA[7] = ds18b20_crc8(romA, 7);
    romB[7] = ds18b20_crc8(romB, 7);

    memcpy(g_rom, romA, 8); /* single-rom lookup shared by both passes */
    memcpy(g_rom_b, romB, 8);

    g_found_count = 0;
    g_wr_bit = 2;
    g_pass = 0;
    hw_set_capture_source(two_dev_capture_src);
    ds18b20_search_start(sink, 2);

    uint16_t guard = 0;
    for (;;) {
        if (ds18b20_search_poll()) {
            break;
        }
        if (mock_tim1.CR1 & TIM_CR1_CEN) {
            uint8_t ok = hw_run_until_uif(100);
            TEST_ASSERT_TRUE(ok);
        }
        if (++guard > 500) {
            break;
        }
    }
    TEST_ASSERT_TRUE(guard <= 500);

    TEST_ASSERT_EQUAL_UINT8(2, ds18b20_search_count());
    TEST_ASSERT_EQUAL_UINT8(2, g_found_count);
    for (int i = 0; i < 8; i++) {
        TEST_ASSERT_EQUAL_HEX8(romA[i], g_found_roms[0][i]);
        TEST_ASSERT_EQUAL_HEX8(romB[i], g_found_roms[1][i]);
    }
}

/*-------------------------------------------------------------
 *  max_devices == 0 aborts the search immediately: the driver
 *  schedules no hardware operation and reports DONE.
 * -----------------------------------------------------------*/
void test_search_max_zero_aborts(void) {
    hw_reset_all();
    g_found_count = 0;
    g_wr_bit = 2;

    ds18b20_search_start(sink, 0);

    /* No hardware scheduled */
    TEST_ASSERT_FALSE(mock_tim1.CR1 & TIM_CR1_CEN);
    /* First poll completes the search with zero devices */
    TEST_ASSERT_EQUAL_UINT8(1, ds18b20_search_poll());
    TEST_ASSERT_EQUAL_UINT8(0, ds18b20_search_count());
    TEST_ASSERT_EQUAL_UINT8(0, g_found_count);
}

/*-------------------------------------------------------------
 *  A sink that returns non-zero aborts the search after the
 *  first device, even when max_devices allows more.
 * -----------------------------------------------------------*/
static uint8_t early_stop_calls;
static uint8_t early_stop_sink(const uint8_t* rom) {
    memcpy(g_found_roms[0], rom, 8);
    early_stop_calls++;
    return 1; /* stop the search */
}

void test_search_sink_early_stop(void) {
    uint8_t serial[7] = {0x28, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06};
    memcpy(g_rom, serial, 7);
    g_rom[7] = ds18b20_crc8(g_rom, 7);

    g_found_count = 0;
    g_wr_bit = 2;
    early_stop_calls = 0;
    hw_set_capture_source(search_capture_src);
    ds18b20_search_start(early_stop_sink, 4);

    uint16_t guard = 0;
    for (;;) {
        if (ds18b20_search_poll()) {
            break;
        }
        if (mock_tim1.CR1 & TIM_CR1_CEN) {
            uint8_t ok = hw_run_until_uif(100);
            TEST_ASSERT_TRUE(ok);
        }
        if (++guard > 500) {
            break;
        }
    }
    TEST_ASSERT_TRUE(guard <= 500);

    TEST_ASSERT_EQUAL_UINT8(1, early_stop_calls);
    TEST_ASSERT_EQUAL_UINT8(1, ds18b20_search_count());
    for (int i = 0; i < 8; i++) {
        TEST_ASSERT_EQUAL_HEX8(g_rom[i], g_found_roms[0][i]);
    }
}

/*-------------------------------------------------------------
 *  A (id=1, cmp=1) pair means no device follows the path: the
 *  search tree is exhausted and the search must terminate.
 * -----------------------------------------------------------*/
static uint16_t exhausted_src(uint32_t idx) {
    uint8_t rcr = (uint8_t)mock_tim1.RCR;
    if (rcr == 0) {
        return idx == 0 ? 510u : 700u; /* presence present */
    }
    if (rcr == 1) { /* first read pair: id=1, cmp=1 */
        g_wr_bit = 2;
        return (idx == 0) ? ONE : ONE;
    }
    return 0u; /* unreachable: search terminates before the merged ops */
}

void test_search_pair_11_terminates(void) {
    hw_set_capture_source(exhausted_src);

    g_found_count = 0;
    g_wr_bit = 2;
    ds18b20_search_start(sink, 4);

    uint16_t guard = 0;
    for (;;) {
        if (ds18b20_search_poll()) {
            break;
        }
        if (mock_tim1.CR1 & TIM_CR1_CEN) {
            uint8_t ok = hw_run_until_uif(100);
            TEST_ASSERT_TRUE(ok);
        }
        if (++guard > 500) {
            break;
        }
    }
    TEST_ASSERT_TRUE(guard <= 500);

    TEST_ASSERT_EQUAL_UINT8(0, ds18b20_search_count());
    TEST_ASSERT_EQUAL_UINT8(0, g_found_count);
    TEST_ASSERT_EQUAL_UINT8(1, ds18b20_search_poll()); /* finished */
}

/*-------------------------------------------------------------
 *  A NULL sink must not be dereferenced. The search still runs to
 *  completion and counts devices internally, but the per-device
 *  callback is never invoked: exercises the `if (search_ctx.sink
 *  && ...)` guard in ds18b20.c.
 * -----------------------------------------------------------*/
void test_search_null_sink_completes(void) {
    uint8_t serial[7] = {0x28, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06};
    memcpy(g_rom, serial, 7);
    g_rom[7] = ds18b20_crc8(g_rom, 7);

    g_found_count = 0;
    g_wr_bit = 2;
    hw_set_capture_source(search_capture_src);
    ds18b20_search_start(NULL, 1);

    uint16_t guard = 0;
    for (;;) {
        if (ds18b20_search_poll()) {
            break;
        }
        if (mock_tim1.CR1 & TIM_CR1_CEN) {
            uint8_t ok = hw_run_until_uif(100);
            TEST_ASSERT_TRUE(ok);
        }
        if (++guard > 500) {
            break;
        }
    }
    TEST_ASSERT_TRUE(guard <= 500);

    /* Device found internally... */
    TEST_ASSERT_EQUAL_UINT8(1, ds18b20_search_count());
    /* ...but the NULL sink was never invoked. */
    TEST_ASSERT_EQUAL_UINT8(0, g_found_count);
}

/*-------------------------------------------------------------
 *  A device whose ROM CRC is wrong is rejected: the search runs
 *  the full 64 bits, then discards the ROM and reports nothing.
 * -----------------------------------------------------------*/
void test_search_rejects_bad_crc_rom(void) {
    uint8_t serial[7] = {0x28, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06};
    memcpy(g_rom, serial, 7);
    g_rom[7] = (uint8_t)(ds18b20_crc8(g_rom, 7) ^ 0xFF); /* corrupt CRC */

    g_found_count = 0;
    g_wr_bit = 2;
    hw_set_capture_source(search_capture_src);
    ds18b20_search_start(sink, 1);

    uint16_t guard = 0;
    for (;;) {
        if (ds18b20_search_poll()) {
            break;
        }
        if (mock_tim1.CR1 & TIM_CR1_CEN) {
            uint8_t ok = hw_run_until_uif(100);
            TEST_ASSERT_TRUE(ok);
        }
        if (++guard > 500) {
            break;
        }
    }
    TEST_ASSERT_TRUE(guard <= 500);

    TEST_ASSERT_EQUAL_UINT8(0, ds18b20_search_count());
    TEST_ASSERT_EQUAL_UINT8(0, g_found_count);
}

/*-------------------------------------------------------------
 *  Four devices differing at bits 9 and 10 (byte 1, bits 0-1):
 *  00 (A), 01 (C), 10 (B), 11 (D). Pass 1 takes '0' at both
 *  discrepancies (A), pass 2 takes bit 10 = '1' (C), pass 3
 *  takes bit 9 = '1' (B), pass 4 both '1' (D). At bits 9 and 10
 *  the bus returns id=0/cmp=0; everywhere else all devices agree,
 *  so the answer is the chosen device's bit.
 * -----------------------------------------------------------*/
static uint8_t g_roms4[4][8];
static uint8_t g_pass4;

static uint16_t four_dev_capture_src(uint32_t idx) {
    uint8_t rcr = (uint8_t)mock_tim1.RCR;
    if (rcr == 0) {
        if (idx == 0) g_pass4++;
        return idx == 0 ? 510u : 700u;
    }
    if (rcr == 1) { /* first read pair: bit 1, shared family */
        g_wr_bit = 2;
        uint8_t b = (g_roms4[0][0] >> 0) & 1u;
        return (idx == 0) ? (b ? ONE : ZERO) : (b ? ZERO : ONE);
    }
    /* merged write+read capturing bit g_wr_bit */
    uint8_t b;
    if (g_wr_bit <= 8) {
        uint8_t byte = (g_wr_bit - 1u) / 8u;
        uint8_t bit = (g_wr_bit - 1u) % 8u;
        b = (g_roms4[0][byte] >> bit) & 1u; /* common family bits */
    } else if (g_wr_bit == 9 || g_wr_bit == 10) {
        b = 2u; /* discrepancy: id=0, cmp=0 */
    } else {
        /* after the discrepancies only the chosen device answers.
         * search order: 00(A), 01(C), 10(B), 11(D) */
        static const uint8_t order[4] = {0, 2, 1, 3};
        const uint8_t* rom = g_roms4[order[g_pass4 - 1]];
        uint8_t byte = (g_wr_bit - 1u) / 8u;
        uint8_t bit = (g_wr_bit - 1u) % 8u;
        b = (rom[byte] >> bit) & 1u;
    }
    if (idx == 0) return 0u;
    if (idx == 1) return b == 2u ? ZERO : (b ? ONE : ZERO);
    g_wr_bit++;
    return b == 2u ? ZERO : (b ? ZERO : ONE);
}

void test_search_four_devices_found(void) {
    uint8_t ser[4][7] = {
        {0x28, 0x00, 0x11, 0x22, 0x33, 0x44, 0x55},
        {0x28, 0x01, 0x11, 0x22, 0x33, 0x44, 0x55},
        {0x28, 0x02, 0x11, 0x22, 0x33, 0x44, 0x55},
        {0x28, 0x03, 0x11, 0x22, 0x33, 0x44, 0x55},
    };
    for (int d = 0; d < 4; d++) {
        memcpy(g_roms4[d], ser[d], 7);
        g_roms4[d][7] = ds18b20_crc8(g_roms4[d], 7);
        TEST_ASSERT_EQUAL_UINT8(0, ds18b20_crc8(g_roms4[d], 8));
    }

    g_found_count = 0;
    g_wr_bit = 2;
    g_pass4 = 0;
    hw_set_capture_source(four_dev_capture_src);
    ds18b20_search_start(sink, 4);

    uint16_t guard = 0;
    for (;;) {
        if (ds18b20_search_poll()) {
            break;
        }
        if (mock_tim1.CR1 & TIM_CR1_CEN) {
            uint8_t ok = hw_run_until_uif(100);
            TEST_ASSERT_TRUE(ok);
        }
        if (++guard > 500) {
            break;
        }
    }
    TEST_ASSERT_TRUE(guard <= 500);

    TEST_ASSERT_EQUAL_UINT8(4, ds18b20_search_count());
    TEST_ASSERT_EQUAL_UINT8(4, g_found_count);
    static const uint8_t expect[4] = {0, 2, 1, 3}; /* A, C, B, D */
    for (int i = 0; i < 4; i++) {
        for (int j = 0; j < 8; j++) {
            TEST_ASSERT_EQUAL_HEX8(g_roms4[expect[i]][j], g_found_roms[i][j]);
        }
    }
}

/*-------------------------------------------------------------
 *  An idle-HIGH gap is injected after each search operation and
 *  must not break the search: the device is still found.
 * -----------------------------------------------------------*/
void test_search_gap_between_slots(void) {
    uint8_t serial[7] = {0x28, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06};
    memcpy(g_rom, serial, 7);
    g_rom[7] = ds18b20_crc8(g_rom, 7);

    g_found_count = 0;
    g_wr_bit = 2;
    hw_set_capture_source(search_capture_src);
    ds18b20_test_set_gap_us(50);
    ds18b20_search_start(sink, 1);

    uint16_t guard = 0;
    for (;;) {
        if (ds18b20_search_poll()) {
            break;
        }
        if (mock_tim1.CR1 & TIM_CR1_CEN) {
            uint8_t ok = hw_run_until_uif(100);
            TEST_ASSERT_TRUE(ok);
        }
        if (++guard > 500) {
            break;
        }
    }
    ds18b20_test_set_gap_us(0); /* leave the harness clean */
    TEST_ASSERT_TRUE(guard <= 500);

    TEST_ASSERT_EQUAL_UINT8(1, ds18b20_search_count());
    TEST_ASSERT_EQUAL_UINT8(1, g_found_count);
    for (int i = 0; i < 8; i++) {
        TEST_ASSERT_EQUAL_HEX8(g_rom[i], g_found_roms[0][i]);
    }
}

/*-------------------------------------------------------------
 *  The search and the measurement state machine share TIM1/DMA.
 *  While a search runs, ds18b20_poll() must NOT react to the
 *  search's UIF or advance the measurement state machine.
 * -----------------------------------------------------------*/
void test_search_poll_ignored_while_search_running(void) {
    uint8_t serial[7] = {0x28, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06};
    memcpy(g_rom, serial, 7);
    g_rom[7] = ds18b20_crc8(g_rom, 7);

    g_found_count = 0;
    g_wr_bit = 2;
    hw_set_capture_source(search_capture_src);
    ds18b20_search_start(sink, 1);

    /* Complete the reset so the search is mid-flight and UIF is set. */
    if (mock_tim1.CR1 & TIM_CR1_CEN) {
        TEST_ASSERT_TRUE(hw_run_until_uif(100));
    }

    /* Mid-search: poll() must not consume the UIF nor advance the state. */
    ds18b20_test_set_state(DS18B20_ST_WAIT);
    ds18b20_poll();
    TEST_ASSERT_EQUAL_UINT8(DS18B20_ST_WAIT, ds18b20_test_get_state());

    /* The search itself still consumes the same UIF and runs to completion. */
    uint16_t guard = 0;
    for (;;) {
        if (ds18b20_search_poll()) {
            break;
        }
        if (mock_tim1.CR1 & TIM_CR1_CEN) {
            TEST_ASSERT_TRUE(hw_run_until_uif(100));
        }
        if (++guard > 500) {
            break;
        }
    }
    TEST_ASSERT_TRUE(guard <= 500);
    TEST_ASSERT_EQUAL_UINT8(1, ds18b20_search_count());

    /* poll() stayed out of the way the whole time: the measurement state was
     * never advanced while the search owned the timer. */
    TEST_ASSERT_EQUAL_UINT8(DS18B20_ST_WAIT, ds18b20_test_get_state());
}

/*-------------------------------------------------------------
 *  ds18b20_search_start() while a search is already running is
 *  ignored: the running search keeps its original sink and max.
 * -----------------------------------------------------------*/
static uint8_t g_reentry_b_calls;
static uint8_t reentry_sink_b(const uint8_t* rom) {
    (void)rom;
    g_reentry_b_calls++;
    return 0;
}

void test_search_start_reentry_ignored(void) {
    uint8_t serial[7] = {0x28, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06};
    memcpy(g_rom, serial, 7);
    g_rom[7] = ds18b20_crc8(g_rom, 7);

    g_found_count = 0;
    g_wr_bit = 2;
    g_reentry_b_calls = 0;
    hw_set_capture_source(search_capture_src);
    ds18b20_search_start(sink, 1);

    /* Re-entry while running must be ignored: the second sink/max must not
     * overwrite the running search. */
    ds18b20_search_start(reentry_sink_b, 4);

    uint16_t guard = 0;
    for (;;) {
        if (ds18b20_search_poll()) {
            break;
        }
        if (mock_tim1.CR1 & TIM_CR1_CEN) {
            TEST_ASSERT_TRUE(hw_run_until_uif(100));
        }
        if (++guard > 500) {
            break;
        }
    }
    TEST_ASSERT_TRUE(guard <= 500);

    TEST_ASSERT_EQUAL_UINT8(1, ds18b20_search_count());
    TEST_ASSERT_EQUAL_UINT8(1, g_found_count); /* original sink got the device */
    TEST_ASSERT_EQUAL_UINT8(0, g_reentry_b_calls); /* second sink never called */
}

/*-------------------------------------------------------------
 *  ds18b20_search_start() mid-measurement (non-IDLE state) is
 *  ignored: nothing is scheduled and no search is started.
 * -----------------------------------------------------------*/
void test_search_start_blocked_mid_measurement(void) {
    /* Non-IDLE measurement state: the timer belongs to the measurement. */
    ds18b20_test_set_state(DS18B20_ST_CONVERT);

    g_found_count = 0;
    g_wr_bit = 2;
    ds18b20_search_start(sink, 1);

    /* No search scheduled: nothing was armed and poll reports "no search". */
    TEST_ASSERT_FALSE(mock_tim1.CR1 & TIM_CR1_CEN);
    TEST_ASSERT_EQUAL_UINT8(1, ds18b20_search_poll());
    TEST_ASSERT_EQUAL_UINT8(0, ds18b20_search_count());
    TEST_ASSERT_EQUAL_UINT8(0, g_found_count);
}

/*-------------------------------------------------------------
 *  Run all search tests
 * -----------------------------------------------------------*/
void test_search_rejected_while_txn_running(void) {
    ds18b20_init();
    ds18b20_test_reset_ctx();
    ds18b20_test_reset_txn();

    uint8_t buf[8];
    ds18b20_read_rom(buf);
    TEST_ASSERT_EQUAL_UINT8(0, ds18b20_test_get_txn_finished());

    g_found_count = 0;
    ds18b20_search_start(sink, 1);
    TEST_ASSERT_EQUAL_UINT8(1, ds18b20_search_poll()); /* no search became active */
    TEST_ASSERT_EQUAL_UINT8(0, ds18b20_search_count());

    ds18b20_test_reset_txn();
}

void run_test_search(void) {
    TEST_RUN(test_search_finds_single_device);
    TEST_RUN(test_search_command_feed_release);
    TEST_RUN(test_search_finds_different_serial);
    TEST_RUN(test_search_filters_non_ds18b20_family);
    TEST_RUN(test_search_no_device_no_presence);
    TEST_RUN(test_write_then_read_configures_registers);
    TEST_RUN(test_search_two_devices_found);
    TEST_RUN(test_search_max_zero_aborts);
    TEST_RUN(test_search_sink_early_stop);
    TEST_RUN(test_search_pair_11_terminates);
    TEST_RUN(test_search_null_sink_completes);
    TEST_RUN(test_search_rejects_bad_crc_rom);
    TEST_RUN(test_search_four_devices_found);
    TEST_RUN(test_search_gap_between_slots);
    TEST_RUN(test_search_poll_ignored_while_search_running);
    TEST_RUN(test_search_start_reentry_ignored);
    TEST_RUN(test_search_start_blocked_mid_measurement);
    TEST_RUN(test_search_rejected_while_txn_running);
}
