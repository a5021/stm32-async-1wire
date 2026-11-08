/**
 * @file app_profile.h
 * @brief Optional CPU-cycle profiler for the example applications.
 *
 * Build with `-DAPP_PROFILE=1` to wrap the non-blocking driver calls. Three
 * counters are kept: the complete public poll call, the driver's real-work
 * branch, and the calls that found no work. The profiler reads a free-running
 * hardware counter, subtracts the measured read overhead and prints compact
 * `PROFILE` lines. It is diagnostic only and compiles to nothing otherwise.
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
#elif defined(OW_FAMILY_C5)
#include "stm32c5xx.h"
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
    uint32_t overflows;
} app_profile_t;

/*
 * Timer backend: DWT->CYCCNT where the core has it (M3/M4/M7/M33), TIM14 on
 * the M0 parts. Both run freely and are read as an unsigned difference; the
 * TIM14 difference is masked to 16 bits.
 */
#if defined(OW_FAMILY_F0)
#define APP_PROFILE_TIM14 1
#define APP_PROFILE_HALF 0x8000u
#else
#define APP_PROFILE_DWT 1
#define APP_PROFILE_HALF 0x80000000u
#endif

static inline uint32_t app_profile_now(void) {
#if defined(APP_PROFILE_TIM14)
    return TIM14->CNT & 0xFFFFu;
#else
    return DWT->CYCCNT;
#endif
}

static inline int app_profile_init(void) {
#if defined(APP_PROFILE_TIM14)
    RCC->APB1ENR |= RCC_APB1ENR_TIM14EN;
    (void)RCC->APB1ENR;
    TIM14->PSC = 0u;
    TIM14->ARR = 0xFFFFu;
    TIM14->EGR = TIM_EGR_UG;
    TIM14->CR1 = TIM_CR1_CEN;
#else
    CoreDebug->DEMCR |= CoreDebug_DEMCR_TRCENA_Msk;
    DWT->CYCCNT = 0u;
    DWT->CTRL |= DWT_CTRL_CYCCNTENA_Msk;
#endif
    uint32_t start = app_profile_now();
    for (volatile uint32_t i = 0u; i < 64u; i++) {
        __NOP();
    }
    return app_profile_now() != start;
}

static inline uint32_t app_profile_delta(uint32_t start, uint32_t end) {
#if defined(APP_PROFILE_TIM14)
    return (end - start) & 0xFFFFu;
#else
    return end - start;
#endif
}

static inline void app_profile_reset(app_profile_t* p) {
    p->cycles = 0u;
    p->events = 0u;
    p->worst = 0u;
    p->overflows = 0u;
}

static inline void app_profile_add(app_profile_t* p, uint32_t delta) {
    /* A difference in the upper half of the timer range means a wrap was
     * missed; drop the sample instead of publishing a short bogus value. */
    if (delta >= APP_PROFILE_HALF) {
        p->overflows++;
        return;
    }
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

static inline uint32_t app_profile_take(uint32_t start) {
    return app_profile_delta(start, app_profile_now());
}

static inline void app_profile_record(app_profile_t* p, uint32_t start) {
    app_profile_add(p, app_profile_take(start));
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
    uart_write_str(" x=");
    uart_write_int((int)p->overflows);
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
    uart_write_str(" x=");
    uart_write_int((int)p->overflows);
    uart_write_str(" a=");
    uart_write_int((int)active_ms);
    uart_write_str(" t=");
    uart_write_int((int)cycle_ms);
    uart_write_str("\r\n");
}

#endif /* APP_PROFILE */

#endif /* APP_PROFILE_H */
