/**
 * @file ow_port_f4.h
 * @brief STM32F4 backend: TIM1 (advanced) + DMA2 dual streams + PA10
 *
 * Header-only static inline implementation of the ow_port_* interface for the
 * STM32F407 (STM32F4DISCOVERY). The 1-Wire bus runs on PA10 (TIM1_CH3 PWM output in open-drain
 * alternate function AF1); CH4 captures in indirect mode on the same pin
 * (CC4S routes IC4 to TI3) and drains CCR4 via DMA2_Stream4. Multi-slot writes
 * and the merged search write+read drive the CC2 slot-end marker
 * (DMA2_Stream2) to reload CCR3 from the caller's ow_pulse_t buffer in one
 * timer pass. TIM1 runs in one-pulse mode (OPM) with the repetition counter
 * (RCR) batching N slots into a single update event (UIF). Reset and single
 * slots need no DMA (direct OC3 write).
 *
 * HARDWARE CONTRACT (see the "Required Timer Capabilities" section of README):
 *  - DMA2 streams run in direct mode (no FIFO): the memory transfer width is
 *    forced to PSIZE. CCR3 is a halfword register, so the feed source must be
 *    halfword (ow_pulse_t = uint16_t on this family); the capture moves
 *    matching 16-bit halfwords (or reads CCR4 low - byte serves as an 8-bit
 *    capture path).
 *  - Each stream selects its own request with CHSEL=6 (Stream2 -> TIM1_CH2,
 *    Stream4 -> TIM1_CH4); the two request paths are independent.
 *  - GPIO follows the F0/G0 MODER/OTYPER/AFR style (no F1 CRH rule): PA10 is
 *    alternate-function open-drain with AFR=1 (TIM1_CH3); the parasite
 *    strong-pull-up toggles only the OTYPER bit.
 *
 * Bring-up notes and the alternatives that were tried and rejected:
 * see HARDWARE-NOTES.md in this directory.
 */

#ifndef OW_PORT_F4_H
#define OW_PORT_F4_H

#include "onewire_internal.h"
#include "ow_bits.h"
#include "stm32f4xx.h"

#include <assert.h>

/* ------------------------------------------------------------------
 *  1-Wire bus pin selection (TIM1_CH3 output + IC4 capture via TI3).
 *
 *  Default: PA10 (AF1). With -DOW_PORT_BUS_PE13=1 the bus moves to PE13
 *  (also TIM1_CH3, AF1) for boards where PA10 is loaded/unavailable. The
 *  PA11 logic-analyzer marker stays on GPIOA in both cases.
 * ------------------------------------------------------------------ */
#if defined(OW_PORT_BUS_PE13)
#define OW_BUS_GPIO (*GPIOE)
#define OW_BUS_GPIO_CLK RCC_AHB1ENR_GPIOEEN
#define OW_BUS_MODER GPIO_MODER_MODER13
#define OW_BUS_MODER_1 GPIO_MODER_MODER13_1
#define OW_BUS_OT GPIO_OTYPER_OT_13
#define OW_BUS_OSPEEDR GPIO_OSPEEDR_OSPEED13
#define OW_BUS_OSPEEDR_Pos GPIO_OSPEEDR_OSPEED13_Pos
#define OW_BUS_AFSEL GPIO_AFRH_AFSEL13
#define OW_BUS_AFSEL_Pos GPIO_AFRH_AFSEL13_Pos
#else
#define OW_BUS_GPIO (*GPIOA)
#define OW_BUS_GPIO_CLK RCC_AHB1ENR_GPIOAEN
#define OW_BUS_MODER GPIO_MODER_MODER10
#define OW_BUS_MODER_1 GPIO_MODER_MODER10_1
#define OW_BUS_OT GPIO_OTYPER_OT_10
#define OW_BUS_OSPEEDR GPIO_OSPEEDR_OSPEED10
#define OW_BUS_OSPEEDR_Pos GPIO_OSPEEDR_OSPEED10_Pos
#define OW_BUS_AFSEL GPIO_AFRH_AFSEL10
#define OW_BUS_AFSEL_Pos GPIO_AFRH_AFSEL10_Pos
#endif

