#include "hw_model.h"
#include "mock_target.h"
#include <stdio.h>

TIM1_TypeDef mock_tim1;
DMA1_Channel_TypeDef mock_dma1_ch4;
DMA1_Channel_TypeDef mock_feed_ch;
GPIO_TypeDef mock_gpioa;
RCC_TypeDef mock_rcc;
USART_TypeDef mock_usart1;
#ifdef OW_PORT_LOW_POWER
SCB_Type mock_scb; /* low-power WFE path: SEVONPEND lives in SCB.SCR */
#endif
#if defined(OW_PORT_TARGET_G0)
SYSCFG_TypeDef mock_syscfg; /* G0 backend only */
DMAMUX_Channel_TypeDef mock_dmamux_ch2; /* G0 backend only */
DMAMUX_Channel_TypeDef mock_dmamux_ch3; /* G0 backend only */
#endif

static uint16_t tim_shadow_out;
static hw_capture_fn capture_source;
static hw_ccr3_feed_log_t feed_log;
static uint32_t op_capture_count;

/* --- truncated 32-bit DMA address -> real host pointer table --- */
#define HW_ADDR_TABLE_MAX 32
static const void* addr_table[HW_ADDR_TABLE_MAX];
static uint32_t addr_lo[HW_ADDR_TABLE_MAX];
static uint8_t addr_count;

void hw_register_buf(const void* ptr) {
    if (addr_count < HW_ADDR_TABLE_MAX) {
        addr_lo[addr_count] = (uint32_t)(uintptr_t)ptr;
        addr_table[addr_count] = ptr;
        addr_count++;
    }
}

static void* hw_resolve(uint32_t lo) {
    for (uint8_t i = 0; i < addr_count; i++) {
        if (addr_lo[i] == lo) {
            return (void*)addr_table[i];
        }
    }
    return NULL;
}

void hw_reset_all(void) {
    mock_tim1 = (TIM1_TypeDef){0};
    mock_dma1_ch4 = (DMA1_Channel_TypeDef){0};
    mock_feed_ch = (DMA1_Channel_TypeDef){0};
    mock_gpioa = (GPIO_TypeDef){0};
    mock_rcc = (RCC_TypeDef){0};
    mock_usart1 = (USART_TypeDef){0};
#ifdef OW_PORT_LOW_POWER
    mock_scb = (SCB_Type){0};
#endif
    /* USART TXE is set by hardware when the transmit buffer is empty —
     * that is the reset/power-on state.  Pre-set it so ow_tx_char() does
     * not spin-wait in host tests. */
#if defined(OW_PORT_TARGET_G0) || defined(OW_PORT_TARGET_F0)
    mock_usart1.ISR = 0x00000080u; /* USART_ISR_TXE / USART_ISR_TXE_TXFNF */
#else
    mock_usart1.SR = 0x00000080u; /* USART_SR_TXE */
#endif
#if defined(OW_PORT_TARGET_G0)
    mock_syscfg = (SYSCFG_TypeDef){0};
    mock_dmamux_ch2 = (DMAMUX_Channel_TypeDef){0};
    mock_dmamux_ch3 = (DMAMUX_Channel_TypeDef){0};
#endif
    tim_shadow_out = 0;
    capture_source = NULL;
    feed_log.count = 0;
    op_capture_count = 0;
    addr_count = 0;
}

void hw_set_capture_source(hw_capture_fn fn) { capture_source = fn; }

const hw_ccr3_feed_log_t* hw_ccr3_feed_log(void) { return &feed_log; }

uint32_t hw_capture_count(void) { return op_capture_count; }

uint16_t hw_effective_ccr3(void) {
    if (MOCK_TIM_OUT_CCMR & MOCK_TIM_OUT_PE) {
        return tim_shadow_out;
    }
    return (uint16_t)MOCK_TIM_OUT_CCR;
}

/* Resolved buffer pointers for the current operation (set by hw_run_until_uif). */
static uint8_t* d16_cur; /* feed DMA source (memory read) */
static uint8_t* d13_cur; /* capture DMA destination (memory write) */

