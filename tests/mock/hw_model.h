#ifndef HW_MODEL_H
#define HW_MODEL_H
#include <stdint.h>

/* Zero all mock registers and model state. */
void hw_reset_all(void);

/* Effective output-compare value on the shared TIM1 output channel
 * CH3/CCR3 (same role on both backends): honours the output preload
 * semantics (shadow register). */
uint16_t hw_effective_ccr3(void);

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
    uint8_t count; /* number of entries recorded in values[] (caps at 128) */
    uint32_t total; /* total transfers performed, not capped */
} hw_ccr3_feed_log_t;
const hw_ccr3_feed_log_t* hw_ccr3_feed_log(void);

/* Number of captures (capture DMA transfers) performed during the last operation. */
uint32_t hw_capture_count(void);

/* ---------------------------------------------------------------------------
 * Temporal TIM/DMA event model
 *
 * hw_run_until_uif() proves the memory-side DMA contract, but it fires the CC2
 * feed DMA once per slot *at the slot start* ("modeled at slot start for
 * simplicity"). Real hardware is different: the slot-end marker compare (CC2,
 * programmed at ONE+ZERO µs) triggers the DMA request, so the CCR3 reload for
 * slot N+1 happens only AFTER slot N's pulse has completed. The temporal
 * stepper below places each event at its physical counter position inside the
 * slot period, so tests can prove the TIM/DMA *temporal* contract:
 *   - CCR3(slot N) stays in effect for the whole of slot N;
 *   - the reload happens only after the CC2 compare, never at the slot start;
 *   - the trailing bus-release zero is applied only after the last slot.
 * ------------------------------------------------------------------------ */
typedef enum {
    HW_TIM_EV_CC2, /* CCR2 compare match (end-of-slot marker); reload pending */
    HW_TIM_EV_FEED, /* CC2-triggered feed DMA: CCR3 reloaded with the next pulse */
    HW_TIM_EV_CAPTURE, /* CC4 capture: CCR4 <- counter, one capture DMA to memory */
    HW_TIM_EV_UPDATE, /* ARR overflow, non-terminal: slot boundary */
    HW_TIM_EV_TERMINAL, /* terminal update: UIF set, OPM stops the timer */
    HW_TIM_EV_IDLE, /* timer not running or operation already finished */
} hw_tim_event_t;

/* Snapshot the timer+DMA configuration the driver programmed and position
 * the model at tick 0 of slot 0. The initial active output value is taken
 * from CCR3 (correct for the non-preload paths). For OC3PE (preload) paths
 * the active value lives in the shadow register, which the driver's register
 * writes cannot reveal afterwards: pass it explicitly via
 * hw_tim_init_shadow(). */
void hw_tim_init(void);
void hw_tim_init_shadow(uint16_t init_shadow);

/* Advance to the next temporal event in physical order (IDLE once the timer
 * stops). The state is exposed through the hw_tim_* getters; the model also
 * advances the mock registers (CCR3/CCR4/CCR, CNDTR, SR, CR1) so tests can
 * observe the DMA registers as hardware would. */
hw_tim_event_t hw_tim_step(void);

uint32_t hw_tim_period(void); /* current slot, 0-based (0..hw_tim_slots()-1) */
uint32_t hw_tim_tick(void); /* current counter position within the slot */
uint16_t hw_tim_active(void); /* output value in effect at this counter position */
uint16_t hw_tim_ccr3(void); /* CCR3 preload register value */
uint16_t hw_tim_ccr4(void); /* CCR4 capture register value */
uint32_t hw_tim_slots(void); /* total slots in the scheduled operation */

#endif /* HW_MODEL_H */