/* @brief Timer prescaler for 1µs resolution (PSC = SYSCLK / 1MHz - 1),
 *       derived from the shared OW_PORT_SYSCLK_MHZ knob in onewire.h.
 *
 *  INVARIANT: the TIM1 kernel clock must equal SYSCLK — only then does
 *  PSC = SYSCLK_MHZ - 1 produce a 1µs tick.  STM32 rule: when the APB
 *  prescaler feeding TIM1 is != 1, the timer clock doubles to 2 × PCLK;
 *  configure_system_clock() relies on that doubling at 168MHz.
 *
 *  F4: TIM1 is on APB2 (RM0090 §7):
 *   - 168MHz (8MHz HSE+PLL, default): PPRE1=/4 (APB1=42MHz), PPRE2=/2
 *     (APB2=84MHz) — both at their datasheet limits — and the APB2 timer
 *     clock doubles to 2 × 84 = 168MHz = SYSCLK.  ✓
 *   - 16MHz (raw HSI) / 8MHz (raw HSE): both APB prescalers stay /1, so
 *     TIM1 = PCLK2 = SYSCLK directly.  ✓
 *  (PPRE1=/4 only affects APB1 peripherals — TIM2..5, USART2/3/6, I2C —
 *  none of which this port uses.)
 *
 *  Test: tests/test_timing.c::test_apb_prescaler_div1_for_tim1() */
#define OW_PORT_TIM_PRESCALER ((OW_PORT_SYSCLK_MHZ) - 1u)
_Static_assert(OW_PORT_TIM_PRESCALER <= 0xFFFFu,
               "TIM prescaler exceeds 16-bit PSC register width");

/* @brief DMA stream assignment (RM0368 §9.3.3, Table 29): each stream selects
 *       its request source with its own CHSEL field.  DMA2_Stream2 CHSEL=6
 *       carries TIM1_CH2 (slot-end CC2 event); DMA2_Stream4 CHSEL=6 carries
 *       TIM1_CH4 (input-capture CC4 event).  CHSEL=6 is the per-stream mux
 *       index, not a shared physical request line. */
#define OW_PORT_DMA_FEED (*DMA2_Stream2)
#define OW_PORT_DMA_CAPTURE (*DMA2_Stream4)
#define OW_PORT_DMA_CHSEL (6u << DMA_SxCR_CHSEL_Pos)

/* @brief Capture-stream control bits: MINC = memory-increment, PSIZE_0 =
 *       16-bit peripheral read (CCR4), PL_1 = high priority.  Direct mode
 *       forces the memory width to PSIZE, so the caller also sets MSIZE_0
 *       (matching halfwords).  Note the F4 DIR field is two bits wide: the
 *       generic single-bit DMA_SxCR_DIR macro would write the reserved 0b11. */
#define OW_PORT_DMA_CR_CAPTURE (DMA_SxCR_MINC | DMA_SxCR_PSIZE_0 | \
                                DMA_SxCR_PL_1)

/* @brief Feed-stream control bits: DIR_0 = memory-to-peripheral, MINC =
 *       memory-increment, PSIZE_0 = 16-bit peripheral write (CCR3 is a
 *       halfword register), PL_1 = high priority.  The caller also sets
 *       MSIZE_0: direct mode forces the memory width to PSIZE, so the source
 *       must be a matching halfword (ow_pulse_t) buffer. */
#define OW_PORT_DMA_CR_FEED (DMA_SxCR_DIR_0 | DMA_SxCR_MINC | \
                             DMA_SxCR_PL_1 | DMA_SxCR_PSIZE_0)

/* @brief Retire a DMA stream's interrupt flags so the next arm starts clean
 * @note Non-blocking. A normal-mode stream disables itself (hardware clears
 *       EN) when its transfer completes, and every operation here is gated by
 *       ow_port_bus_done(), so the stream is already idle when it is re-armed:
 *       this only has to clear the TC/HT/TE/DME/FE flags. STM32F4 clears EN
 *       only at the end of a transfer, so an EN=0 wait would block for the
 *       remainder of an in-flight transfer - the CR write below just requests
 *       the disable (a no-op in steady state) and never waits. Capture =
 *       DMA2_Stream4 (high group, HIFCR).
 */
__STATIC_FORCEINLINE void ow_port_dma_rearm(DMA_Stream_TypeDef* stream,
                                            volatile uint32_t* isr,
                                            uint32_t mask) {
    stream->CR = 0; /* request disable: no-op once the previous transfer finished */
    *isr = mask; /* clear TC/HT/TE/DME/FE so the re-enable is clean */
}

/**
 * @brief Force a timer update event, leaving UIF set
 * @note Kickstart / timer hand-over: EGR=UG with no SR clear, so the owner
 *       (measurement state machine) sees UIF set and advances immediately.
 */
__STATIC_FORCEINLINE void ow_port_kick(void) {
    T1.EGR = TIM_EGR(UG);
    __DSB();
}

/**
 * @brief Force a timer update event and clear the update flag
 * @note Re-arm: reloads ARR/RCR/CCR preloads and clears UIF so the freshly
 *       scheduled operation has a clean completion flag. Same intent as the
 *       F1/F0/G0 ports (EGR=UG; clear the flag) but the F4 flag clear is gated
 *       with URS instead of spun on - see the body comment. No waiting.
 */
