#ifndef OW_STATS_TEST_ACCESS_H
#define OW_STATS_TEST_ACCESS_H
/* Access to ow_stats internals: implemented in ow_stats_test_access.c which
 * #includes src/ow_stats.c, so the module is compiled ONLY through that TU. */
#include "ow_stats.h"
#include <stdint.h>

const ow_stats_sensor_t* ow_stats_test_get_sensor(uint8_t index);
uint16_t ow_stats_test_get_histogram(uint8_t bucket);
uint8_t ow_stats_test_get_sensor_count(void);
uint16_t ow_stats_test_get_total_cycles(void);
uint16_t ow_stats_test_get_total_errors(void);
uint8_t ow_stats_test_get_dump_phase(void);
uint8_t ow_stats_test_get_dump_sensor(void);
uint8_t ow_stats_test_get_dump_subpos(void);

#endif /* OW_STATS_TEST_ACCESS_H */
