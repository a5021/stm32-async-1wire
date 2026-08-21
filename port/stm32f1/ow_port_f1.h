/**
 * @file ow_port_f1.h
 * @brief STM32F1 backend: TIM1 (advanced) + DMA1 (channels 3/6) + PA8
 *
 * Header-only static inline implementation of the ow_port_* interface for the
 * STM32F103. The 1-Wire bus runs on PA8 (TIM1_CH1 PWM output in open-drain
 * alternate function); CH2 captures in indirect mode on the same pin and DMA1
 * channel 3 moves CCR2 captures to memory, while channel 3 feeds CCR1 from a
 * precomputed pulse buffer on the CC3 compare event (DMA1 channel 6). TIM1
 * runs in one-pulse mode (OPM) with the repetition counter (RCR) batching N
 * slots into a single update event (UIF).
 *
 * The CCR1 feed uses channel 3 (CC3DE) instead of channel 4 so the feed logic
 * is identical across the STM32F1 (DMA1 channel 6) and STM32F0 (DMA1 channel
 * 5, the only TIM1 channel with a DMA request that can reload CCR1) backends.
 */

#ifndef OW_PORT_F1_H
#define OW_PORT_F1_H

#include "onewire.h"
#include "macro.h"
#include "stm32f1xx.h"

/* @brief Timer prescaler for 1µs resolution (PSC = SYSCLK / 1MHz - 1) */
#if defined(HSI_8MHZ)
#define OW_PORT_TIM_PRESCALER 7u /* 8MHz / 8 = 1MHz -> 1µs/tick */
#else
#define OW_PORT_TIM_PRESCALER 71u /* 72MHz / 72 = 1MHz -> 1µs/tick */
#endif

/* @brief CH2 input capture filter (IC2F), chosen to keep the filter latency
 *       clock-independent in µs. At 72MHz fDTS/4 with N=8 samples adds
 *       ~0.44µs; the same setting at 8MHz HSI would sample at 2MHz and add
 *       ~4µs, pushing '1' slot captures from ~8µs to 11-12µs — past
 *       ONEWIRE_SHORT_PULSE_MAX. On the slow clock use fCK_INT with N=4
 *       (~0.5µs latency) so captures stay in the '1' window. */
#if defined(HSI_8MHZ)
#define OW_PORT_IC2F_ARGS IC2F_1 /* fCK_INT, N=4 -> ~0.5µs @8MHz */
#else
#define OW_PORT_IC2F_ARGS IC2F_0, IC2F_1, IC2F_2 /* fDTS/4, N=8 -> ~0.44µs @72MHz */
#endif

/* @brief DMA channel control bits for 16-bit capture: MINC | PSIZE_0 | EN */
#define OW_PORT_DMA_CCR_CAPTURE (DMA_CCR_MINC | DMA_CCR_PSIZE_0 | DMA_CCR_EN)

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
 *       scheduled operation has a clean completion flag.
 */
__STATIC_FORCEINLINE void ow_port_update_event(void) {
    T1.EGR = TIM_EGR(UG);
    __DSB();
    T1.SR &= ~TIM_SR(UIF);
}

/**
 * @brief Enable clocks, configure the timer prescaler and PA8 open-drain AF
 */
__STATIC_FORCEINLINE void ow_port_init(void) {
    RC.APB2ENR |= RCC_APB2ENR(IOPAEN, TIM1EN);
    RC.AHBENR |= RCC_AHBENR(DMA1EN);
    T1.PSC = OW_PORT_TIM_PRESCALER;
    ow_port_kick(); /* kickstart: first poll advances immediately */
    T1.BDTR = TIM_BDTR(MOE);
    PA.CRH |= GPIO_CRH(CNF8_0, CNF8_1, MODE8_1);
}

/**
 * @brief Non-blocking completion check for the scheduled operation
 * @return 1 if finished (update flag set and cleared), 0 while still running
 */
