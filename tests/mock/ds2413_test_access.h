#ifndef DS2413_TEST_ACCESS_H
#define DS2413_TEST_ACCESS_H
#include <stdint.h>

/* Register the DS2413 driver's DMA buffers with hw_model so its
 * truncated-address resolution can find them. Call once after hw_reset_all(). */
void ds2413_test_register_buffers(void);

/* Command pulse value at bit slot i (ow_pulse_t widened to uint16_t). */
uint16_t ds2413_test_get_cmd_pulse(uint8_t i);

/* Reset the DS2413 software context to its post-init state without re-running
 * onewire_init(). For host tests that drive many independent operations. */
void ds2413_test_reset_ctx(void);

/* 1 once the driver finished the last operation (FINISHED flag). */
uint8_t ds2413_test_is_finished(void);

/* Park the shared 1-Wire search state at FINISHED so a driver operation may
 * start. For host loops that drive many operations. */
void ds2413_test_reset_search(void);

#endif /* DS2413_TEST_ACCESS_H */