__STATIC_FORCEINLINE void ow_port_update_event(void) {
    /* Force a timer update (reload ARR/RCR/CCR preloads and clear CNT) without
     * leaving a stale UIF behind. On the F4 the UG-raised UIF appears a few
     * timer cycles after the EGR write, so clearing SR straight after EGR=UG
     * races it and can drop the clear, leaving a stale update flag that makes
     * the next ow_port_bus_done() report the freshly scheduled operation
     * complete before it starts. Gate the forced update with URS so it raises
     * no UIF at all, clear any leftover flag, then restore URS; the real
     * completion (counter overflow) still sets UIF. Non-blocking, no spin. */
    T1.CR1 |= TIM_CR1(URS);
    T1.EGR = TIM_EGR(UG);
    __DSB();
    T1.SR = 0; /* UIF (and any stale CCxIF) cleared: fresh op gets a clean completion flag */
    T1.CR1 &= ~TIM_CR1(URS);
}

/**
 * @brief Enable clocks, configure the timer prescaler and PA10 open-drain AF
 */
__STATIC_FORCEINLINE void ow_port_init(void) {
    RC.AHB1ENR |= RCC_BITS(AHB1ENR, DMA2EN, GPIOAEN) | OW_BUS_GPIO_CLK; /* TIM1 requests route via DMA2 */
    RC.APB2ENR |= RCC_APB2ENR(TIM1EN);
    T1.PSC = OW_PORT_TIM_PRESCALER;
    ow_port_kick(); /* kickstart: first poll advances immediately */
    T1.BDTR = TIM_BDTR(MOE);
    /* Bus pin: alternate function, open-drain, AF1 (TIM1_CH3) */
    OW_BUS_GPIO.MODER = (OW_BUS_GPIO.MODER & ~OW_BUS_MODER) | OW_BUS_MODER_1;
    OW_BUS_GPIO.OTYPER |= OW_BUS_OT;
    OW_BUS_GPIO.AFR[1] = (OW_BUS_GPIO.AFR[1] & ~OW_BUS_AFSEL) | (1u << OW_BUS_AFSEL_Pos);
    /* Bus-pin drive strength.  The strong pull-up is this pin in AF
     * push-pull (TIM1_CH3 driven HIGH while the timer is stopped), so its
     * drive strength is the parasite supply for the whole fleet: at the
     * reset-default low speed a simultaneous (broadcast) conversion of
     * several devices droops the line into brown-out (POR 85 C / garbage
     * with valid CRC), while one device at a time still converts fine.
     * Configurable via OW_BUS_DRIVE (default MAX = very-high). */
    OW_BUS_GPIO.OSPEEDR = (OW_BUS_GPIO.OSPEEDR & ~OW_BUS_OSPEEDR) |
                          ((OW_BUS_DRIVE & 0x3u) << OW_BUS_OSPEEDR_Pos);
    /* PA11: debug/logic-analyzer marker, GPIO output push-pull, low by default.
     * PA11 is not needed for TIM1_CH4 (IC4 is routed internally to TI3), so the
     * pad is free as a plain output. */
    PA.MODER = (PA.MODER & ~GPIO_MODER_MODER11) | GPIO_MODER_MODER11_0;
    PA.OTYPER &= ~GPIO_OTYPER_OT_11;
    PA.ODR &= ~GPIO_ODR_OD11;
}

/**
 * @brief Toggle the PA11 logic-analyzer marker line
 * @note One transition per merged op pass (and per re-arm call it happens in).
 */
__STATIC_FORCEINLINE void ow_port_marker_toggle(void) {
    PA.ODR ^= GPIO_ODR_OD11;
}

#if OW_PORT_LOW_POWER
/** @brief Set while a hardware stage longer than 1 ms is running. */
extern uint8_t ow_long_pending; /* defined in onewire.c, shared across TUs */
#endif

/**
 * @brief Non-blocking completion check for the scheduled operation
 * @return 1 if finished (update flag set and cleared), 0 while still running
 */
