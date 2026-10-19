#ifndef DS2431_TEST_ACCESS_H
#define DS2431_TEST_ACCESS_H
#include <stdint.h>

/* Register the DS2431 driver's DMA buffers with hw_model so its
 * truncated-address resolution can find them. Call once after every
 * hw_reset_all(). */
void ds2431_test_register_buffers(void);

/* Command pulse value at bit slot i (ow_pulse_t widened to uint16_t), for
 * post-build command assertions against the driver's command table. */
uint16_t ds2431_test_get_cmd_pulse(uint8_t i);

/* Reset the DS2431 software context to its post-init state (finished, parked
 * at IDLE) without re-running onewire_init(). For host tests and fuzzers that
 * drive many independent operations. */
void ds2431_test_reset_ctx(void);

/* Latch a completed Write Scratchpad's target address and end-offset status
 * byte, so a following Read Scratchpad echo-verifies and a Copy Scratchpad is
 * accepted, without having to run the Write Scratchpad operation first. */
void ds2431_test_set_pending(uint8_t addr_lo, uint8_t addr_hi, uint8_t es);

/* Load the payload the last Write Scratchpad would have sent, so a following
 * Read Scratchpad verifies it against the read-back scratchpad data without
 * having to run the write operation first. */
void ds2431_test_set_payload(const uint8_t* data, uint8_t len);

/* 1 once the driver has finished the last operation (FINISHED flag), i.e. the
 * last ds2431_*_poll() returned 1 and the timer has been handed back. */
uint8_t ds2431_test_is_finished(void);

/* Park the shared 1-Wire search state at FINISHED (no onewire_init()) so a
 * driver operation may start. For host loops that drive many operations. */
void ds2431_test_reset_search(void);

#endif /* DS2431_TEST_ACCESS_H */