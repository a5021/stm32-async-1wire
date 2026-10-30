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

/* Terminal update event of the timer: sets UIF and, in One-Pulse Mode,
 * clears CEN as the silicon does. All completion paths go through here. */
void hw_tim_update_event(void);

/* Set the capture source hook: called for every capture slot (0-based index)
 * of the current operation. May be NULL (captures 0). */
typedef uint16_t (*hw_capture_fn)(uint32_t slot_index);
void hw_set_capture_source(hw_capture_fn fn);

/* Start the timer, checking the arming order on the way.
 *
 * The port starts the timer through OW_PORT_TIMER_START() so that the model
 * sees the moment the timer begins to run. It checks that the DMA channels
 * the operation arms are armed *before* that moment: the final register
 * state is the same either way, but an edge that arrives while a channel is
 * still disarmed is lost, and the first slot of the operation would
 * silently capture nothing. */
void hw_model_timer_start(void);

/* Limit the number of capture transfers the model performs in one operation.
 * 0 (the default) means unlimited: as many captures run as the DMA counter
 * scheduled, which is the historical model behaviour. A non-zero n models the
 * silicon closely for *underrun* scenarios: on both DMA controllers EN clears
 * only when the memory counter drains, so an operation that produces fewer
 * physical bus edges than the CPU armed (e.g. a no-presence reset schedules
 * OW_PORT_CAPTURE_BUF_SIZE = 2 captures but only the master-release edge
 * arrives) leaves the capture channel armed mid-transfer (CNDTR > 0, EN set)
 * when the timer operation completes. Forcing the model to take at most n
 * captures therefore leaves the channel in that exact state, so the tests can
 * assert the register state the *next* operation's DMA re-arm sees. The
 * historical instant-drain behaviour would falsify that state (CNDTR 0, EN 0)
 * and mask the very gap this API exists to expose. Both bellies of the model
 * honour it: hw_run_until_uif() skips the extra dma13_transfer() calls (never
 * performing them), and the temporal stepper reports no further capture
 * ticks — the register state after an underrun is identical in both. */
void hw_set_capture_edges(uint32_t n);

/* Register a buffer so the model can resolve the truncated 32-bit DMA
 * addresses the driver stores in CMAR back to real host pointers.
 * The exact address the driver stores must be registered (e.g. &cmd[1]
 * when the driver feeds from cmd[1]). */
void hw_register_buf(const void* ptr);

/* The buffer the feed DMA reads from and the one the capture DMA writes to,
 * resolved back to host pointers (NULL when the channel is disarmed or the
 * address is not registered). The DMA registers hold a truncated 32-bit
 * address, so a test that asserts on the register value alone cannot say
 * *which* buffer the driver pointed at; these can. */
const void* hw_feed_source(void);
const void* hw_capture_destination(void);

/* The peripheral request the channel carries: the DMAMUX channel's request
 * id on G0/G4, the GPDMA transfer register's REQSEL on H5, and 0 on the
 * families whose request map is fixed in silicon (the port programs nothing
 * there, so there is nothing to read back). */
uint32_t hw_feed_request(void);
uint32_t hw_capture_request(void);

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

/* ---------------------------------------------------------------------------
 * Low-power WFE hook (host tests)
 *
 * The low-power port sleeps in __WFE(); on the host that macro is otherwise a
 * no-op, so a still-running long stage would spin forever and the
 * ow_port_sleep_until_done() loop could never be exercised. When a build
 * defines -DMOCK_WFE_HOOK the mock __WFE() routes here, and a test can install
 * a callback (mock_wfe_set_hook) that models the TIM1 update event arriving
 * asynchronously (e.g. set UIF on the Nth wake-up) or counts the sleeps. The
 * default callback is NULL, so __WFE() stays equivalent to the plain stub.
 * ------------------------------------------------------------------------ */
void mock_wfe_hook(void);
void mock_wfe_set_hook(void (*hook)(void));

#if defined(STM32H7) && defined(DS18B20_TEST_HARNESS)
/* H7 clock-failure seam: arm the next `allow` matching waits to pass, then
 * fail one. mask/value must match the wait's (mask, value) exactly. */
void mock_wait_fail_after(uint32_t mask, uint32_t value, uint32_t allow);
uint8_t mock_wait_should_fail(uint32_t mask, uint32_t value);

/* Override the clocks reported by SystemCoreClockUpdate(). 0 means "derive
 * from OW_SYSCLK_MHZ / OW_TIM1_MHZ" (the default). */
void mock_set_clock_hz(uint32_t core_hz, uint32_t d2_hz);
#endif /* STM32H7 && DS18B20_TEST_HARNESS */

#endif /* HW_MODEL_H */
