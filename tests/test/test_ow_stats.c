#include "ds18b20.h"
#include "ow_stats_test_access.h"
#include "unity.h"

/* ---- capture sink: routes dump output into dump_buf for assertions ---- */
static char dump_buf[512];
static uint32_t dump_len;

static int dump_sink_str(const char* s) {
    int n = 0;
    while (*s && dump_len < sizeof(dump_buf)) {
        dump_buf[dump_len++] = *s++;
        n++;
    }
    return n;
}

static int dump_sink_int(int v) {
    char tmp[12];
    int i = 0;
    int n = 0;
    int j;
    unsigned int u = (v < 0) ? (unsigned int)-(v + 1) + 1u : (unsigned int)v;
    if (v == 0) return dump_sink_str("0");
    while (u > 0) {
        tmp[i++] = (char)('0' + (u % 10));
        u /= 10;
    }
    if (v < 0) tmp[i++] = '-';
    for (j = i - 1; j >= 0; j--) {
        if (dump_len < sizeof(dump_buf)) {
            dump_buf[dump_len++] = tmp[j];
            n++;
        }
    }
    return n;
}

static int dump_sink_hex(uint8_t b) {
    static const char hex[] = "0123456789ABCDEF";
    int n = 0;
    if (dump_len < sizeof(dump_buf)) {
        dump_buf[dump_len++] = hex[(b >> 4) & 0x0F];
        n++;
    }
    if (dump_len < sizeof(dump_buf)) {
        dump_buf[dump_len++] = hex[b & 0x0F];
        n++;
    }
    return n;
}

static int dump_sink_byte(int b) {
    if (dump_len < sizeof(dump_buf)) dump_buf[dump_len++] = (char)b;
    return 1;
}

static void dump_sink_poll(void) {}

static const ow_stats_sink_t dump_sink = {
    dump_sink_str, dump_sink_int, dump_sink_hex, dump_sink_byte, dump_sink_poll};

/* ---- init ---- */
void test_ow_stats_init_zeroes_all(void) {
    /* Populate some state first */
    static const uint8_t rom1[] = {0x28, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07};
    static volatile uint8_t pulse[] = {10, 10, 10, 10, 10, 10, 10, 10};
    ow_stats_capture_pulse(pulse, 8, rom1);
    ow_stats_count_error(DS18B20_TEMP_ERROR_CRC_FAIL, rom1);
    ow_stats_tick();

    ow_stats_init();

    TEST_ASSERT_EQUAL_UINT8(0, ow_stats_test_get_sensor_count());
    TEST_ASSERT_EQUAL_UINT32(0, ow_stats_test_get_total_cycles());
    TEST_ASSERT_EQUAL_UINT32(0, ow_stats_test_get_total_errors());
    for (uint8_t i = 0; i < OW_STATS_HIST_BUCKETS; i++) {
        TEST_ASSERT_EQUAL_UINT32(0, ow_stats_test_get_histogram(i));
    }
}

/* ---- tick ---- */
void test_ow_stats_tick_increments(void) {
    ow_stats_init();
    TEST_ASSERT_EQUAL_UINT32(1, ow_stats_tick());
    TEST_ASSERT_EQUAL_UINT32(2, ow_stats_tick());
    TEST_ASSERT_EQUAL_UINT32(3, ow_stats_tick());
}

/* ---- counters must not overflow past UINT16 (long stats window) ---- */
void test_ow_stats_counters_no_overflow(void) {
    ow_stats_init();
    static const uint8_t rom[] = {0x28, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07};
    static volatile uint8_t p[] = {10, 10, 10, 10, 10, 10, 10, 10};

    for (int i = 0; i < 70000; i++) {
        ow_stats_tick();
        ow_stats_capture_pulse(p, 8, rom);
    }

    TEST_ASSERT_EQUAL_UINT32(70000u, ow_stats_test_get_total_cycles());
    const ow_stats_sensor_t* s = ow_stats_test_get_sensor(0);
    TEST_ASSERT_TRUE(s != NULL);
    TEST_ASSERT_EQUAL_UINT32(70000u, s->count);
}

