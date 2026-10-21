#include "hw_model.h"
#include "mock_target.h"
#include "onewire.h"
#include "ow_config.h"
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>

TIM1_TypeDef mock_tim1;
DMA1_Channel_TypeDef mock_dma1_ch4;
DMA1_Channel_TypeDef mock_feed_ch;
GPIO_TypeDef mock_gpioa;
RCC_TypeDef mock_rcc;
USART_TypeDef mock_usart1;
#if OW_LOW_POWER
SCB_Type mock_scb; /* low-power WFE path: SEVONPEND lives in SCB.SCR */
#endif
#if defined(STM32G0) || defined(STM32G4)
DMAMUX_Channel_TypeDef mock_dmamux_ch2; /* G0/G4 backends: DMAMUX routing */
DMAMUX_Channel_TypeDef mock_dmamux_ch3; /* G0/G4 backends: DMAMUX routing */
#endif
#if defined(STM32G0)
SYSCFG_TypeDef mock_syscfg; /* G0 backend only */
#endif
#if defined(STM32F4)
DMA_TypeDef mock_dma2; /* F4 backend only */
FLASH_TypeDef mock_flash; /* F4 backend only: clock-latency register */
PWR_TypeDef mock_pwr; /* F4 backend only: over-drive, the F446 180MHz path */
#endif
#if defined(STM32G4)
FLASH_TypeDef mock_flash; /* G4 backend only: clock-latency register */
PWR_TypeDef mock_pwr; /* G4 backend only: Range 1 Boost for 170MHz */
#endif
#if defined(STM32H5)
FLASH_TypeDef mock_flash; /* H5 backend only: clock-latency register */
PWR_TypeDef mock_pwr; /* H5 backend only: VOS0 for the 250MHz clock */
#endif

static uint16_t tim_shadow_out;
static hw_capture_fn capture_source;
static hw_ccr3_feed_log_t feed_log;
static uint32_t op_capture_count;
static uint32_t capture_edges; /* 0 = unlimited (historical model behaviour) */

/* Low-power WFE hook state: see hw_model.h. The NULL default keeps __WFE() a
 * no-op, so builds without -DMOCK_WFE_HOOK are unchanged. */
static void (*s_wfe_hook)(void);

void mock_wfe_set_hook(void (*hook)(void)) {
    s_wfe_hook = hook;
}

void mock_wfe_hook(void) {
    if (s_wfe_hook != NULL) {
        s_wfe_hook();
    }
}

/* --- truncated 32-bit DMA address -> real host pointer table ---
 * The mock models what the silicon sees: CMAR/CPAR are 32-bit registers, so
 * it keeps the true host pointer only in this bookkeeping table and resolves
 * against the exact 32-bit value that ended up in the register.  On a 64-bit
 * host the (uint32_t)(uintptr_t) truncation could in principle alias two
 * distinct buffers to the same CMAR value; that collision is a model-invariant
 * violation and must fail loudly, so the "exact DMA address" claims of the
 * tests stay honest. */
#define HW_ADDR_TABLE_MAX 32
static const void* addr_table[HW_ADDR_TABLE_MAX];
static uint32_t addr_lo[HW_ADDR_TABLE_MAX];
static uint8_t addr_count;

static void* hw_resolve(uint32_t lo) {
    for (uint8_t i = 0; i < addr_count; i++) {
        if (addr_lo[i] == lo) {
            return (void*)addr_table[i];
        }
    }
    return NULL;
}

/* A model invariant was violated: the model cannot behave like the silicon
 * from here, so every conclusion drawn from it would be unsound. That is not
 * a test failure to be reported and stepped over - it is the model saying it
 * does not know what the hardware would do. Abort, loudly, with the address
 * that could not be resolved. */
static void hw_fatal(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    fputs("hw_model: FATAL: ", stderr);
    vfprintf(stderr, fmt, ap);
    fputc('\n', stderr);
    va_end(ap);
    abort();
}