__STATIC_FORCEINLINE uint8_t ow_port_bus_done(void) {
    if (T1.SR & TIM_SR(UIF)) {
        /* No software bus release needed: every operation returns the line to
         * idle HIGH in hardware. DMA-fed writes (ow_port_feed,
         * ow_port_write_then_read) append a trailing 0 to the CCR3 feed, and
         * the direct-write/capture operations (reset, read, single slot) use
         * an OC3PE preload of 0 — both applied exactly when the one-pulse
         * timer stops. */
#if OW_PORT_LOW_POWER
        /* The update event both interrupts the low-power WFE sleep and, via
         * SEVONPEND, raises an NVIC pending bit. UIE also latches a pending
         * bit at every ow_port_update_event() re-arm (EGR=UG). Clear the
         * pending flag here so the next __WFE() truly sleeps; otherwise the
         * pending bit would make __WFE() return immediately forever (silent
         * degradation back to a busy-loop). */
        NVIC_ClearPendingIRQ(OW_PORT_TIM1_UPD_IRQn);
        ow_long_pending = 0;
#endif
        T1.SR = 0;
        return 1u;
    }
    return 0u;
}

#if OW_PORT_LOW_POWER
/**
 * @brief Whether the currently scheduled operation is a "long" stage (> 1 ms)
 * @return 1 while a long stage (conversion, scratchpad read, EEPROM hold-off,
 *         inter-cycle pause) is in flight, 0 otherwise
 * @note A low-power application checks this, then calls
 *       ow_port_sleep_until_done() when it is set, instead of busy-polling.
 */
__STATIC_FORCEINLINE uint8_t ow_port_long_wait_pending(void) {
    return ow_long_pending;
}

/**
 * @brief Block in WFE until the scheduled long stage completes
 * @note Only the update event wakes the core (SEVONPEND, no ISR). Valid only
 *       while a long stage (> 1 ms) is running; the pending bit is cleared in
 *       ow_port_bus_done().
 */
__STATIC_FORCEINLINE void ow_port_sleep_until_done(void) {
    /* Prepare for sleep: clear any stale NVIC pending bit so a leftover
     * event cannot wake the very first WFE (silent busy-loop degradation).
     * A fresh pending bit will be latched by the timer's update when the
     * long stage completes. */
    NVIC_ClearPendingIRQ(OW_PORT_TIM1_UPD_IRQn);
    /* Re-arm the event: SEV sets the event register, the first WFE returns
     * immediately and clears it, so the second WFE in the loop truly sleeps
     * until a new event arrives. */
    __SEV();
    __WFE();
    while (!(T1.SR & TIM_SR(UIF))) {
        __WFE(); /* sleeps; woken by the pending bit via SEVONPEND (no ISR) */
    }
    /* Consume the wake-up event so the next sleep starts from a clean state. */
    NVIC_ClearPendingIRQ(OW_PORT_TIM1_UPD_IRQn);
}
#endif

/**
 * @brief Set the bus pin drive mode (open-drain vs push-pull)
 * @param[in] push_pull 1 selects alternate-function push-pull (master actively
 *        drives both bus levels), 0 selects alternate-function open-drain
 *        (master drives LOW only, releasing HIGH to the external pull-up).
 * @note Used by the parasite strong-pull-up and, when OW_DRIVE_ACTIVE is
 *       set, by the active-drive write path. The pin never leaves
 *       alternate-function (TIM1_CH3); only the output-stage topology changes.
 */
__STATIC_FORCEINLINE void ow_port_set_pin_mode(uint8_t push_pull) {
    if (push_pull) {
        OW_BUS_GPIO.OTYPER &= ~OW_BUS_OT; /* OD -> PP (strong HIGH) */
    } else {
        OW_BUS_GPIO.OTYPER |= OW_BUS_OT; /* PP -> OD (release) */
    }
}

/**
 * @brief Configure timer and DMA for a capture operation
 * @param[out] dst Destination buffer for captured data
 * @param[in] count Number of transfers
 * @param[in] width DMA transfer width: 8 for 8-bit, 16 for 16-bit
 */
__STATIC_FORCEINLINE void ow_port_capture(volatile void* dst, uint16_t count, uint16_t width) {
#if OW_DRIVE_ACTIVE
    ow_port_set_pin_mode(0); /* read/reset phases must be open-drain (slave can pull LOW) */
#endif
    T1.CCMR2 = TIM_CCMR2(OC3M_0, OC3M_1, OC3M_2, OC3PE, CC4S_1, OW_PORT_IC4F_ARGS);
    T1.CCER = TIM_CCER(CC3E, CC4E);
#if OW_PORT_LOW_POWER
    T1.DIER = TIM_DIER(CC4DE, UIE);
    if ((uint32_t)count * (ONEWIRE_ONE_PULSE + ONEWIRE_ZERO_PULSE + ONEWIRE_GUARD_BAND) > 1000u) {
        ow_long_pending = 1; /* e.g. a 72-slot scratchpad read (~5 ms) */
    }
#else
    T1.DIER = TIM_DIER(CC4DE);
#endif
    ow_port_update_event();
    T1.CCR3 = 0;
    ow_port_dma_rearm(DMA2_Stream4, &DMA2->HIFCR,
                      DMA_HIFCR_CTCIF4 | DMA_HIFCR_CHTIF4 | DMA_HIFCR_CTEIF4 |
                          DMA_HIFCR_CDMEIF4 | DMA_HIFCR_CFEIF4);
    OW_PORT_DMA_CAPTURE.PAR = (uint32_t)&T1.CCR4;
    OW_PORT_DMA_CAPTURE.M0AR = (uint32_t)dst;
    OW_PORT_DMA_CAPTURE.NDTR = count;
    OW_PORT_DMA_CAPTURE.CR = (OW_PORT_DMA_CR_CAPTURE & ~DMA_SxCR_PSIZE_0) |
                             OW_PORT_DMA_CHSEL |
                             ((width == 16) ? (DMA_SxCR_MSIZE_0 | DMA_SxCR_PSIZE_0) : 0) |
                             DMA_SxCR_EN;
    T1.CR1 = TIM_CR1(OPM, CEN);
}

