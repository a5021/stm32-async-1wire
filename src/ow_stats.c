/**
 * @file ow_stats.c
 * @brief 1-Wire signal statistics implementation.
 * @see ow_stats.h for the public API.
 */

#include "ow_stats.h"
#include "ds18b20.h"

#ifdef OW_STATS_ENABLE

#include <string.h>

/* ---- default no-op sink ---- */

static int sink_str(const char* s) { (void)s; return 0; }
static int sink_int(int v) { (void)v; return 0; }
static int sink_hex(uint8_t b) { (void)b; return 0; }
static int sink_byte(int b) { (void)b; return 0; }
static void sink_poll(void) {}

static const ow_stats_sink_t default_sink = {
    sink_str, sink_int, sink_hex, sink_byte, sink_poll
};

static const ow_stats_sink_t* sink = &default_sink;

/* ---- private state ---- */

static ow_stats_t st;

/** Non-blocking dump state. */
static uint32_t dump_phase; /**< 0 = idle, 1+ = active */
static uint32_t dump_sensor; /**< current sensor index */

/* ---- histogram helpers ---- */

static uint8_t hist_bucket(uint8_t pulse) {
    if (pulse <= 2) return 0;
    if (pulse <= 4) return 1;
    if (pulse <= 6) return 2;
    if (pulse <= 9) return 3;
    if (pulse <= 12) return 4;
    if (pulse <= 14) return 5;
    if (pulse <= 19) return 6;
    if (pulse <= 24) return 7;
    if (pulse <= 29) return 8;
    if (pulse <= 39) return 9;
    if (pulse <= 49) return 10;
    if (pulse <= 59) return 11;
    return 12;
}

/* ---- sensor lookup ---- */

static uint8_t sensor_find_or_alloc(const uint8_t* rom) {
    if (!rom) return OW_STATS_MAX_SENSORS;
    for (uint8_t i = 0; i < st.sensor_count; i++) {
        if (memcmp(st.sensors[i].rom, rom, 8) == 0) return i;
    }
    if (st.sensor_count < OW_STATS_MAX_SENSORS) {
        uint8_t idx = st.sensor_count++;
        memcpy(st.sensors[idx].rom, rom, 8);
        st.sensors[idx].min_pulse = 0xFF;
        st.sensors[idx].max_pulse = 0;
        return idx;
    }
    return OW_STATS_MAX_SENSORS;
}

/* ---- public API ---- */

void ow_stats_set_sink(const ow_stats_sink_t* s) {
    if (s) sink = s;
}

void ow_stats_init(void) {
    memset(&st, 0, sizeof(st));
    dump_phase = 0;
}

void ow_stats_capture_pulse(const volatile uint8_t* pulse, uint8_t n,
                            const uint8_t* rom) {
    uint8_t si = sensor_find_or_alloc(rom);
    for (uint8_t i = 0; i < n; i++) {
        uint8_t p = pulse[i];
        st.histogram[hist_bucket(p)]++;
        if (si < OW_STATS_MAX_SENSORS) {
            if (p < st.sensors[si].min_pulse) st.sensors[si].min_pulse = p;
            if (p > st.sensors[si].max_pulse) st.sensors[si].max_pulse = p;
        }
    }
    if (si < OW_STATS_MAX_SENSORS) {
        st.sensors[si].count++;
    }
}

void ow_stats_count_error(int16_t error, const uint8_t* rom) {
    uint8_t si = sensor_find_or_alloc(rom);
    if (si < OW_STATS_MAX_SENSORS) {
        switch (error) {
        case DS18B20_TEMP_ERROR_CRC_FAIL:
            st.sensors[si].crc_err++;
            break;
        case DS18B20_TEMP_ERROR_NO_SENSOR:
            st.sensors[si].no_presence++;
            break;
        default:
            st.sensors[si].generic_err++;
            break;
        }
    }
    st.total_errors++;
}

void ow_stats_dump_start(void) {
    dump_phase = 1;
    dump_sensor = 0;
}

uint8_t ow_stats_dump_poll(void) {
    if (dump_phase == 0) return 1;

    /* Each poll call enqueues one section into the sink's non-blocking TX
     * buffer (typically the UART ring).  The caller drains it via the
     * sink's poll_tx, so nothing here blocks. */
    switch (dump_phase) {
    case 1:
        sink->write_str("--- stats [");
        sink->write_int((int)st.total_cycles);
        sink->write_str(" c] ---\r\n");
        dump_phase = 2;
        dump_sensor = 0;
        break;

    case 2:
        if (dump_sensor < st.sensor_count) {
            const ow_stats_sensor_t* s = &st.sensors[dump_sensor];
            for (uint32_t j = 0; j < 8; j++) {
                sink->write_hex(s->rom[j]);
                if (j != 7) sink->enqueue_byte(' ');
            }
            sink->enqueue_byte(':');
            sink->write_int((int)s->min_pulse);
            sink->enqueue_byte('-');
            sink->write_int((int)s->max_pulse);
            sink->enqueue_byte(' ');
            sink->enqueue_byte('n');
            sink->write_int((int)s->count);
            sink->enqueue_byte(' ');
            sink->enqueue_byte('e');
            sink->write_int((int)(s->crc_err + s->no_presence + s->generic_err));
            sink->write_str("\r\n");
            dump_sensor++;
        } else {
            dump_phase = 3;
        }
        break;

    case 3:
        sink->write_str("h:");
        for (uint32_t i = 0; i < OW_STATS_HIST_BUCKETS; i++) {
            if (st.histogram[i]) {
                sink->write_int((int)i);
                sink->enqueue_byte('=');
                sink->write_int((int)st.histogram[i]);
                sink->enqueue_byte(' ');
            }
        }
        sink->write_str("\r\n");
        dump_phase = 4;
        break;

    case 4:
        sink->write_str("t=");
        sink->write_int((int)st.total_cycles);
        sink->write_str("c ");
        sink->write_int((int)st.total_errors);
        sink->write_str("e\r\n");
        dump_phase = 0;
        break;
    }

    return (dump_phase == 0) ? 1 : 0;
}

void ow_stats_reset(void) {
    uint32_t n = st.sensor_count;
    uint8_t roms[OW_STATS_MAX_SENSORS][8];
    for (uint32_t i = 0; i < n; i++) {
        memcpy(roms[i], st.sensors[i].rom, 8);
    }
    memset(&st, 0, sizeof(st));
    for (uint32_t i = 0; i < n; i++) {
        memcpy(st.sensors[i].rom, roms[i], 8);
        st.sensors[i].min_pulse = 0xFF;
    }
    st.sensor_count = n;
}

uint32_t ow_stats_tick(void) {
    return ++st.total_cycles;
}

#endif /* OW_STATS_ENABLE */