void hw_register_buf(const void* ptr) {
    uint32_t lo = (uint32_t)(uintptr_t)ptr;
    if (hw_resolve(lo) == ptr) {
        return; /* already registered */
    }
    if (hw_resolve(lo) != NULL) {
        hw_fatal("distinct host buffers truncate to the same 32-bit CMAR "
                 "value 0x%08lX",
                 (unsigned long)lo);
    }
    if (addr_count >= HW_ADDR_TABLE_MAX) {
        hw_fatal("registered-buffer table exhausted (%u entries)",
                 (unsigned)HW_ADDR_TABLE_MAX);
    }
    addr_lo[addr_count] = lo;
    addr_table[addr_count] = ptr;
    addr_count++;
}

/* The memory address the feed DMA reads from, and the one the capture DMA
 * writes to, as the silicon sees it: the direction-relative registers on
 * GPDMA (H5), the classic CMAR everywhere else. */
static uint32_t hw_feed_addr(void) {
#if defined(STM32H5)
    return (uint32_t)MOCK_DMA_FEED.CSAR;
#else
    return (uint32_t)MOCK_DMA_FEED.CMAR;
#endif
}

static uint32_t hw_cap_addr(void) {
#if defined(STM32H5)
    return (uint32_t)MOCK_DMA_CAP.CDAR;
#else
    return (uint32_t)MOCK_DMA_CAP.CMAR;
#endif
}

const void* hw_feed_source(void) {
    return hw_resolve(hw_feed_addr());
}

const void* hw_capture_destination(void) {
    return hw_resolve(hw_cap_addr());
}

/* The peripheral request the channel carries, as the silicon sees it: the
 * DMAMUX channel's request id on G0/G4, the GPDMA transfer register's REQSEL
 * on H5, and nothing at all on the families whose request map is fixed in
 * silicon (there the port programs nothing, and there is nothing to read
 * back). */
uint32_t hw_feed_request(void) {
#if defined(STM32H5)
    return (mock_feed_ch.CTR2 >> DMA_CTR2_REQSEL_Pos) & 0xFFu;
#elif defined(STM32G0) || defined(STM32G4)
    return (uint32_t)mock_dmamux_ch2.CCR;
#else
    return 0u;
#endif
}

uint32_t hw_capture_request(void) {
#if defined(STM32H5)
    return (mock_dma1_ch4.CTR2 >> DMA_CTR2_REQSEL_Pos) & 0xFFu;
#elif defined(STM32G0) || defined(STM32G4)
    return (uint32_t)mock_dmamux_ch3.CCR;
#else
    return 0u;
#endif
}

