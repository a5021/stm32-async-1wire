/**
 * @file ow_port_g0.h
 * @brief STM32G0 backend: TIM1 (advanced) + DMA1 + DMAMUX + bus on PA10
 *
 * Header-only static inline implementation of the ow_port_* interface for
 * the STM32G031x6. The channel scheme is identical to the STM32F0 backend:
 * CH3 (PWM output, open-drain alternate function) drives the bus, CH4
 * captures in indirect mode on the same pin (CC4S routes IC4 to TI3), a
 * plain CH2 compare acts as the end-of-slot marker whose DMA request feeds
 * CCR3 from a precomputed pulse buffer, and TIM1 runs in one-pulse mode
 * (OPM) with the repetition counter (RCR) batching N slots into a single
 * update event (UIF).
 *
 * Two things differ from the F0/F1 families:
 *
 * Bus pin availability. The STM32G031F6P6 TSSOP20 package does not bond out
 * PA9/PA10. Instead, SYSCFG_CFGR1.PA11_RMP / PA12_RMP make the PA11/PA12
 * pads operate as PA9/PA10 GPIO ports (RM0444 §8.1.1): after the remap the
 * pads carry the PA9/PA10 peripheral signals and are configured entirely
 * through the logical pin-9/pin-10 register bits of GPIOA. ow_port_init()
 * sets both remap bits; every later access below uses the logical PA10
 * bits, exactly like the other backends. Consequence for applications:
 * while the driver is initialised, pads PA11/PA12 must not be used as
 * standalone GPIOs - configuring them clears the remap bits and silently
 * disconnects the bus.
 *
 * DMA request routing. This family has no fixed DMA request map and no
 * DMA_CSELR; a DMAMUX sits between peripherals and DMA channels. The feed
 * (TIM1_CC2 marker request, #21) rides DMAMUX channel 2 paired with
 * DMA1_Channel3, and the capture drain (TIM1_CH4 request, #23) rides DMAMUX
 * channel 3 paired with DMA1_Channel4. Request numbers come from RM0444
 * table 51 / the LL header (LL_DMAMUX_REQ_TIM1_CH2=21, LL_DMAMUX_REQ_TIM1_
 * CH4=23); the mux is programmed before the channel's EN bit is set. Note
 * that DMAMUX needs no separate RCC clock gate: DMA1EN covers it.
 *
 * Clock handling is shared: OW_PORT_SYSCLK_MHZ (onewire.h, default 64 =
 * HSI16 + PLL) drives the prescaler arithmetically and selects the CH4
 * input-capture filter via the shared <=16MHz slow-clock threshold.
 */

#ifndef OW_PORT_G0_H
#define OW_PORT_G0_H

#include "macro.h"
#include "onewire.h"
#include "stm32g0xx.h"

/* @brief Timer prescaler for 1µs resolution (PSC = SYSCLK / 1MHz - 1),
 *       derived from the shared OW_PORT_SYSCLK_MHZ knob in onewire.h. */
#define OW_PORT_TIM_PRESCALER ((OW_PORT_SYSCLK_MHZ) - 1u)

/* @brief CH4 input capture filter (IC4F), chosen to keep the filter latency
 *       clock-independent in µs. At 64MHz fDTS/4 with N=8 samples adds
 *       ~0.5µs; on a slow clock (≤16MHz) fDTS would sample at ~2MHz and add
 *       ~4µs, pushing '1' slot captures past ONEWIRE_SHORT_PULSE_MAX. On
 *       slow clocks use fCK_INT with N=4 (~0.5µs latency) so captures stay
 *       in the '1' window. Mirrors ow_port_f0.h. */
#if (OW_PORT_SYSCLK_MHZ) <= 16
#define OW_PORT_IC4F_ARGS IC4F_1 /* fCK_INT, N=4 -> ~0.5µs on slow clocks */
#else
#define OW_PORT_IC4F_ARGS IC4F_0, IC4F_1, IC4F_2 /* fDTS/4, N=8 -> ~0.5µs @64MHz */
#endif

/* @brief DMA channel assignment: channel 3 carries the CC2 slot-end marker
 *       request (feeds CCR3), channel 4 carries the CC4 capture request
 *       (drains CCR4). DMAMUX channel x is hard-wired to DMA1 channel x+1,
 *       so the feed rides DMAMUX channel 2 and captures ride channel 3. */