/* One feed transfer: memory -> output CCR (16-bit peripheral, 8-bit memory). */
static void dma16_transfer(void) {
    DMA1_Channel_TypeDef* d = &mock_feed_ch;
    if (!(d->CCR & DMA_CCR_EN) || d->CNDTR == 0) {
        return;
    }
    if (d16_cur == NULL) {
        fprintf(stderr, "hw_model: unresolved feed source address\n");
        d->CNDTR = 0;
        d->CCR &= ~DMA_CCR_EN;
        return;
    }
    uint16_t val = *d16_cur;
    MOCK_TIM_OUT_CCR = val;
    d16_cur += 1; /* MSIZE 8-bit */
    d->CNDTR--;
    if (d->CNDTR == 0) {
        d->CCR &= ~DMA_CCR_EN;
    }
    if (feed_log.count < 128u) {
        feed_log.values[feed_log.count++] = val;
    }
}

/* One capture transfer: capture CCR -> memory (MSIZE 8 or 16 per DMA config). */
static void dma13_transfer(void) {
    DMA1_Channel_TypeDef* d = &mock_dma1_ch4;
    if (!(d->CCR & DMA_CCR_EN) || d->CNDTR == 0) {
        return;
    }
    if (d13_cur == NULL) {
        fprintf(stderr, "hw_model: unresolved capture destination address\n");
        d->CNDTR = 0;
        d->CCR &= ~DMA_CCR_EN;
        return;
    }
    uint16_t val = (uint16_t)MOCK_TIM_CAP_CCR;
    if (d->CCR & DMA_CCR_MSIZE_0) {
        *(volatile uint16_t*)d13_cur = val;
        d13_cur += 2;
    } else {
        *(volatile uint8_t*)d13_cur = (uint8_t)val;
        d13_cur += 1;
    }
    d->CNDTR--;
    if (d->CNDTR == 0) {
        d->CCR &= ~DMA_CCR_EN;
    }
}

uint8_t hw_run_until_uif(uint32_t max_slots) {
    TIM1_TypeDef* t = &mock_tim1;
    if (!(t->CR1 & TIM_CR1_CEN)) {
        return (t->SR & TIM_SR_UIF) ? 1u : 0u;
    }
    feed_log.count = 0;
    op_capture_count = 0;
    /* Resolve the DMA buffer addresses exactly as the driver stored them. */
    d16_cur = (uint8_t*)hw_resolve((uint32_t)mock_feed_ch.CMAR);
    d13_cur = (uint8_t*)hw_resolve((uint32_t)mock_dma1_ch4.CMAR);
    uint32_t slots = (uint32_t)(t->RCR & 0xFFu) + 1u;
    if (slots > max_slots) {
        slots = max_slots;
    }
    /* captures per slot: ceil(CNDTR / slots) — e.g. reset = 2 in 1 slot. */
    uint32_t cps = 0;
    if ((t->DIER & MOCK_TIM_CAP_DE) && (mock_dma1_ch4.CCR & DMA_CCR_EN) && mock_dma1_ch4.CNDTR > 0) {
        uint32_t n = mock_dma1_ch4.CNDTR;
        uint32_t s = (uint32_t)(t->RCR & 0xFFu) + 1u;
        cps = (n + s - 1u) / s;
    }
    for (uint32_t i = 0; i < slots; i++) {
        /* Slot-end marker compare event -> feed DMA reloads the output CCR.
         * Modeled at slot start for simplicity; the ordering does not affect
         * the tested invariants. */
        if (t->DIER & MOCK_TIM_FEED_DE) {
            dma16_transfer();
        }
        /* Capture event -> capture value + capture DMA to memory. */
        if (t->DIER & MOCK_TIM_CAP_DE) {
            for (uint32_t c = 0; c < cps; c++) {
                uint16_t cap = capture_source ? capture_source(op_capture_count) : 0u;
                MOCK_TIM_CAP_CCR = cap;
                dma13_transfer();
                op_capture_count++;
            }
        }
        if (i == (uint32_t)(t->RCR & 0xFFu)) {
            /* terminal update event: UIF, OPM stop, preload -> shadow. */
            t->SR |= TIM_SR_UIF;
            if (t->CR1 & TIM_CR1_OPM) {
                t->CR1 &= (uint32_t)~TIM_CR1_CEN;
            }
            tim_shadow_out = (uint16_t)MOCK_TIM_OUT_CCR;
            return 1u;
        }
    }
    return 0u;
}

/* ======================================================================
 * Temporal TIM/DMA event model
 *
 * Discrete-event stepper that places each event at its physical counter
 * position inside a slot period. Unlike hw_run_until_uif() (which fires the
 * feed DMA once per slot at the slot START), this model mirrors real TIM1
 * behaviour: the CC2 compare at ONE+ZERO µs is the end-of-slot marker whose
 * DMA request reloads CCR3 for the *next* slot, i.e. only after slot N's
 * pulse has completed. See tests/test/test_tim_model.c for the contract it
 * proves (CCR3 held per slot, reload only after CC2, trailing 0 last).
 * ==================================================================== */