/**
 * @brief Transmit a command sequence of arbitrary length using DMA
 * @param[in] cmd Pointer to command sequence in pulse duration format
 * @param[in] slots Number of bit slots (bits) to transmit, 1..ONEWIRE_MAX_SLOTS (256).
 * @return 1 if the write was scheduled, 0 if `slots` is out of range. In debug
 *         builds the reject path also traps with an assert.
 * @note The buffer must hold `slots + 1` entries and the entry at index
 *       `slots` must be 0: the final CC2-triggered DMA transfer feeds that
 *       trailing 0 into CCR3 during the last slot, so the one-pulse timer
 *       stops with the line already released to idle HIGH (hardware bus
 *       release — no software CCR3 write needed afterwards).
 * @note Non-blocking: a single timer pass (RCR = slots - 1) reloads CCR3 from
 *       the caller's buffer on every slot-end (CC2) event. In direct mode the
 *       feed width must match PSIZE, so it runs at 16-bit directly from the
 *       ow_pulse_t buffer (uint16_t on this family) — zero-copy, no staging.
 */
__STATIC_FORCEINLINE uint8_t ow_port_feed(const ow_pulse_t* cmd, uint16_t slots) {
    if (slots == 0u || slots > ONEWIRE_MAX_SLOTS) {
        assert(0 && "ow_port_feed: slots out of range");
        return 0;
    }
    T1.RCR = slots - 1;
    T1.ARR = ONEWIRE_ONE_PULSE + ONEWIRE_ZERO_PULSE + ONEWIRE_GUARD_BAND; /* Total bit slot time */
    T1.CCR2 = ONEWIRE_ONE_PULSE + ONEWIRE_ZERO_PULSE; /* End-of-slot reload trigger */
    /* OC3 in PWM mode (no preload so the DMA reload is immediate). */
    T1.CCMR2 = TIM_CCMR2(OC3M_0, OC3M_1, OC3M_2);
    T1.CCER = TIM_CCER(CC3E);
    /* Keep the DMA request disconnected while the stream re-arms. */
#if OW_PORT_LOW_POWER
    T1.DIER = TIM_DIER(UIE);
#else
    T1.DIER = 0;
#endif
    ow_port_update_event();
    ow_port_dma_rearm(DMA2_Stream2, &DMA2->LIFCR,
                      DMA_LIFCR_CTCIF2 | DMA_LIFCR_CHTIF2 | DMA_LIFCR_CTEIF2 |
                          DMA_LIFCR_CDMEIF2 | DMA_LIFCR_CFEIF2);
    OW_PORT_DMA_FEED.PAR = (uint32_t)&T1.CCR3;
    OW_PORT_DMA_FEED.M0AR = (uint32_t)&cmd[1];
    OW_PORT_DMA_FEED.NDTR = slots; /* Feed slots 2..N, then the trailing 0 (bus release) */
    OW_PORT_DMA_FEED.CR = OW_PORT_DMA_CR_FEED | OW_PORT_DMA_CHSEL | DMA_SxCR_MSIZE_0 | DMA_SxCR_EN;
    /* Re-connect the slot-end reload, then re-arm the first slot last so a
     * stale CC2 request can never clobber it before the timer has started. */
#if OW_PORT_LOW_POWER
    T1.DIER = TIM_DIER(CC2DE, UIE);
#else
    T1.DIER = TIM_DIER(CC2DE);
#endif
    T1.CCR3 = cmd[0]; /* Slot 1 pulse */
    T1.CR1 = TIM_CR1(OPM, CEN);
    return 1u;
}