/* ---- histogram buckets ---- */
void test_ow_stats_capture_pulse_bucket_0_2(void) {
    ow_stats_init();
    static volatile uint8_t pulse[] = {2, 1, 0};
    ow_stats_capture_pulse(pulse, 3, NULL);
    TEST_ASSERT_EQUAL_UINT32(3, ow_stats_test_get_histogram(0));
    TEST_ASSERT_EQUAL_UINT32(0, ow_stats_test_get_histogram(1));
}

void test_ow_stats_capture_pulse_bucket_3_4(void) {
    ow_stats_init();
    static volatile uint8_t pulse[] = {3, 4};
    ow_stats_capture_pulse(pulse, 2, NULL);
    TEST_ASSERT_EQUAL_UINT32(2, ow_stats_test_get_histogram(1));
}

void test_ow_stats_capture_pulse_boundary_values(void) {
    ow_stats_init();
    /* Boundaries: 2→b0, 3→b1, 4→b1, 5→b2, 6→b2, 7→b3, 9→b3,
     * 10→b4, 12→b4, 13→b5, 14→b5, 15→b6, 19→b6, 20→b7, 24→b7,
     * 25→b8, 29→b8, 30→b9, 39→b9, 40→b10, 49→b10, 50→b11, 59→b11,
     * 60→b12, 100→b12 */
    static volatile uint8_t pulse[] = {2, 3, 4, 5, 6, 7, 9, 10, 12, 13,
                                       14, 15, 19, 20, 24, 25, 29, 30, 39,
                                       40, 49, 50, 59, 60, 100};
    ow_stats_capture_pulse(pulse, 25, NULL);

    TEST_ASSERT_EQUAL_UINT32(1, ow_stats_test_get_histogram(0)); /* 2 */
    TEST_ASSERT_EQUAL_UINT32(2, ow_stats_test_get_histogram(1)); /* 3,4 */
    TEST_ASSERT_EQUAL_UINT32(2, ow_stats_test_get_histogram(2)); /* 5,6 */
    TEST_ASSERT_EQUAL_UINT32(2, ow_stats_test_get_histogram(3)); /* 7,9 */
    TEST_ASSERT_EQUAL_UINT32(2, ow_stats_test_get_histogram(4)); /* 10,12 */
    TEST_ASSERT_EQUAL_UINT32(2, ow_stats_test_get_histogram(5)); /* 13,14 */
    TEST_ASSERT_EQUAL_UINT32(2, ow_stats_test_get_histogram(6)); /* 15,19 */
    TEST_ASSERT_EQUAL_UINT32(2, ow_stats_test_get_histogram(7)); /* 20,24 */
    TEST_ASSERT_EQUAL_UINT32(2, ow_stats_test_get_histogram(8)); /* 25,29 */
    TEST_ASSERT_EQUAL_UINT32(2, ow_stats_test_get_histogram(9)); /* 30,39 */
    TEST_ASSERT_EQUAL_UINT32(2, ow_stats_test_get_histogram(10)); /* 40,49 */
    TEST_ASSERT_EQUAL_UINT32(2, ow_stats_test_get_histogram(11)); /* 50,59 */
    TEST_ASSERT_EQUAL_UINT32(2, ow_stats_test_get_histogram(12)); /* 60,100 */
}

void test_ow_stats_capture_pulse_above_60(void) {
    ow_stats_init();
    static volatile uint8_t pulse[] = {80, 120, 200};
    ow_stats_capture_pulse(pulse, 3, NULL);
    TEST_ASSERT_EQUAL_UINT32(3, ow_stats_test_get_histogram(12));
    TEST_ASSERT_EQUAL_UINT32(0, ow_stats_test_get_histogram(0));
}

/* ---- per-sensor min/max/count ---- */
void test_ow_stats_capture_pulse_tracks_min_max(void) {
    ow_stats_init();
    static const uint8_t rom[] = {0x28, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07};
    static volatile uint8_t pulse1[] = {10, 10, 10, 10, 10, 10, 10, 10};
    static volatile uint8_t pulse2[] = {20, 20, 20, 20, 20, 20, 20, 20};
    static volatile uint8_t pulse3[] = {15, 15, 15, 15, 15, 15, 15, 15};

    ow_stats_capture_pulse(pulse1, 8, rom);
    ow_stats_capture_pulse(pulse2, 8, rom);
    ow_stats_capture_pulse(pulse3, 8, rom);

    const ow_stats_sensor_t* s = ow_stats_test_get_sensor(0);
    TEST_ASSERT_TRUE(s != NULL);
    TEST_ASSERT_EQUAL_UINT8(10, s->min_pulse);
    TEST_ASSERT_EQUAL_UINT8(20, s->max_pulse);
    TEST_ASSERT_EQUAL_UINT32(3, s->count);
}