typedef struct {
    uint16_t arr; /* auto-reload value (slot period, µs) */
    uint16_t ccr2; /* slot-end marker compare value (ONE+ZERO µs) */
    uint8_t pe; /* OC3PE: output-compare preload active */
    uint8_t feed_en; /* CC2DE + feed channel EN && CNDTR > 0 */
    uint8_t cap_en; /* CC4DE + capture channel EN && CNDTR > 0 */
    uint8_t feed_pend; /* CC2 matched; the reload transfer is due */
    uint8_t done; /* terminal update fired */
    uint32_t feed_rem; /* feed transfers still pending */
    uint8_t* feed_ptr; /* resolved feed source cursor */
    uint8_t* cap_ptr; /* resolved capture destination cursor */
    uint32_t cap_total; /* total capture transfers scheduled */
    uint32_t cap_done; /* capture transfers performed so far */
    uint32_t cps; /* captures per slot (ceil) */
    uint8_t slots; /* number of slots (RCR + 1) */
    uint8_t period; /* current slot, 0-based */
    uint32_t tick; /* current counter position */
    uint16_t shadow; /* active output value (preload shadow or CCR3) */
} hw_tim_t;

static hw_tim_t g_tim;

void hw_tim_init(void) {
    g_tim = (hw_tim_t){0};
    g_tim.arr = (uint16_t)mock_tim1.ARR;
    g_tim.ccr2 = (uint16_t)mock_tim1.CCR2;
    g_tim.pe = (uint8_t)((MOCK_TIM_OUT_CCMR & MOCK_TIM_OUT_PE) ? 1u : 0u);
    g_tim.slots = (uint8_t)((mock_tim1.RCR & 0xFFu) + 1u);
    g_tim.tick = 0;
    g_tim.period = 0;
    g_tim.shadow = (uint16_t)mock_tim1.CCR3;
    if ((mock_tim1.DIER & MOCK_TIM_FEED_DE) && (mock_feed_ch.CCR & DMA_CCR_EN) &&
        mock_feed_ch.CNDTR > 0u) {
        g_tim.feed_en = 1u;
        g_tim.feed_rem = mock_feed_ch.CNDTR;
        g_tim.feed_ptr = (uint8_t*)hw_resolve((uint32_t)mock_feed_ch.CMAR);
        if (g_tim.feed_ptr == NULL) {
            fprintf(stderr, "hw_model: unresolved feed source address\n");
            g_tim.feed_en = 0u;
            mock_feed_ch.CNDTR = 0;
            mock_feed_ch.CCR &= ~DMA_CCR_EN;
        }
    }
    if ((mock_tim1.DIER & MOCK_TIM_CAP_DE) && (mock_dma1_ch4.CCR & DMA_CCR_EN) &&
        mock_dma1_ch4.CNDTR > 0u) {
        g_tim.cap_en = 1u;
        g_tim.cap_total = mock_dma1_ch4.CNDTR;
        g_tim.cps = (g_tim.cap_total + g_tim.slots - 1u) / g_tim.slots;
        g_tim.cap_ptr = (uint8_t*)hw_resolve((uint32_t)mock_dma1_ch4.CMAR);
        if (g_tim.cap_ptr == NULL) {
            fprintf(stderr, "hw_model: unresolved capture destination address\n");
            g_tim.cap_en = 0u;
            mock_dma1_ch4.CNDTR = 0;
            mock_dma1_ch4.CCR &= ~DMA_CCR_EN;
        }
    }
}

void hw_tim_init_shadow(uint16_t init_shadow) {
    hw_tim_init();
    g_tim.shadow = init_shadow;
}

/* One feed transfer: memory -> CCR3 (immediate when no OC3PE preload). */
static void hw_tim_do_feed(void) {
    uint16_t val = *g_tim.feed_ptr;
    g_tim.feed_ptr += 1; /* MSIZE 8-bit */
    mock_tim1.CCR3 = val;
    if (g_tim.pe == 0u) {
        g_tim.shadow = val; /* no preload: the output updates immediately */
    }
    g_tim.feed_rem--;
    mock_feed_ch.CNDTR = g_tim.feed_rem;
    if (g_tim.feed_rem == 0u) {
        g_tim.feed_en = 0u;
        mock_feed_ch.CCR &= ~DMA_CCR_EN;
    }
}