#define OW_PORT_DMA_FEED D13 /* DMA1_Channel3 */
#define OW_PORT_DMA_CAPTURE D14 /* DMA1_Channel4 */

/* @brief DMAMUX request numbers for this backend (RM0444 table 51):
 *       TIM1_CC2 = 21 (slot-end marker feeding CCR3),
 *       TIM1_CH4 = 23 (input capture draining CCR4). */
#define OW_PORT_DMAMUX_REQ_TIM1_CC2 21u
#define OW_PORT_DMAMUX_REQ_TIM1_CH4 23u

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
 * @brief Enable clocks, apply the PA11/PA12 remap, configure timer and bus
 * @note The remap makes the PA12 pad operate as the PA10 GPIO port (and the
 *       PA11 pad as PA9, kept for the application's USART1 TX). All GPIO
 *       configuration below addresses the logical pin-10 bits; see the file
 *       comment. DMAMUX shares the DMA1 clock gate - nothing to enable.
 */
__STATIC_FORCEINLINE void ow_port_init(void) {
    RC.AHBENR |= RCC_AHBENR_DMA1EN;
    RC.IOPENR |= RCC_IOPENR_GPIOAEN;
    RC.APBENR2 |= RCC_APBENR2_SYSCFGEN | RCC_APBENR2_TIM1EN;
    (void)RC.APBENR2; /* dummy read: settle the freshly-gated APB clock before
                         the first SYSCFG register access below */
    SYSCFG->CFGR1 |= SYSCFG_CFGR1_PA11_RMP | SYSCFG_CFGR1_PA12_RMP;
    T1.PSC = OW_PORT_TIM_PRESCALER;
    ow_port_kick(); /* kickstart: first poll advances immediately */
    T1.BDTR = TIM_BDTR(MOE);
    /* Logical PA10 (physical PA12 pad after remap): alternate function mode,
     * open-drain, AF2 (TIM1_CH3). */
    PA.MODER = (PA.MODER & ~GPIO_MODER_MODE10) | GPIO_MODER_MODE10_1;
    PA.OTYPER |= GPIO_OTYPER_OT10;
    PA.AFR[1] = (PA.AFR[1] & ~GPIO_AFRH_AFSEL10) | (2u << GPIO_AFRH_AFSEL10_Pos);
}

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
        T1.SR = 0;
        return 1u;
    }
    return 0u;
}

/**
 * @brief Configure timer, DMAMUX and DMA for a capture operation
 * @param[out] dst Destination buffer for captured data
 * @param[in] count Number of transfers
 * @param[in] width DMA transfer width: 8 for 8-bit, 16 for 16-bit
 */
__STATIC_FORCEINLINE void ow_port_capture(volatile void* dst, uint16_t count, uint16_t width) {
    T1.CCMR2 = TIM_CCMR2(OC3M_0, OC3M_1, OC3M_2, OC3PE, CC4S_1, OW_PORT_IC4F_ARGS);
    T1.CCER = TIM_CCER(CC3E, CC4E);
    T1.DIER = TIM_DIER(CC4DE);
    ow_port_update_event();
    T1.CCR3 = 0;
    OW_PORT_DMA_CAPTURE.CCR = 0;
    DMAMUX1_Channel3->CCR = OW_PORT_DMAMUX_REQ_TIM1_CH4; /* route CC4 captures */
    OW_PORT_DMA_CAPTURE.CPAR = (uint32_t)&T1.CCR4;
    OW_PORT_DMA_CAPTURE.CMAR = (uint32_t)dst;
    OW_PORT_DMA_CAPTURE.CNDTR = count;
    OW_PORT_DMA_CAPTURE.CCR = OW_PORT_DMA_CCR_CAPTURE | ((width == 16) ? DMA_CCR_MSIZE_0 : 0);
    T1.CR1 = TIM_CR1(OPM, CEN);
}

/**
 * @brief Transmit a command sequence of arbitrary length using DMA
 * @param[in] cmd Pointer to command sequence in pulse duration format
 * @param[in] slots Number of bit slots (bits) to transmit
 * @note The buffer must hold `slots + 1` entries and the entry at index
 *       `slots` must be 0: the final CC2-triggered DMA transfer feeds that
 *       trailing 0 into CCR3 during the last slot, so the one-pulse timer
 *       stops with the line already released to idle HIGH (hardware bus
 *       release — no software CCR3 write needed afterwards).
 */
