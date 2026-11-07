/**
 * @file app_profile.h
 * @brief Optional CPU-cycle profiler for the example applications.
 *
 * Build with `-DAPP_PROFILE=1` to wrap the non-blocking driver calls. The
 * profiler reads SysTick->VAL around each call, subtracts the measured read
 * overhead and prints one compact `PROFILE` line per search, measurement or
 * resolution change. It is diagnostic only and compiles to nothing otherwise.
 */

#ifndef APP_PROFILE_H
#define APP_PROFILE_H

#include "app.h"
#if defined(OW_FAMILY_F0)
#include "stm32f0xx.h"
#elif defined(OW_FAMILY_F3)
#include "stm32f3xx.h"
#elif defined(OW_FAMILY_F4)
#include "stm32f4xx.h"
#elif defined(OW_FAMILY_G0)
#include "stm32g0xx.h"
#elif defined(OW_FAMILY_G4)
#include "stm32g4xx.h"
#elif defined(OW_FAMILY_L4)
#include "stm32l4xx.h"
#elif defined(OW_FAMILY_H5)
#include "stm32h5xx.h"
#elif defined(OW_FAMILY_H7)
#include "stm32h7xx.h"
#include "system_stm32h7xx.h"
#else
#include "stm32f1xx.h"
#endif

#ifndef APP_PROFILE
#define APP_PROFILE 0
#endif

#if APP_PROFILE

#ifdef OW_PROFILE
void ow_profile_work_begin(void);
void ow_profile_work_end(void);
#endif

typedef struct {
    uint32_t cycles;
    uint32_t events;
    uint32_t worst;
    uint32_t overhead;
} app_profile_t;

static inline uint32_t app_profile_now(void) {
    return SysTick->VAL;
}

static inline uint32_t app_profile_delta(uint32_t start, uint32_t end) {
    /* SysTick counts down. A driver poll is far shorter than one 1 ms reload,
     * so at most one wrap has to be folded in. */
    uint32_t period = SysTick->LOAD + 1u;
    return start >= end ? (start - end) : ((period - end) + start);
}

static inline void app_profile_reset(app_profile_t* p) {
    p->cycles = 0u;
    p->events = 0u;
    p->worst = 0u;
}

static inline void app_profile_record(app_profile_t* p, uint32_t start) {
    uint32_t delta = app_profile_delta(start, app_profile_now());
    if (delta > p->overhead) {
        delta -= p->overhead;
    } else {
        delta = 0u;
    }
    p->cycles += delta;
    p->events++;
    if (delta > p->worst) {
        p->worst = delta;
    }
}

static inline uint32_t app_profile_calibrate(void) {
    app_profile_t p = {0};
    for (uint32_t i = 0u; i < 1024u; i++) {
        uint32_t start = app_profile_now();
        app_profile_record(&p, start);
    }
    return p.events ? (p.cycles / p.events) : 0u;
}

static app_profile_t* app_profile_work_current;
static uint32_t app_profile_work_start;

#ifdef OW_PROFILE
void ow_profile_work_begin(void) {
    app_profile_work_start = app_profile_now();
}

void ow_profile_work_end(void) {
    if (app_profile_work_current) {
        app_profile_record(app_profile_work_current, app_profile_work_start);
    }
}
#endif

static inline void app_profile_select(app_profile_t* p) {
    app_profile_work_current = p;
}

static inline void app_profile_print_cal(uint32_t overhead) {
    uart_write_str("PROFILE cal o=");
    uart_write_int((int)overhead);
    uart_write_str("\r\n");
}

static inline void app_profile_print(const char* tag, const app_profile_t* p) {
    uart_write_str("PROFILE ");
    uart_write_str(tag);
    uart_write_str(" c=");
    uart_write_int((int)p->cycles);
    uart_write_str(" p=");
    uart_write_int((int)p->events);
    uart_write_str(" w=");
    uart_write_int((int)p->worst);
    uart_write_str("\r\n");
}

static inline void app_profile_print_timed(const char* tag,
                                           const app_profile_t* p,
                                           uint32_t active_ms,
                                           uint32_t cycle_ms) {
    uart_write_str("PROFILE ");
    uart_write_str(tag);
    uart_write_str(" c=");
    uart_write_int((int)p->cycles);
    uart_write_str(" p=");
    uart_write_int((int)p->events);
    uart_write_str(" w=");
    uart_write_int((int)p->worst);
    uart_write_str(" a=");
    uart_write_int((int)active_ms);
    uart_write_str(" t=");
    uart_write_int((int)cycle_ms);
    uart_write_str("\r\n");
}

#endif /* APP_PROFILE */

#endif /* APP_PROFILE_H */
