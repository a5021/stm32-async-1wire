#include "hw_model.h"
#include "mock_target.h"
#include <stdio.h>

TIM1_TypeDef mock_tim1;
DMA1_Channel_TypeDef mock_dma1_ch3;
DMA1_Channel_TypeDef mock_feed_ch;
GPIO_TypeDef mock_gpioa;
RCC_TypeDef mock_rcc;

static uint16_t tim_shadow_out;
static hw_capture_fn capture_source;
static hw_ccr1_feed_log_t feed_log;
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
    mock_dma1_ch3 = (DMA1_Channel_TypeDef){0};
    mock_feed_ch = (DMA1_Channel_TypeDef){0};
    mock_gpioa = (GPIO_TypeDef){0};
    mock_rcc = (RCC_TypeDef){0};
    tim_shadow_out = 0;
    capture_source = NULL;
    feed_log.count = 0;
    op_capture_count = 0;
    addr_count = 0;
}

void hw_set_capture_source(hw_capture_fn fn) { capture_source = fn; }

const hw_ccr1_feed_log_t* hw_ccr1_feed_log(void) { return &feed_log; }

uint32_t hw_capture_count(void) { return op_capture_count; }

uint16_t hw_effective_ccr1(void) {
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
    DMA1_Channel_TypeDef* d = &mock_dma1_ch3;
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
    d13_cur = (uint8_t*)hw_resolve((uint32_t)mock_dma1_ch3.CMAR);
    uint32_t slots = (uint32_t)(t->RCR & 0xFFu) + 1u;
    if (slots > max_slots) {
        slots = max_slots;
    }
    /* captures per slot: ceil(CNDTR / slots) — e.g. reset = 2 in 1 slot. */
    uint32_t cps = 0;
    if ((t->DIER & MOCK_TIM_CAP_DE) && (mock_dma1_ch3.CCR & DMA_CCR_EN) && mock_dma1_ch3.CNDTR > 0) {
        uint32_t n = mock_dma1_ch3.CNDTR;
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