__STATIC_FORCEINLINE void ow_port_feed(const uint8_t* cmd, uint16_t slots) {
    T1.RCR = slots - 1;
    T1.ARR = ONEWIRE_ONE_PULSE + ONEWIRE_ZERO_PULSE + ONEWIRE_GUARD_BAND;
    T1.CCR3 = cmd[0];
    T1.CCR2 = ONEWIRE_ONE_PULSE + ONEWIRE_ZERO_PULSE;
    T1.CCMR2 = TIM_CCMR2(OC3M_0, OC3M_1, OC3M_2);
    T1.CCER = TIM_CCER(CC3E);
    T1.DIER = TIM_DIER(CC2DE);
    ow_port_update_event();
    OW_PORT_DMA_FEED.CCR = 0;
    DMAMUX1_Channel2->CCR = OW_PORT_DMAMUX_REQ_TIM1_CC2; /* route CC2 marker feed */
    OW_PORT_DMA_FEED.CPAR = (uint32_t)&T1.CCR3;
    OW_PORT_DMA_FEED.CMAR = (uint32_t)&cmd[1];
    OW_PORT_DMA_FEED.CNDTR = slots; /* Feed slots 2..N, then the trailing 0 (bus release) */
    OW_PORT_DMA_FEED.CCR = DMA_CCR(DIR, MINC, PSIZE_0, EN);
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
    T1.CCR3 = OW_PORT_RESET_PULSE_DURATION;
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
        T1.CCR3 = pulses[0]; /* Pulse duration encodes the bit */
        /* OC3PE plus a preload zero release the bus at the terminal update
         * event, exactly when the one-pulse timer stops (hardware bus release). */
        T1.CCMR2 = TIM_CCMR2(OC3M_0, OC3M_1, OC3M_2, OC3PE);
        T1.CCER = TIM_CCER(CC3E);
        T1.DIER = 0; /* No DMA for a single bit slot */
        ow_port_update_event();
        T1.CCR3 = 0; /* Preload 0 -> line idles HIGH when the timer stops */
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
    T1.CCR3 = ONEWIRE_ONE_PULSE; /* Read pulse duration */
    T1.CCMR2 = TIM_CCMR2(OC3M_0, OC3M_1, OC3M_2, OC3PE, CC4S_1, OW_PORT_IC4F_ARGS);
    T1.CCER = TIM_CCER(CC3E, CC4E);
    T1.DIER = TIM_DIER(CC4DE);
    ow_port_update_event();
    T1.CCR3 = 0; /* Clear output compare value */
    OW_PORT_DMA_CAPTURE.CCR = 0;
    DMAMUX1_Channel3->CCR = OW_PORT_DMAMUX_REQ_TIM1_CH4;
    OW_PORT_DMA_CAPTURE.CPAR = (uint32_t)&T1.CCR4;
    OW_PORT_DMA_CAPTURE.CMAR = (uint32_t)edge_out;
    OW_PORT_DMA_CAPTURE.CNDTR = 2;
    OW_PORT_DMA_CAPTURE.CCR = DMA_CCR(MINC, PSIZE_0, MSIZE_0, EN);
    T1.CR1 = TIM_CR1(OPM, CEN);
}

/**
 * @brief Schedule a merged single-slot write followed by a two-slot read pair
 * @param[in] bit Direction bit to write in slot 1 (0 or 1)
 * @param[in] edge3 Buffer for the three captured edges (write slot, id, cmp)
 * @param[in] read_pulse CCR3 reloads for read slots 2-3 (+ trailing 0)
 */
