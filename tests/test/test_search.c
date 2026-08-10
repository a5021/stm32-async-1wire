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

#include "unity.h"
#include "hw_model.h"
#include "ds18b20_test_access.h"
#include "ds18b20.h"
#include "stm32f1xx.h"
#include <string.h>

#define ONE  5u
#define ZERO 60u

static uint8_t g_rom[8];
static uint8_t g_found_roms[4][8];
static uint8_t g_found_count;
static uint8_t g_wr_bit; /* bit whose pair the next merged write+read returns */

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
    uint8_t bit  = (g_wr_bit - 1u) % 8u;
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
 *  A second device type with a different serial must also be
 *  recovered correctly (ROM built the same way).
 * -----------------------------------------------------------*/
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

static uint16_t no_presence_src(uint32_t i) { (void)i; return 100u; }

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
 *  Run all search tests
 * -----------------------------------------------------------*/
void run_test_search(void) {
    TEST_RUN(test_search_finds_single_device);
    TEST_RUN(test_search_finds_different_serial);
    TEST_RUN(test_search_filters_non_ds18b20_family);
    TEST_RUN(test_search_no_device_no_presence);
}