/* ---- rom = NULL → only histogram, no sensor allocation ---- */
void test_ow_stats_capture_pulse_rom_null(void) {
    ow_stats_init();
    static volatile uint8_t pulse[] = {10, 10, 10};
    ow_stats_capture_pulse(pulse, 3, NULL);
    TEST_ASSERT_EQUAL_UINT8(0, ow_stats_test_get_sensor_count());
    TEST_ASSERT_EQUAL_UINT32(3, ow_stats_test_get_histogram(4));
}

/* ---- duplicate ROM → same sensor, count incremented ---- */
void test_ow_stats_capture_pulse_duplicate_rom(void) {
    ow_stats_init();
    static const uint8_t rom[] = {0x28, 0x10, 0x20, 0x30, 0x40, 0x50, 0x60, 0x70};
    static volatile uint8_t p1[] = {5, 5, 5, 5, 5, 5, 5, 5};
    static volatile uint8_t p2[] = {10, 10, 10, 10, 10, 10, 10, 10};

    ow_stats_capture_pulse(p1, 8, rom);
    ow_stats_capture_pulse(p2, 8, rom);

    TEST_ASSERT_EQUAL_UINT8(1, ow_stats_test_get_sensor_count());
    const ow_stats_sensor_t* s = ow_stats_test_get_sensor(0);
    TEST_ASSERT_EQUAL_UINT32(2, s->count);
    TEST_ASSERT_EQUAL_UINT8(5, s->min_pulse);
    TEST_ASSERT_EQUAL_UINT8(10, s->max_pulse);
}

/* ---- 9+ different ROMs → max 8 sensors ---- */
void test_ow_stats_capture_pulse_overflow_8(void) {
    ow_stats_init();
    for (uint8_t i = 0; i < 9; i++) {
        uint8_t rom[8] = {0x28, i, 0, 0, 0, 0, 0, 0};
        static volatile uint8_t p[] = {10, 10, 10, 10, 10, 10, 10, 10};
        ow_stats_capture_pulse(p, 8, rom);
    }
    TEST_ASSERT_EQUAL_UINT8(OW_STATS_MAX_SENSORS, ow_stats_test_get_sensor_count());
}

/* ---- count_error: CRC ---- */
void test_ow_stats_count_error_crc(void) {
    ow_stats_init();
    static const uint8_t rom[] = {0x28, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07};
    ow_stats_count_error(DS18B20_TEMP_ERROR_CRC_FAIL, rom);

    const ow_stats_sensor_t* s = ow_stats_test_get_sensor(0);
    TEST_ASSERT_EQUAL_UINT32(1, s->crc_err);
    TEST_ASSERT_EQUAL_UINT32(0, s->no_presence);
    TEST_ASSERT_EQUAL_UINT32(0, s->generic_err);
    TEST_ASSERT_EQUAL_UINT32(1, ow_stats_test_get_total_errors());
}

/* ---- count_error: no sensor ---- */
void test_ow_stats_count_error_no_sensor(void) {
    ow_stats_init();
    static const uint8_t rom[] = {0x28, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08};
    ow_stats_count_error(DS18B20_TEMP_ERROR_NO_SENSOR, rom);

    const ow_stats_sensor_t* s = ow_stats_test_get_sensor(0);
    TEST_ASSERT_EQUAL_UINT32(0, s->crc_err);
    TEST_ASSERT_EQUAL_UINT32(1, s->no_presence);
    TEST_ASSERT_EQUAL_UINT32(0, s->generic_err);
}

/* ---- count_error: generic ---- */
void test_ow_stats_count_error_generic(void) {
    ow_stats_init();
    static const uint8_t rom[] = {0x28, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09};
    ow_stats_count_error(DS18B20_TEMP_ERROR_GENERIC, rom);

    const ow_stats_sensor_t* s = ow_stats_test_get_sensor(0);
    TEST_ASSERT_EQUAL_UINT32(0, s->crc_err);
    TEST_ASSERT_EQUAL_UINT32(0, s->no_presence);
    TEST_ASSERT_EQUAL_UINT32(1, s->generic_err);
}