/* Counter position of the next capture of the current slot, or UINT32_MAX.
 * A capture fires at the pulse-edge time: the counter value at the moment
 * the bus edge arrives equals the captured duration. */
static uint32_t hw_tim_next_capture_tick(void) {
    if (!g_tim.cap_en || g_tim.cap_done >= g_tim.cap_total) {
        return UINT32_MAX;
    }
    uint32_t cap_end = (g_tim.period + 1u) * g_tim.cps;
    if (cap_end > g_tim.cap_total) {
        cap_end = g_tim.cap_total;
    }
    if (g_tim.cap_done >= cap_end) {
        return UINT32_MAX; /* all captures of this slot already taken */
    }
    uint32_t t = capture_source ? capture_source(g_tim.cap_done) : 0u;
    if (t <= g_tim.tick || t >= g_tim.arr) {
        return UINT32_MAX; /* outside the current counter window */
    }
    return t;
}

/* One capture transfer: CCR4 -> memory (MSIZE 8 or 16 per DMA config). */
static void hw_tim_do_capture(void) {
    uint16_t val = capture_source ? capture_source(g_tim.cap_done) : 0u;
    mock_tim1.CCR4 = val;
    if (mock_dma1_ch4.CCR & DMA_CCR_MSIZE_0) {
        *(volatile uint16_t*)g_tim.cap_ptr = val;
        g_tim.cap_ptr += 2;
    } else {
        *(volatile uint8_t*)g_tim.cap_ptr = (uint8_t)val;
        g_tim.cap_ptr += 1;
    }
    g_tim.cap_done++;
    mock_dma1_ch4.CNDTR = g_tim.cap_total - g_tim.cap_done;
    if (g_tim.cap_done >= g_tim.cap_total) {
        g_tim.cap_en = 0u;
        mock_dma1_ch4.CCR &= ~DMA_CCR_EN;
    }
}

hw_tim_event_t hw_tim_step(void) {
    if (!(mock_tim1.CR1 & TIM_CR1_CEN) || g_tim.done) {
        return HW_TIM_EV_IDLE;
    }
    /* CC2 matched on the previous step: the requested feed transfer is due. */
    if (g_tim.feed_pend) {
        g_tim.feed_pend = 0u;
        hw_tim_do_feed();
        return HW_TIM_EV_FEED;
    }
    /* Pick the next event by the smallest counter position. */
    uint32_t next = g_tim.arr;
    uint8_t kind = 3u; /* 3 = update/terminal */
    uint32_t cap_tick = hw_tim_next_capture_tick();
    if (g_tim.feed_en && g_tim.ccr2 > g_tim.tick && g_tim.ccr2 < g_tim.arr) {
        if (g_tim.ccr2 < next) {
            next = g_tim.ccr2;
            kind = 1u; /* 1 = CC2 */
        }
    }
    if (cap_tick != UINT32_MAX && cap_tick < next) {
        next = cap_tick;
        kind = 2u; /* 2 = capture */
    }
    if (next >= g_tim.arr) {
        /* ARR overflow: slot boundary (terminal on the last slot). */
        g_tim.tick = g_tim.arr;
        if (g_tim.pe) {
            g_tim.shadow = (uint16_t)mock_tim1.CCR3; /* preload -> shadow */
        }
        if (g_tim.period >= (uint32_t)g_tim.slots - 1u) {
            g_tim.done = 1u;
            mock_tim1.SR |= TIM_SR_UIF;
            if (mock_tim1.CR1 & TIM_CR1_OPM) {
                mock_tim1.CR1 &= (uint32_t)~TIM_CR1_CEN;
            }
            return HW_TIM_EV_TERMINAL;
        }
        g_tim.period++;
        g_tim.tick = 0;
        return HW_TIM_EV_UPDATE;
    }
    g_tim.tick = next;
    if (kind == 1u) {
        g_tim.feed_pend = 1u;
        return HW_TIM_EV_CC2;
    }
    hw_tim_do_capture();
    return HW_TIM_EV_CAPTURE;
}

uint32_t hw_tim_period(void) { return g_tim.period; }
uint32_t hw_tim_tick(void) { return g_tim.tick; }
uint16_t hw_tim_active(void) {
    return g_tim.pe ? g_tim.shadow : (uint16_t)mock_tim1.CCR3;
}
uint16_t hw_tim_ccr3(void) { return (uint16_t)mock_tim1.CCR3; }
uint16_t hw_tim_ccr4(void) { return (uint16_t)mock_tim1.CCR4; }
uint32_t hw_tim_slots(void) { return g_tim.slots; }
