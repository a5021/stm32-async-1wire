#include "hw_model.h"
#include "stm32f1xx.h"
#include <stdio.h>

TIM1_TypeDef mock_tim1;
DMA1_Channel_TypeDef mock_dma1_ch3;
DMA1_Channel_TypeDef mock_dma1_ch4;
GPIO_TypeDef mock_gpioa;
RCC_TypeDef mock_rcc;

static uint16_t tim_shadow_ccr1;
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
    mock_dma1_ch4 = (DMA1_Channel_TypeDef){0};
    mock_gpioa = (GPIO_TypeDef){0};
    mock_rcc = (RCC_TypeDef){0};
    tim_shadow_ccr1 = 0;
    capture_source = NULL;
    feed_log.count = 0;
    op_capture_count = 0;
    addr_count = 0;
}

void hw_set_capture_source(hw_capture_fn fn) { capture_source = fn; }

const hw_ccr1_feed_log_t* hw_ccr1_feed_log(void) { return &feed_log; }

uint32_t hw_capture_count(void) { return op_capture_count; }

uint16_t hw_effective_ccr1(void) {
    if (mock_tim1.CCMR1 & TIM_CCMR1_OC1PE) {
        return tim_shadow_ccr1;
    }
    return (uint16_t)mock_tim1.CCR1;
}

/* Resolved buffer pointers for the current operation (set by hw_run_until_uif). */
static uint8_t* d14_cur; /* channel-4 CCR1 feed source (memory read) */
static uint8_t* d13_cur; /* channel-3 capture destination (memory write) */

/* One D14 transfer: memory -> CCR1 (16-bit peripheral, 8-bit memory). */
static void dma14_transfer(void) {
    DMA1_Channel_TypeDef* d = &mock_dma1_ch4;
    if (!(d->CCR & DMA_CCR_EN) || d->CNDTR == 0) {
        return;
    }
    if (d14_cur == NULL) {
        fprintf(stderr, "hw_model: unresolved channel-4 source address\n");
        d->CNDTR = 0;
        d->CCR &= ~DMA_CCR_EN;
        return;
    }
    uint16_t val = *d14_cur;
    mock_tim1.CCR1 = val;
    d14_cur += 1; /* MSIZE 8-bit */
    d->CNDTR--;
    if (d->CNDTR == 0) {
        d->CCR &= ~DMA_CCR_EN;
    }
    if (feed_log.count < 64u) {
        feed_log.values[feed_log.count++] = val;
    }
}

/* One D13 transfer: CCR2 -> memory (MSIZE 8 or 16 per DMA config). */
static void dma13_transfer(void) {
    DMA1_Channel_TypeDef* d = &mock_dma1_ch3;
    if (!(d->CCR & DMA_CCR_EN) || d->CNDTR == 0) {
        return;
    }
    if (d13_cur == NULL) {
        fprintf(stderr, "hw_model: unresolved channel-3 destination address\n");
        d->CNDTR = 0;
        d->CCR &= ~DMA_CCR_EN;
        return;
    }
    uint16_t val = (uint16_t)mock_tim1.CCR2;
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
    d14_cur = (uint8_t*)hw_resolve((uint32_t)mock_dma1_ch4.CMAR);
    d13_cur = (uint8_t*)hw_resolve((uint32_t)mock_dma1_ch3.CMAR);
    uint32_t slots = (uint32_t)(t->RCR & 0xFFu) + 1u;
    if (slots > max_slots) {
        slots = max_slots;
    }
    /* captures per slot: ceil(CNDTR / slots) — e.g. reset = 2 in 1 slot. */
    uint32_t cps = 0;
    if ((t->DIER & TIM_DIER_CC2DE) && (mock_dma1_ch3.CCR & DMA_CCR_EN) &&
        mock_dma1_ch3.CNDTR > 0) {
        uint32_t n = mock_dma1_ch3.CNDTR;
        uint32_t s = (uint32_t)(t->RCR & 0xFFu) + 1u;
        cps = (n + s - 1u) / s;
    }
    for (uint32_t i = 0; i < slots; i++) {
        /* CC4 compare event -> channel-4 DMA feeds CCR1. Modeled at slot start
         * for simplicity; the ordering does not affect the tested invariants. */
        if (t->DIER & TIM_DIER_CC4DE) {
            dma14_transfer();
        }
        /* CC2 capture -> capture value + channel-3 DMA to memory. */
        if (t->DIER & TIM_DIER_CC2DE) {
            for (uint32_t c = 0; c < cps; c++) {
                uint16_t cap = capture_source ? capture_source(op_capture_count) : 0u;
                mock_tim1.CCR2 = cap;
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
            tim_shadow_ccr1 = (uint16_t)t->CCR1;
            return 1u;
        }
    }
    return 0u;
}