__STATIC_FORCEINLINE uint8_t ow_port_bus_done(void) {
    if (T1.SR & TIM_SR(UIF)) {
        /* No software bus release needed: every operation returns the line to
         * idle HIGH in hardware. DMA-fed writes (ow_port_feed,
         * ow_port_write_then_read) append a trailing 0 to the CCR1 feed, and
         * the direct-write/capture operations (reset, read, single slot) use
         * an OC1PE preload of 0 — both applied exactly when the one-pulse
         * timer stops. */
        T1.SR = 0;
        return 1u;
    }
    return 0u;
}

/**
 * @brief Configure timer and DMA for a capture operation
 * @param[out] dst Destination buffer for captured data
 * @param[in] count Number of transfers
 * @param[in] width DMA transfer width: 8 for 8-bit, 16 for 16-bit
 */
__STATIC_FORCEINLINE void ow_port_capture(volatile void* dst, uint16_t count, uint16_t width) {
    T1.CCMR1 = TIM_CCMR1(OC1M_0, OC1M_1, OC1M_2, OC1PE, CC2S_1, OW_PORT_IC2F_ARGS);
    T1.CCER = TIM_CCER(CC1E, CC2E);
    T1.DIER = TIM_DIER(CC2DE);
    ow_port_update_event();
    T1.CCR1 = 0;
    D13.CCR = 0;
    D13.CPAR = (uint32_t)&T1.CCR2;
    D13.CMAR = (uint32_t)dst;
    D13.CNDTR = count;
    D13.CCR = OW_PORT_DMA_CCR_CAPTURE | ((width == 16) ? DMA_CCR_MSIZE_0 : 0);
    T1.CR1 = TIM_CR1(OPM, CEN);
}

/**
 * @brief Transmit a command sequence of arbitrary length using DMA
 * @param[in] cmd Pointer to command sequence in pulse duration format
 * @param[in] slots Number of bit slots (bits) to transmit
 * @note The buffer must hold `slots + 1` entries and the entry at index
 *       `slots` must be 0: the final CC3-triggered DMA transfer feeds that
 *       trailing 0 into CCR1 during the last slot, so the one-pulse timer
 *       stops with the line already released to idle HIGH (hardware bus
 *       release — no software CCR1 write needed afterwards).
 */
__STATIC_FORCEINLINE void ow_port_feed(const uint8_t* cmd, uint16_t slots) {
    T1.RCR = slots - 1;
    T1.ARR = ONEWIRE_ONE_PULSE + ONEWIRE_ZERO_PULSE + ONEWIRE_GUARD_BAND;
    T1.CCR1 = cmd[0];
    T1.CCR3 = ONEWIRE_ONE_PULSE + ONEWIRE_ZERO_PULSE;
    T1.CCMR1 = TIM_CCMR1(OC1M_0, OC1M_1, OC1M_2);
    T1.CCER = TIM_CCER(CC1E);
    T1.DIER = TIM_DIER(CC3DE);
    ow_port_update_event();
    D16.CCR = 0;
    D16.CPAR = (uint32_t)&T1.CCR1;
    D16.CMAR = (uint32_t)&cmd[1];
    D16.CNDTR = slots; /* Feed slots 2..N, then the trailing 0 (bus release) */
    D16.CCR = DMA_CCR(DIR, MINC, PSIZE_0, EN);
    T1.CR1 = TIM_CR1(OPM, CEN);
}

/**
 * @brief Start a hardware-timed wait (conversion wait / inter-cycle pause)
 * @param[in] arr Auto-reload value (one timer period in µs)
 * @param[in] rcr Repetition counter (number of periods - 1)
 */
__STATIC_FORCEINLINE void ow_port_start_timer(uint16_t arr, uint8_t rcr) {
    T1.ARR = arr;
    T1.RCR = rcr;
    ow_port_update_event();
    T1.CR1 = TIM_CR1(OPM, CEN);
}

/**
 * @brief Schedule a 1-Wire bus reset with presence capture
 * @param[out] edge_out Buffer for the captured edge timestamps (2 x 16-bit)
 */
