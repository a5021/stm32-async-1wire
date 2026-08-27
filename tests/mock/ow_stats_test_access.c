/* Single translation unit that compiles the ow_stats module together with
 * test accessors. Build ONLY this TU for ow_stats; do not compile
 * src/ow_stats.c separately, or its static state would not be shared
 * with the accessors. */
#define HOST_BUILD 1
#define OW_STATS_ENABLE 1
#include "ow_stats_test_access.h"
#include "../src/ow_stats.c"

const ow_stats_sensor_t* ow_stats_test_get_sensor(uint8_t index) {
    if (index >= OW_STATS_MAX_SENSORS) {
        return NULL;
    }
    return &st.sensors[index];
}

uint16_t ow_stats_test_get_histogram(uint8_t bucket) {
    if (bucket >= OW_STATS_HIST_BUCKETS) {
        return 0;
    }
    return st.histogram[bucket];
}

uint8_t ow_stats_test_get_sensor_count(void) { return st.sensor_count; }
uint16_t ow_stats_test_get_total_cycles(void) { return st.total_cycles; }
uint16_t ow_stats_test_get_total_errors(void) { return st.total_errors; }
uint8_t ow_stats_test_get_dump_phase(void) { return dump_phase; }
uint8_t ow_stats_test_get_dump_sensor(void) { return dump_sensor; }