/**
 * @brief Start a hardware-timed wait (conversion wait / inter-cycle pause)
 * @param[in] arr Auto-reload value (one timer period in µs)
 * @param[in] rcr Repetition counter (number of periods - 1)
 */
__STATIC_FORCEINLINE void ow_port_start_timer(uint16_t arr, uint8_t rcr) {
    T1.ARR = arr;
    T1.RCR = rcr;
#if OW_PORT_LOW_POWER
    if ((uint32_t)(rcr + 1u) * arr > 1000u) {
        ow_long_pending = 1; /* long stage: conversion / EEPROM hold-off / pause */
        /* Enable the update interrupt so the pending bit wakes __WFE() via
         * SEVONPEND. ow_port_capture() already sets UIE for the long scratchpad
         * read stage, but start_timer() must too, else WFE sleeps forever. */
        T1.DIER |= TIM_DIER(UIE);
    }
#endif
    ow_port_update_event();
    T1.CR1 = TIM_CR1(OPM, CEN);
}

/**
 * @brief Schedule a 1-Wire bus reset with presence capture
 * @param[out] reset_pulses Buffer for the captured reset + presence pulse
 *                          durations (2 x 16-bit)
 */
__STATIC_FORCEINLINE void ow_port_reset(volatile uint16_t* reset_pulses) {
    T1.RCR = 0;
    T1.ARR = OW_PORT_RESET_TIMEOUT;
    T1.CCR3 = OW_PORT_RESET_PULSE_DURATION;
    /* Clear the capture buffer: only reset_pulses[0] (master release) is always
     * written by the DMA, so a no-presence reset would otherwise leave a stale
     * reset_pulses[1] from a previous presence reset and onewire_present() would
     * report a false device. Zeroing makes a single-capture reset report "no
     * device". */
    reset_pulses[0] = 0;
    reset_pulses[1] = 0;
    ow_port_capture(reset_pulses, OW_PORT_CAPTURE_BUF_SIZE, 16);
}

/**
 * @brief Schedule a write of `slots` bit slots
 * @param[in] pulses Pulse buffer (one entry per slot); for `slots > 1` the
 *                   entry at index `slots` must be 0 (hardware bus release)
 * @param[in] slots Number of bit slots to transmit, 1..ONEWIRE_MAX_SLOTS (256).
 * @return 1 if the write was scheduled, 0 if `slots` is out of range. In debug
 *         builds the reject path also traps with an assert.
 */
__STATIC_FORCEINLINE uint8_t ow_port_write_slots(const ow_pulse_t* pulses, uint16_t slots) {
    if (slots == 0u || slots > ONEWIRE_MAX_SLOTS) {
        assert(0 && "ow_port_write_slots: slots out of range");
        return 0;
    }
#if OW_DRIVE_ACTIVE
    ow_port_set_pin_mode(1); /* active-drive write: master drives both levels */
#endif
    if (slots == 1) {
        /* Single slot: no DMA needed, avoids a zero-length DMA transaction */
        T1.RCR = 0; /* Single slot, no repetition */
        T1.ARR = ONEWIRE_ONE_PULSE + ONEWIRE_ZERO_PULSE + ONEWIRE_GUARD_BAND; /* Total bit slot time */
        T1.CCR3 = pulses[0]; /* Pulse duration encodes the bit */
        /* OC3PE plus a preload zero release the bus at the terminal update
         * event, exactly when the one-pulse timer stops (hardware bus release). */
        T1.CCMR2 = TIM_CCMR2(OC3M_0, OC3M_1, OC3M_2, OC3PE);
        T1.CCER = TIM_CCER(CC3E);
#if OW_PORT_LOW_POWER
        T1.DIER = TIM_DIER(UIE); /* no DMA for a single bit slot; keep UIE for WFE */
#else
        T1.DIER = 0; /* No DMA for a single bit slot */
#endif
        ow_port_update_event();
        T1.CCR3 = 0; /* Preload 0 -> line idles HIGH when the timer stops */
        T1.CR1 = TIM_CR1(OPM, CEN);
        return 1u;
    }
    return ow_port_feed(pulses, slots);
}

/**
 * @brief Schedule a two-slot read of a Search ROM id/cmp bit pair
 * @param[out] pair_pulses Buffer for the captured pulse durations (2 x 16-bit)
 */
