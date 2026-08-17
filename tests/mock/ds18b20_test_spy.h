#ifndef DS18B20_TEST_SPY_H
#define DS18B20_TEST_SPY_H
#include <stdint.h>

/* Shared strong overrides of the driver's weak callbacks (ds18b20_complete,
 * ds18b20_busy). The driver is compiled only once through
 * ds18b20_test_access.c, so exactly ONE test translation unit may provide the
 * strong definitions; they live here so every test can observe completions
 * and busy transitions without symbol collisions. */

#define TEST_SPY_MAX_COMPLETES 8u

extern int16_t test_spy_complete_value; /* last reported temperature/error */
extern uint8_t test_spy_complete_called; /* 1 if complete fired at least once */
extern uint8_t test_spy_complete_count; /* completions since the last reset */
extern int16_t test_spy_complete_values[TEST_SPY_MAX_COMPLETES];
extern uint8_t test_spy_complete_indices[TEST_SPY_MAX_COMPLETES]; /* ds18b20_scan_index() at callback time */
extern uint8_t test_spy_busy_calls; /* busy() invocations since the last reset */
extern unsigned test_spy_busy_last_action; /* last busy() argument */
extern void (*test_spy_on_complete_hook)(void); /* optional per-test hook invoked inside ds18b20_complete */

void test_spy_reset(void);

#endif /* DS18B20_TEST_SPY_H */
