#ifndef HW_MODEL_H
#define HW_MODEL_H
#include <stdint.h>

/* Zero all mock registers and model state. */
void hw_reset_all(void);

/* Effective output-compare value (CCR1 on F1, CCR3 on F0): honours the
 * output preload semantics (shadow register). */
uint16_t hw_effective_ccr1(void);

/* Simulate the timer until UIF is set (one operation) or max_slots slots have
 * elapsed. Returns 1 if UIF became set (operation complete), 0 otherwise. */
uint8_t hw_run_until_uif(uint32_t max_slots);

/* Capture source hook: called for every capture slot (0-based index)
 * of the current operation. May be NULL (captures 0). */
typedef uint16_t (*hw_capture_fn)(uint32_t slot_index);
void hw_set_capture_source(hw_capture_fn fn);

/* Register a buffer so the model can resolve the truncated 32-bit DMA
 * addresses the driver stores in CMAR back to real host pointers.
 * The exact address the driver stores must be registered (e.g. &cmd[1]
 * when the driver feeds from cmd[1]). */
void hw_register_buf(const void* ptr);

/* Log of output-compare values written by the feed DMA (per operation).
 * Sized for the longest DMA-fed write (the Match ROM resolution config
 * write: 104 slots). */
typedef struct {
    uint16_t values[128];
    uint8_t count;
} hw_ccr1_feed_log_t;
const hw_ccr1_feed_log_t* hw_ccr1_feed_log(void);

/* Number of captures (capture DMA transfers) performed during the last operation. */
uint32_t hw_capture_count(void);

#endif /* HW_MODEL_H */