__STATIC_FORCEINLINE void ow_port_read_pair(volatile uint16_t* pair_pulses) {
    T1.RCR = 1; /* Two read slots, then a single update event */
    T1.ARR = ONEWIRE_ONE_PULSE + ONEWIRE_ZERO_PULSE + ONEWIRE_GUARD_BAND; /* Total bit slot time */
    T1.CCR3 = ONEWIRE_ONE_PULSE; /* Read pulse duration */
    T1.CCMR2 = TIM_CCMR2(OC3M_0, OC3M_1, OC3M_2, OC3PE, CC4S_1, OW_PORT_IC4F_ARGS);
    T1.CCER = TIM_CCER(CC3E, CC4E);
#if OW_PORT_LOW_POWER
    T1.DIER = TIM_DIER(CC4DE, UIE);
#else
    T1.DIER = TIM_DIER(CC4DE);
#endif
    ow_port_update_event();
    T1.CCR3 = 0; /* Clear the output-compare value (CCR4 capture is independent) */
    ow_port_dma_rearm(DMA2_Stream4, &DMA2->HIFCR,
                      DMA_HIFCR_CTCIF4 | DMA_HIFCR_CHTIF4 | DMA_HIFCR_CTEIF4 |
                          DMA_HIFCR_CDMEIF4 | DMA_HIFCR_CFEIF4);
    OW_PORT_DMA_CAPTURE.PAR = (uint32_t)&T1.CCR4;
    OW_PORT_DMA_CAPTURE.M0AR = (uint32_t)pair_pulses;
    OW_PORT_DMA_CAPTURE.NDTR = 2;
    OW_PORT_DMA_CAPTURE.CR = OW_PORT_DMA_CR_CAPTURE | OW_PORT_DMA_CHSEL |
                             DMA_SxCR_MSIZE_0 | DMA_SxCR_EN;
    T1.CR1 = TIM_CR1(OPM, CEN);
}

/**
 * @brief Merge the direction-bit write with the id/cmp read pair in one pass
 * @param[in] bit Direction bit to write in slot 1 (0 or 1)
 * @param[in] pulse3 Buffer for the three captured slots (write-slot capture,
 *                   id pulse, cmp pulse)
 * @param[in] read_pulse CCR3 reloads for read slots 2-3 (+ trailing 0)
 * @note Single timer pass (RCR=2, three slots) with two DMA2 streams armed
 *       together. The CC2 slot-end marker (Stream2; CCR2 = ONE+ZERO, frozen
 *       output) reloads CCR3 from read_pulse —
 *       ONEWIRE_ONE_PULSE for slots 2-3, then 0 during slot 3 so the one-pulse
 *       timer stops with the line released to idle HIGH — while CC4 (Stream4)
 *       captures the write-slot pulse plus the id/cmp pair into pulse3[0..2].
 *       OC3PE is off so the reload is immediate. DMA requests stay disconnected
 *       (DIER=UIE only) through the re-arm kick so a stale CC2 request can
 *       never fire the reload early; they are re-connected right before CEN
 *       with the direction pulse re-armed last.
 */