/* ---- reset: zeros counters, preserves ROM table ---- */
void test_ow_stats_reset_preserves_rom(void) {
    ow_stats_init();
    static const uint8_t rom[] = {0x28, 0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF, 0x11};
    static volatile uint8_t p[] = {10, 10, 10, 10, 10, 10, 10, 10};
    ow_stats_capture_pulse(p, 8, rom);
    ow_stats_count_error(DS18B20_TEMP_ERROR_CRC_FAIL, rom);
    ow_stats_tick();

    ow_stats_reset();

    TEST_ASSERT_EQUAL_UINT32(0, ow_stats_test_get_total_cycles());
    TEST_ASSERT_EQUAL_UINT32(0, ow_stats_test_get_total_errors());
    const ow_stats_sensor_t* s = ow_stats_test_get_sensor(0);
    TEST_ASSERT_EQUAL_UINT32(0, s->count);
    TEST_ASSERT_EQUAL_UINT8(0xFF, s->min_pulse);
    TEST_ASSERT_EQUAL_UINT8(0, s->max_pulse);
    TEST_ASSERT_EQUAL_UINT32(0, s->crc_err);
    /* ROM preserved */
    for (uint8_t i = 0; i < 8; i++) {
        TEST_ASSERT_EQUAL_UINT8(rom[i], s->rom[i]);
    }
    TEST_ASSERT_EQUAL_UINT8(1, ow_stats_test_get_sensor_count());
}

/* ---- dump_start sets phase ---- */
void test_ow_stats_dump_start_sets_phase(void) {
    ow_stats_init();
    ow_stats_set_sink(&dump_sink);
    dump_len = 0;
    ow_stats_dump_start();
    TEST_ASSERT_EQUAL_UINT8(1, ow_stats_test_get_dump_phase());
}

/* ---- dump_poll completes ---- */
void test_ow_stats_dump_poll_completes(void) {
    ow_stats_init();
    ow_stats_set_sink(&dump_sink);
    dump_len = 0;
    /* Add some data */
    static const uint8_t rom[] = {0x28, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07};
    static volatile uint8_t p[] = {10, 10, 10, 10, 10, 10, 10, 10};
    ow_stats_capture_pulse(p, 8, rom);
    ow_stats_count_error(DS18B20_TEMP_ERROR_CRC_FAIL, rom);
    ow_stats_tick();

    ow_stats_dump_start();
    /* Run dump_poll until it returns 1 (complete) or bail after 200 iterations */
    uint8_t done = 0;
    for (int i = 0; i < 200; i++) {
        if (ow_stats_dump_poll()) {
            done = 1;
            break;
        }
    }
    TEST_ASSERT_EQUAL_UINT8(1, done);
    TEST_ASSERT_EQUAL_UINT8(0, ow_stats_test_get_dump_phase());
    /* The dump must actually write through the sink. */
    TEST_ASSERT_TRUE(dump_len > 0);
}

void run_test_ow_stats(void) {
    TEST_RUN(test_ow_stats_init_zeroes_all);
    TEST_RUN(test_ow_stats_tick_increments);
    TEST_RUN(test_ow_stats_counters_no_overflow);
    TEST_RUN(test_ow_stats_capture_pulse_bucket_0_2);
    TEST_RUN(test_ow_stats_capture_pulse_bucket_3_4);
    TEST_RUN(test_ow_stats_capture_pulse_boundary_values);
    TEST_RUN(test_ow_stats_capture_pulse_above_60);
    TEST_RUN(test_ow_stats_capture_pulse_tracks_min_max);
    TEST_RUN(test_ow_stats_capture_pulse_rom_null);
    TEST_RUN(test_ow_stats_capture_pulse_duplicate_rom);
    TEST_RUN(test_ow_stats_capture_pulse_overflow_8);
    TEST_RUN(test_ow_stats_count_error_crc);
    TEST_RUN(test_ow_stats_count_error_no_sensor);
    TEST_RUN(test_ow_stats_count_error_generic);
    TEST_RUN(test_ow_stats_reset_preserves_rom);
    TEST_RUN(test_ow_stats_dump_start_sets_phase);
    TEST_RUN(test_ow_stats_dump_poll_completes);
}