__STATIC_FORCEINLINE void ow_port_reset(volatile uint16_t* edge_out) {
    T1.RCR = 0;
    T1.ARR = OW_PORT_RESET_TIMEOUT;
    T1.CCR1 = OW_PORT_RESET_PULSE_DURATION;
    /* Clear the capture buffer: only edge[0] (master release) is always written
     * by the DMA, so a no-presence reset would otherwise leave a stale edge[1]
     * from a previous presence reset and onewire_present() would report a false
     * device. Zeroing makes a single-capture reset report "no device". */
    edge_out[0] = 0;
    edge_out[1] = 0;
    ow_port_capture((volatile void*)edge_out, OW_PORT_CAPTURE_BUF_SIZE, 16);
}

/**
 * @brief Schedule a write of `slots` bit slots
 * @param[in] pulses Pulse buffer (one entry per slot); for `slots > 1` the
 *                   entry at index `slots` must be 0 (hardware bus release)
 * @param[in] slots Number of bit slots to transmit
 */
__STATIC_FORCEINLINE void ow_port_write_slots(const uint8_t* pulses, uint16_t slots) {
    if (slots == 1) {
        /* Single slot: no DMA needed, avoids a zero-length DMA transaction */
        T1.RCR = 0; /* Single slot, no repetition */
        T1.ARR = ONEWIRE_ONE_PULSE + ONEWIRE_ZERO_PULSE + ONEWIRE_GUARD_BAND; /* Total bit slot time */
        T1.CCR1 = pulses[0]; /* Pulse duration encodes the bit */
        /* OC1PE plus a preload zero release the bus at the terminal update
         * event, exactly when the one-pulse timer stops (hardware bus release). */
        T1.CCMR1 = TIM_CCMR1(OC1M_0, OC1M_1, OC1M_2, OC1PE);
        T1.CCER = TIM_CCER(CC1E);
        T1.DIER = 0; /* No DMA for a single bit slot */
        ow_port_update_event();
        T1.CCR1 = 0; /* Preload 0 -> line idles HIGH when the timer stops */
        T1.CR1 = TIM_CR1(OPM, CEN);
        return;
    }
    ow_port_feed(pulses, slots);
}

/**
 * @brief Schedule a two-slot read of a Search ROM id/cmp bit pair
 * @param[out] edge_out Buffer for the captured edge timestamps (2 x 16-bit)
 */
__STATIC_FORCEINLINE void ow_port_read_pair(volatile uint16_t* edge_out) {
    T1.RCR = 1; /* Two read slots, then a single update event */
    T1.ARR = ONEWIRE_ONE_PULSE + ONEWIRE_ZERO_PULSE + ONEWIRE_GUARD_BAND; /* Total bit slot time */
    T1.CCR1 = ONEWIRE_ONE_PULSE; /* Read pulse duration */
    T1.CCMR1 = TIM_CCMR1(OC1M_0, OC1M_1, OC1M_2, OC1PE, CC2S_1, OW_PORT_IC2F_ARGS);
    T1.CCER = TIM_CCER(CC1E, CC2E);
    T1.DIER = TIM_DIER(CC2DE);
    ow_port_update_event();
    T1.CCR1 = 0; /* Clear output compare value */
    D13.CCR = 0;
    D13.CPAR = (uint32_t)&T1.CCR2;
    D13.CMAR = (uint32_t)edge_out;
    D13.CNDTR = 2;
    D13.CCR = DMA_CCR(MINC, PSIZE_0, MSIZE_0, EN);
    T1.CR1 = TIM_CR1(OPM, CEN);
}

/**
 * @brief Schedule a merged single-slot write followed by a two-slot read pair
 * @param[in] bit Direction bit to write in slot 1 (0 or 1)
 * @param[in] edge3 Buffer for the three captured edges (write slot, id, cmp)
 * @param[in] read_pulse CCR1 reloads for read slots 2-3 (+ trailing 0)
 */