__STATIC_FORCEINLINE void ow_port_write_then_read(uint8_t bit, volatile uint16_t* pulse3,
                                                  const ow_pulse_t* read_pulse) {
#if OW_DRIVE_ACTIVE
    ow_port_set_pin_mode(0); /* merged write+read stays open-drain so the read half is safe */
#endif
    const ow_pulse_t write_pulse = bit ? ONEWIRE_ONE_PULSE : ONEWIRE_ZERO_PULSE;
    ow_port_marker_toggle(); /* LA marker: rising edge = merged pass starts here */
    /* Clean re-arm of both streams: clear all flags. */
    ow_port_dma_rearm(DMA2_Stream2, &DMA2->LIFCR,
                      DMA_LIFCR_CTCIF2 | DMA_LIFCR_CHTIF2 | DMA_LIFCR_CTEIF2 |
                          DMA_LIFCR_CDMEIF2 | DMA_LIFCR_CFEIF2);
    ow_port_dma_rearm(DMA2_Stream4, &DMA2->HIFCR,
                      DMA_HIFCR_CTCIF4 | DMA_HIFCR_CHTIF4 | DMA_HIFCR_CTEIF4 |
                          DMA_HIFCR_CDMEIF4 | DMA_HIFCR_CFEIF4);
    /* Arrange the timer pass: three slots, then a single update event. CC4 is
     * armed for the whole pass, so the write-slot falling edge is captured
     * into pulse3[0] as well as the id/cmp reads into [1] and [2]. */
    T1.RCR = 2;
    T1.ARR = ONEWIRE_ONE_PULSE + ONEWIRE_ZERO_PULSE + ONEWIRE_GUARD_BAND; /* Total bit slot time */
    T1.CCR2 = ONEWIRE_ONE_PULSE + ONEWIRE_ZERO_PULSE; /* End-of-slot reload trigger */
    /* OC3 in PWM mode (no preload so the DMA reload is immediate), CC4 capture armed */
    T1.CCMR2 = TIM_CCMR2(OC3M_0, OC3M_1, OC3M_2, CC4S_1, OW_PORT_IC4F_ARGS);
    T1.CCER = TIM_CCER(CC3E, CC4E);
    /* Keep the DMA requests disconnected while the channels re-arm. */
#if OW_PORT_LOW_POWER
    T1.DIER = TIM_DIER(UIE); /* keep UIE for WFE while the DMA requests are apart */
#else
    T1.DIER = 0;
#endif
    /* Capture stream: write-slot capture plus the id/cmp pulse pair. */
    OW_PORT_DMA_CAPTURE.PAR = (uint32_t)&T1.CCR4;
    OW_PORT_DMA_CAPTURE.M0AR = (uint32_t)pulse3;
    OW_PORT_DMA_CAPTURE.NDTR = 3;
    OW_PORT_DMA_CAPTURE.CR = OW_PORT_DMA_CR_CAPTURE | OW_PORT_DMA_CHSEL |
                             DMA_SxCR_MSIZE_0 | DMA_SxCR_EN;
    /* Feed stream: halfword-width reloads of CCR3 — read pulse for slots 2-3,
     * then the trailing 0 during slot 3 so the OPM stop hands the line back
     * idle HIGH (hardware bus release); fed zero-copy from read_pulse. */
    OW_PORT_DMA_FEED.PAR = (uint32_t)&T1.CCR3;
    OW_PORT_DMA_FEED.M0AR = (uint32_t)read_pulse;
    OW_PORT_DMA_FEED.NDTR = 3;
    OW_PORT_DMA_FEED.CR = OW_PORT_DMA_CR_FEED | OW_PORT_DMA_CHSEL |
                          DMA_SxCR_MSIZE_0 | DMA_SxCR_EN;
    /* UG kick: reload ARR/RCR preloads and hand the fresh operation a clean
     * UIF, then re-connect the DMA requests and start. The direction pulse is
     * re-armed after the DIER write so a stale CC2 request can never clobber
     * it before the timer runs. */
    ow_port_update_event();
#if OW_PORT_LOW_POWER
    T1.DIER = TIM_DIER(CC4DE, CC2DE, UIE); /* Capture + CCR3 reload via DMA (UIE for WFE) */
#else
    T1.DIER = TIM_DIER(CC4DE, CC2DE); /* Capture + CCR3 reload via DMA */
#endif
    T1.CCR3 = write_pulse; /* Re-arm the direction pulse (safe against a stale CC2 DMA reload) */
    T1.CR1 = TIM_CR1(OPM, CEN);
}

/**
 * @brief Schedule a read of `bytes` bytes from the bus
 * @param[out] dst Buffer for the captured pulse durations (bytes x 8 x 8-bit)
 * @param[in] bytes Number of bytes to read, 1..ONEWIRE_MAX_READ_BYTES (32).
 * @return 1 if the read was scheduled, 0 if `bytes` is out of range. In debug
 *         builds the reject path also traps with an assert.
 */
__STATIC_FORCEINLINE uint8_t ow_port_read_data(volatile uint8_t* dst, uint8_t bytes) {
    if (bytes == 0u || bytes > ONEWIRE_MAX_READ_BYTES) {
        assert(0 && "ow_port_read_data: bytes out of range");
        return 0;
    }
    const uint16_t bits = (uint16_t)bytes * ONEWIRE_BITS_PER_BYTE;
    T1.RCR = bits - 1;
    T1.ARR = ONEWIRE_ONE_PULSE + ONEWIRE_ZERO_PULSE + ONEWIRE_GUARD_BAND;
    T1.CCR3 = ONEWIRE_ONE_PULSE;
    ow_port_capture(dst, bits, 8);
    return 1u;
}

/**
 * @brief Engage or release the parasite-power strong pull-up on the bus
 * @param[in] on 1 drives the bus line HIGH actively, 0 releases it again
 * @note The pin stays in alternate-function mode (TIM1_CH3) at all times.
 *       Engaged: OTYPER.OT10 is cleared (alternate-function push-pull) so the
 *       AF output stage drives the line HIGH actively, sourcing the current
 *       parasite devices need during temperature conversion and EEPROM
 *       programming windows.  Released: OT10 is set again (AF open-drain), the
 *       AF output goes inactive (PWM mode 2 with the counter stopped at zero)
 *       so the pin floats HIGH via the external pull-up.  No BSRR or MODER
 *       writes needed: the timer is stopped (OPM) during the window, the
 *       output is inactive, and ODR is irrelevant in AF mode.
 */
__STATIC_FORCEINLINE void ow_port_strong_pullup(uint8_t on) {
    ow_port_set_pin_mode(on);
}

#endif /* OW_PORT_F4_H */