__STATIC_FORCEINLINE void ow_port_write_then_read(uint8_t bit, volatile uint16_t* edge3,
                                                  const uint8_t* read_pulse) {
    const uint8_t write_pulse = bit ? (uint8_t)ONEWIRE_ONE_PULSE : (uint8_t)ONEWIRE_ZERO_PULSE;
    T1.RCR = 2; /* Three slots, then a single update event */
    T1.ARR = ONEWIRE_ONE_PULSE + ONEWIRE_ZERO_PULSE + ONEWIRE_GUARD_BAND; /* Total bit slot time */
    /* Arm the direction pulse first. The bus was released idle-high by
     * ow_port_bus_done(), so this write produces the single clean falling edge
     * the devices re-sync their slot timer to. Holding it from the top instead
     * of arming it right before CEN means the CC4 capture is armed while the
     * bus is low, so the open-drain RC rise can never be mistaken for a slot
     * edge. */
    T1.CCR3 = write_pulse; /* Slot 1 write pulse encodes the direction bit */
    T1.CCR2 = ONEWIRE_ONE_PULSE + ONEWIRE_ZERO_PULSE; /* End-of-slot reload trigger */
    /* OC3 in PWM mode (no preload so the reload is immediate), CC4 capture armed */
    T1.CCMR2 = TIM_CCMR2(OC3M_0, OC3M_1, OC3M_2, CC4S_1, OW_PORT_IC4F_ARGS);
    T1.CCER = TIM_CCER(CC3E, CC4E); /* Enable both channels */
    /* Disconnect DMA requests while re-arming the channels, then re-connect
     * them only after the timer flags are clean and just before starting.
     * (The end-of-slot CC2 compare event of the previous merged operation can
     * leave a pending request that fires the reload DMA immediately on re-arm,
     * overwriting the freshly written direction pulse in CCR3.) */
    T1.DIER = 0;
    ow_port_update_event();
    /* Capture DMA: all three slot edges into the merged-edge buffer */
    OW_PORT_DMA_CAPTURE.CCR = 0;
    DMAMUX1_Channel3->CCR = OW_PORT_DMAMUX_REQ_TIM1_CH4;
    OW_PORT_DMA_CAPTURE.CPAR = (uint32_t)&T1.CCR4;
    OW_PORT_DMA_CAPTURE.CMAR = (uint32_t)edge3;
    OW_PORT_DMA_CAPTURE.CNDTR = 3;
    OW_PORT_DMA_CAPTURE.CCR = DMA_CCR(MINC, PSIZE_0, MSIZE_0, EN);
    /* Feed DMA: reload CCR3 with the read pulse for slots 2-3, then write the
     * trailing 0 during slot 3 so the one-pulse timer stops with the line
     * released to idle HIGH (hardware bus release). */
    OW_PORT_DMA_FEED.CCR = 0;
    DMAMUX1_Channel2->CCR = OW_PORT_DMAMUX_REQ_TIM1_CC2;
    OW_PORT_DMA_FEED.CPAR = (uint32_t)&T1.CCR3;
    OW_PORT_DMA_FEED.CMAR = (uint32_t)read_pulse;
    OW_PORT_DMA_FEED.CNDTR = 3;
    OW_PORT_DMA_FEED.CCR = DMA_CCR(DIR, MINC, PSIZE_0, EN);
    T1.SR = 0; /* Clear any pending capture/compare flags before enabling DMA requests */
    T1.DIER = TIM_DIER(CC4DE, CC2DE); /* Capture + CCR3 reload via DMA */
    T1.CCR3 = write_pulse; /* Re-arm the direction pulse (safe against a stale CC2 DMA reload) */
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
    T1.CCR3 = ONEWIRE_ONE_PULSE;
    ow_port_capture((volatile void*)dst, bits, 8);
}

/**
 * @brief Engage or release the parasite-power strong pull-up on the bus
 * @param[in] on 1 drives the bus line HIGH actively, 0 releases it again
 * @note Engaged: logical PA10 leaves the timer-driven alternate-function mode
 *       and becomes a general purpose push-pull output driving HIGH,
 *       sourcing the current parasite devices need during temperature
 *       conversion and EEPROM programming windows. Released: alternate
 *       function open-drain is restored, handing the pin back to TIM1_CH3
 *       (which idles with the output released, so the external pull-up takes
 *       over). The BSRR write happens before the mode switch so the push-pull
 *       stage never actively drives LOW - engaging can only pull the line up;
 *       releasing restores open-drain before leaving output mode for the same
 *       reason. Register names follow the G0 CMSIS spelling (MODE10/OT10).
 */
__STATIC_FORCEINLINE void ow_port_strong_pullup(uint8_t on) {
    if (on) {
        PA.BSRR = GPIO_BSRR_BS10; /* Output data HIGH before switching mode */
        PA.MODER = (PA.MODER & ~GPIO_MODER_MODE10) | GPIO_MODER_MODE10_0; /* AF -> output */
        PA.OTYPER &= ~GPIO_OTYPER_OT10; /* Open-drain -> push-pull */
    } else {
        PA.OTYPER |= GPIO_OTYPER_OT10; /* Push-pull -> open-drain (line released) */
        PA.MODER = (PA.MODER & ~GPIO_MODER_MODE10) | GPIO_MODER_MODE10_1; /* Output -> AF */
    }
}

#endif /* OW_PORT_G0_H */