void hw_reset_all(void) {
    mock_tim1 = (TIM1_TypeDef){0};
    mock_dma1_ch4 = (DMA1_Channel_TypeDef){0};
    mock_feed_ch = (DMA1_Channel_TypeDef){0};
    mock_gpioa = (GPIO_TypeDef){0};
    mock_rcc = (RCC_TypeDef){0};
    mock_usart1 = (USART_TypeDef){0};
#if OW_LOW_POWER
    mock_scb = (SCB_Type){0};
#endif
    /* USART TXE is set by hardware when the transmit buffer is empty —
     * that is the reset/power-on state.  Pre-set it so ow_tx_char() does
     * not spin-wait in host tests. */
#if defined(STM32G0) || defined(STM32F0) || defined(STM32F3) || defined(STM32G4) || defined(STM32H5)
    mock_usart1.ISR = 0x00000080u; /* USART_ISR_TXE / USART_ISR_TXE_TXFNF */
#else
    mock_usart1.SR = 0x00000080u; /* USART_SR_TXE */
#endif
#if defined(STM32G0) || defined(STM32G4)
    mock_dmamux_ch2 = (DMAMUX_Channel_TypeDef){0};
    mock_dmamux_ch3 = (DMAMUX_Channel_TypeDef){0};
#endif
#if defined(STM32G4)
    mock_flash = (FLASH_TypeDef){0};
    /* VOSF starts clear, like hardware after the regulator settles, so the
     * Range-1-Boost wait in configure_system_clock() passes immediately. */
    mock_pwr = (PWR_TypeDef){0};
#endif
#if defined(STM32H5)
    mock_flash = (FLASH_TypeDef){0};
    /* VOS0/HSERDY/PLL1RDY all start clear: the mock does not model the
     * regulator ramp, the oscillator start or the PLL lock, so the clock tests
     * preset exactly the flags the path they exercise waits on. A build that
     * forgets one of those waits then blocks on the first poll instead of
     * passing by accident. */
    mock_pwr = (PWR_TypeDef){0};
#endif
#if defined(STM32F4)
    mock_dma2 = (DMA_TypeDef){0};
    mock_flash = (FLASH_TypeDef){0};
    /* Over-drive starts disabled, and - unlike the real part - the ready flags
     * start clear too, so a clock test that forgets to enable OD blocks on the
     * first wait instead of passing by accident. */
    mock_pwr = (PWR_TypeDef){0};
#endif
    tim_shadow_out = 0;
    capture_source = NULL;
    capture_edges = 0;
    feed_log.count = 0;
    feed_log.total = 0;
    op_capture_count = 0;
    addr_count = 0;
}

void hw_set_capture_source(hw_capture_fn fn) { capture_source = fn; }

void hw_set_capture_edges(uint32_t n) { capture_edges = n; }

const hw_ccr3_feed_log_t* hw_ccr3_feed_log(void) { return &feed_log; }

uint32_t hw_capture_count(void) { return op_capture_count; }

uint16_t hw_effective_ccr3(void) {
    if (MOCK_TIM_OUT_CCMR & MOCK_TIM_OUT_PE) {
        return tim_shadow_out;
    }
    return (uint16_t)MOCK_TIM_OUT_CCR;
}

/* Resolved buffer pointers for the current operation (set by hw_run_until_uif). */
static ow_pulse_t* d16_cur; /* feed DMA source (memory read) */
static uint8_t* d13_cur; /* capture DMA destination (memory write) */

/* One feed transfer: memory -> output CCR (family-width memory cell). */
static void dma16_transfer(void) {
    DMA1_Channel_TypeDef* d = &MOCK_DMA_FEED;
    if (!(d->CCR & MOCK_DMA_FEED_EN) || d->CNDTR == 0) {
        return;
    }
    if (d16_cur == NULL) {
        /* The driver programmed a memory address the model does not know.
         * Continuing would mean inventing a source, and every conclusion
         * drawn from a fabricated transfer is unsound - the feed would
         * simply stop and the test would read that as "no feed happened".
         * Refuse instead. */
        hw_fatal("unresolved feed source address 0x%08lX - register the "
                 "buffer the driver feeds from with hw_register_buf()",
                 (unsigned long)hw_feed_addr());
    }
    uint16_t val = *d16_cur;
    MOCK_TIM_OUT_CCR = val;
    d16_cur += 1; /* advance one ow_pulse_t cell */
    d->CNDTR--;
    if (d->CNDTR == 0) {
        d->CCR &= ~MOCK_DMA_FEED_EN;
    }
    feed_log.total++;
    if (feed_log.count < 128u) {
        feed_log.values[feed_log.count++] = val;
    }
}