__STATIC_FORCEINLINE void ow_port_write_then_read(uint8_t bit, volatile uint16_t* edge3,
                                                 const uint8_t* read_pulse) {
    const uint8_t write_pulse = bit ? (uint8_t)ONEWIRE_ONE_PULSE : (uint8_t)ONEWIRE_ZERO_PULSE;
    T1.RCR = 2; /* Three slots, then a single update event */
    T1.ARR = ONEWIRE_ONE_PULSE + ONEWIRE_ZERO_PULSE + ONEWIRE_GUARD_BAND; /* Total bit slot time */
    /* Arm the direction pulse first. The bus was released idle-high by
     * ow_port_bus_done(), so this write produces the single clean falling edge
     * the devices re-sync their slot timer to. Holding it from the top instead
     * of arming it right before CEN means the CC2 capture is armed while the
     * bus is low, so the open-drain RC rise can never be mistaken for a slot
     * edge. */
    T1.CCR1 = write_pulse; /* Slot 1 write pulse encodes the direction bit */
    T1.CCR3 = ONEWIRE_ONE_PULSE + ONEWIRE_ZERO_PULSE; /* End-of-slot reload trigger */
    /* OC1 in PWM mode (no preload so the reload is immediate), CC2 capture armed */
    T1.CCMR1 = TIM_CCMR1(OC1M_0, OC1M_1, OC1M_2, CC2S_1, OW_PORT_IC2F_ARGS);
    T1.CCER = TIM_CCER(CC1E, CC2E); /* Enable both channels */
    /* Disconnect DMA requests while re-arming the channels, then re-connect
     * them only after the timer flags are clean and just before starting.
     * (The end-of-slot CC3 compare event of the previous merged operation can
     * leave a pending request that fires the reload DMA immediately on re-arm,
     * overwriting the freshly written direction pulse in CCR1.) */
    T1.DIER = 0;
    ow_port_update_event();
    /* DMA Ch3: capture all three slot edges into the merged-edge buffer */
    D13.CCR = 0;
    D13.CPAR = (uint32_t)&T1.CCR2;
    D13.CMAR = (uint32_t)edge3;
    D13.CNDTR = 3;
    D13.CCR = DMA_CCR(MINC, PSIZE_0, MSIZE_0, EN);
    /* DMA Ch6: reload CCR1 with the read pulse for slots 2-3, then write the
     * trailing 0 during slot 3 so the one-pulse timer stops with the line
     * released to idle HIGH (hardware bus release). */
    D16.CCR = 0;
    D16.CPAR = (uint32_t)&T1.CCR1;
    D16.CMAR = (uint32_t)read_pulse;
    D16.CNDTR = 3;
    D16.CCR = DMA_CCR(DIR, MINC, PSIZE_0, EN);
    T1.SR = 0; /* Clear any pending capture/compare flags before enabling DMA requests */
    T1.DIER = TIM_DIER(CC2DE, CC3DE); /* Capture + CCR1 reload via DMA */
    T1.CCR1 = write_pulse; /* Re-arm the direction pulse (safe against a stale CC3 DMA reload) */
    T1.CR1 = TIM_CR1(OPM, CEN);
}

/**
 * @brief Schedule a read of `bytes` bytes from the bus
 * @param[out] dst Buffer for the captured pulse durations (bytes x 8 x 8-bit)
 * @param[in] bytes Number of bytes to read
 */
__STATIC_FORCEINLINE void ow_port_read_data(volatile uint8_t* dst, uint8_t bytes) {
    const uint16_t bits = (uint16_t)bytes * ONEWIRE_BITS_PER_BYTE;
    T1.RCR = bits - 1;
    T1.ARR = ONEWIRE_ONE_PULSE + ONEWIRE_ZERO_PULSE + ONEWIRE_GUARD_BAND;
    T1.CCR1 = ONEWIRE_ONE_PULSE;
    ow_port_capture((volatile void*)dst, bits, 8);
}

#endif /* OW_PORT_F1_H */