/* One capture transfer: capture CCR -> memory (MSIZE 8 or 16 per DMA config). */
static void dma13_transfer(void) {
    DMA1_Channel_TypeDef* d = &MOCK_DMA_CAP;
    if (!(d->CCR & MOCK_DMA_CAP_EN) || d->CNDTR == 0) {
        return;
    }
    if (d13_cur == NULL) {
        hw_fatal("unresolved capture destination address 0x%08lX - register "
                 "the buffer the driver captures into with hw_register_buf()",
                 (unsigned long)hw_cap_addr());
    }
    uint16_t val = (uint16_t)MOCK_TIM_CAP_CCR;
#if defined(STM32H5)
    /* GPDMA carries the memory width in CTR1.DDW_LOG2 (halfword = 16-bit
     * cells); the classic backends carry it in CCR.MSIZE_0. */
    if (d->CTR1 & (1u << DMA_CTR1_DDW_LOG2_Pos)) {
#else
    if (d->CCR & MOCK_DMA_CAP_MSIZE_0) {
#endif
        *(volatile uint16_t*)d13_cur = val;
        d13_cur += 2;
    } else {
        *(volatile uint8_t*)d13_cur = (uint8_t)val;
        d13_cur += 1;
    }
    d->CNDTR -= MOCK_CAP_BYTES_PER_XFR; /* classic: 1/transfer; H5: BNDT bytes (2 for halfword source) */
    if (d->CNDTR == 0) {
        d->CCR &= ~MOCK_DMA_CAP_EN;
    }
}

uint8_t hw_run_until_uif(uint32_t max_slots) {
    TIM1_TypeDef* t = &mock_tim1;
    if (!(t->CR1 & TIM_CR1_CEN)) {
        return (t->SR & TIM_SR_UIF) ? 1u : 0u;
    }
    feed_log.count = 0;
    feed_log.total = 0;
    op_capture_count = 0;
    /* Resolve the DMA buffer addresses exactly as the driver stored them. */
    d16_cur = (ow_pulse_t*)hw_resolve(hw_feed_addr());
    d13_cur = (uint8_t*)hw_resolve(hw_cap_addr());
    if (d16_cur == NULL && (MOCK_DMA_FEED.CCR & MOCK_DMA_FEED_EN) && MOCK_DMA_FEED.CNDTR > 0u) {
        hw_fatal("unresolved feed source address 0x%08lX - register the "
                 "buffer the driver feeds from with hw_register_buf()",
                 (unsigned long)hw_feed_addr());
    }
    if (d13_cur == NULL && (MOCK_DMA_CAP.CCR & MOCK_DMA_CAP_EN) && MOCK_DMA_CAP.CNDTR > 0u) {
        hw_fatal("unresolved capture destination address 0x%08lX - register "
                 "the buffer the driver captures into with hw_register_buf()",
                 (unsigned long)hw_cap_addr());
    }
    uint32_t slots = (uint32_t)(t->RCR & 0xFFu) + 1u;
    if (slots > max_slots) {
        slots = max_slots;
    }
    /* captures per slot: ceil(CNDTR / slots) - e.g. reset = 2 in 1 slot. */
    uint32_t cps = 0;
    if ((t->DIER & MOCK_TIM_CAP_DE) && (MOCK_DMA_CAP.CCR & MOCK_DMA_CAP_EN) &&
        MOCK_DMA_CAP.CNDTR > 0) {
        uint32_t n = MOCK_CAP_TRANSFERS(MOCK_DMA_CAP.CNDTR);
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
        /* Capture event -> capture value + capture DMA to memory. When a
         * capture-edge limit is set, stop before the limit is exceeded *without*
         * performing the extra transfers: the DMA counter then never drains and
         * EN stays latched, the real-silicon underrun state (see the note on
         * hw_set_capture_edges). */
        if (t->DIER & MOCK_TIM_CAP_DE) {
            for (uint32_t c = 0; c < cps; c++) {
                if (capture_edges != 0u && op_capture_count >= capture_edges) {
                    break;
                }
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
    ow_pulse_t* feed_ptr; /* resolved feed source cursor */
    uint8_t* cap_ptr; /* resolved capture destination cursor */
    uint32_t cap_total; /* total capture transfers scheduled */
    uint32_t cap_done; /* capture transfers performed so far */
    uint32_t cps; /* captures per slot (ceil) */
    uint32_t slots; /* number of slots (RCR + 1); 256 must fit (RCR = 255) */
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
    g_tim.slots = (uint32_t)((mock_tim1.RCR & 0xFFu) + 1u);
    g_tim.tick = 0;
    g_tim.period = 0;
    g_tim.shadow = (uint16_t)mock_tim1.CCR3;
    if ((mock_tim1.DIER & MOCK_TIM_FEED_DE) && (MOCK_DMA_FEED.CCR & MOCK_DMA_FEED_EN) &&
        MOCK_DMA_FEED.CNDTR > 0u) {
        g_tim.feed_en = 1u;
        g_tim.feed_rem = MOCK_DMA_FEED.CNDTR;
        g_tim.feed_ptr = (ow_pulse_t*)hw_resolve(hw_feed_addr());
        if (g_tim.feed_ptr == NULL) {
            hw_fatal("unresolved feed source address 0x%08lX - register the "
                     "buffer the driver feeds from with hw_register_buf()",
                     (unsigned long)hw_feed_addr());
        }
    }
    if ((mock_tim1.DIER & MOCK_TIM_CAP_DE) && (MOCK_DMA_CAP.CCR & MOCK_DMA_CAP_EN) &&
        MOCK_DMA_CAP.CNDTR > 0u) {
        g_tim.cap_en = 1u;
        g_tim.cap_total = MOCK_CAP_TRANSFERS(MOCK_DMA_CAP.CNDTR);
        g_tim.cps = (g_tim.cap_total + g_tim.slots - 1u) / g_tim.slots;
        g_tim.cap_ptr = (uint8_t*)hw_resolve(hw_cap_addr());
        if (g_tim.cap_ptr == NULL) {
            hw_fatal("unresolved capture destination address 0x%08lX - "
                     "register the buffer the driver captures into with "
                     "hw_register_buf()",
                     (unsigned long)hw_cap_addr());
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
    g_tim.feed_ptr += 1; /* advance one ow_pulse_t cell */
    mock_tim1.CCR3 = val;
    if (g_tim.pe == 0u) {
        g_tim.shadow = val; /* no preload: the output updates immediately */
    }
    g_tim.feed_rem--;
    MOCK_DMA_FEED.CNDTR = g_tim.feed_rem;
    if (g_tim.feed_rem == 0u) {
        g_tim.feed_en = 0u;
        MOCK_DMA_FEED.CCR &= ~MOCK_DMA_FEED_EN;
    }
}

/* Counter position of the next capture of the current slot, or UINT32_MAX.
 * A capture fires at the pulse-edge time: the counter value at the moment
 * the bus edge arrives equals the captured duration. */
static uint32_t hw_tim_next_capture_tick(void) {
    if (!g_tim.cap_en || g_tim.cap_done >= g_tim.cap_total) {
        return UINT32_MAX;
    }
    if (capture_edges != 0u && g_tim.cap_done >= capture_edges) {
        return UINT32_MAX; /* edge limit: no further captures, channel stays armed */
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
#if defined(STM32H5)
    /* GPDMA width in CTR1.DDW_LOG2 (halfword = 16-bit cells). */
    if (mock_dma1_ch4.CTR1 & (1u << DMA_CTR1_DDW_LOG2_Pos)) {
#else
    if (MOCK_DMA_CAP.CCR & MOCK_DMA_CAP_MSIZE_0) {
#endif
        *(volatile uint16_t*)g_tim.cap_ptr = val;
        g_tim.cap_ptr += 2;
    } else {
        *(volatile uint8_t*)g_tim.cap_ptr = (uint8_t)val;
        g_tim.cap_ptr += 1;
    }
    g_tim.cap_done++;
    MOCK_DMA_CAP.CNDTR = MOCK_CAP_CNDTR(g_tim.cap_total - g_tim.cap_done);
    if (g_tim.cap_done >= g_tim.cap_total) {
        g_tim.cap_en = 0u;
        MOCK_DMA_CAP.CCR &= ~MOCK_DMA_CAP_EN;
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
