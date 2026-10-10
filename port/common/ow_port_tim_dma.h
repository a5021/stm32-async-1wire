/* ============================================================
 *  ow_port_tim_dma.h - the TIM1 + DMA core shared by all seven families
 *
 *  The 1-Wire bus on all seven of these families is the same machine: TIM1 in
 *  one-pulse mode drives the slot pulse on CH3, CH4 captures the bus in
 *  indirect mode, and a plain CH2 compare at ONE+ZERO us is the end-of-slot
 *  marker that triggers the feed DMA into CCR3.  Those 17 functions are one
 *  piece of code, not four that happen to look alike - they were four, because
 *  each backend was written against its own reference manual and then never
 *  merged.
 *
 *  What actually differs between families is small enough to name: the clock
 *  gates in ow_port_init(), the bus-pin mux, and the DMA request routing.
 *  Clocks stay per-family (OW_PORT_ENABLE_BUS_CLOCKS), and so does the pin
 *  mux: OW_PORT_CONFIG_BUS_PIN() and OW_PORT_SET_PIN_MODE() are two short
 *  statement macros per backend (F1's legacy CRH field proves the shapes are
 *  family-owned, not shared).  DMA request routing defaults to a no-op (fixed-map families) and G0/G4
 *  program their DMAMUX through the same two macros.
 *
 *  A family header that includes this must, before the include:
 *
 *    - include onewire.h (which pulls in the same backend), ow_bits.h and its
 *      own device header: ow_bits.h supplies the T1/PA/RC/D1x aliases this
 *      uses (including the D13/D14 channel defaults below), and the family
 *      facts block up in the headers supplies ow_pulse_t,
 *      OW_PORT_SYSCLK_MHZ and OW_PORT_TIM1_UPD_IRQn;
 *    - define OW_PORT_ENABLE_BUS_CLOCKS(), which gates this family's clocks;
 *    - define OW_PORT_CONFIG_BUS_PIN() and OW_PORT_SET_PIN_MODE(push_pull),
 *      which put the bus pin into alternate-function open-drain mode and
 *      toggle it to push-pull for the parasite strong pull-up;
 *    - optionally override OW_PORT_DMA_FEED / OW_PORT_DMA_CAPTURE (default
 *      D13/D14: TIM1_CC2 feed on channel 3, TIM1_CH4 capture on channel 4)
 *      and OW_PORT_ROUTE_CAPTURE() / OW_PORT_ROUTE_FEED() (default no-ops
 *      for fixed request maps; G0 programs its DMAMUX here).  The channel
 *      assignment is the one thing here that is not interchangeable - read
 *      the family header's assignment against its reference manual.
 *
 *  There is no run-time cost to the factoring: everything stays
 *  __STATIC_FORCEINLINE and every difference is resolved by the preprocessor,
 *  so a family's object code is what it was before the split.  That is checked,
 *  not assumed - the .bin of every example for the families already sharing
 *  this core is byte-identical before and after the split that introduced it.
 *
 *  F4 rides the same core through accessor macros with DMA1 defaults (see the
 *  OW_PORT_DMA_* block below) that the F4 shim overrides — same bodies,
 *  family-spelled registers, with init clocks/pins/drive arriving through
 *  the OW_PORT_ENABLE_BUS_CLOCKS/OW_PORT_CONFIG_BUS_PIN hooks — and H5
 *  through GPDMA CTR1/CTR2/REQSEL vocabulary overrides the same way.  Even the
 *  merged write+read pass is shared by all seven families with no
 *  per-family knobs left: two arm-order experiments on F446 (UG-vs-DMA order,
 *  early direction-pulse arm) both behave identically, so one order serves
 *  all - see the note above ow_port_write_then_read.
 * ============================================================ */

#ifndef OW_PORT_TIM_DMA_H
#define OW_PORT_TIM_DMA_H

/* The core is only usable inside a family header that declared its pin/timer/
 * DMA plumbing first.  Clocks are always per-family (checked below), and so
 * is the pin mux (two statement macros, checked below); DMA channels and
 * request routing have defaults a family overrides only when its hardware
 * differs. */
/* Default DMA channel assignment: feed rides TIM1_CC2 -> channel 3, capture
 * rides TIM1_CH4 -> channel 4 (D13/D14 from ow_bits.h).  These are not
 * arbitrary channel numbers: each backend's pair was read out of its own
 * reference manual and bench-verified (F0/F1/F3/G0/G4 14/14 matrices, F4 LA,
 * H5 GPDMA channels 2/3 with REQSEL 59/61 pending LA proof),
 * and a wrong pair is silent (feed never fires, captures read back empty).
 * A backend on different DMA IP (F4: DMA2 streams) overrides both.  There is
 * deliberately no #error here: D13/D14 resolve through ow_bits.h, which every
 * backend includes before this core, so a header that forgot its own device
 * include still fails loudly on the missing device macros. */
#ifndef OW_PORT_DMA_FEED
#define OW_PORT_DMA_FEED D13
#endif
#ifndef OW_PORT_DMA_CAPTURE
#define OW_PORT_DMA_CAPTURE D14
#endif
/* Default request routing: no-op (fixed request maps need no programming).
 * G0/G4 program their DMAMUX through these. */
#ifndef OW_PORT_ROUTE_CAPTURE
#define OW_PORT_ROUTE_CAPTURE() \
    do {                        \
    } while (0)
#endif
#ifndef OW_PORT_ROUTE_FEED
#define OW_PORT_ROUTE_FEED() \
    do {                     \
    } while (0)
#endif
#ifndef OW_PORT_ENABLE_BUS_CLOCKS
#error "ow_port_tim_dma.h: define OW_PORT_ENABLE_BUS_CLOCKS() before including this core"
#endif
#ifndef OW_PORT_CONFIG_BUS_PIN
#error "ow_port_tim_dma.h: define OW_PORT_CONFIG_BUS_PIN() before including this core"
#endif
#ifndef OW_PORT_SET_PIN_MODE
#error "ow_port_tim_dma.h: define OW_PORT_SET_PIN_MODE(push_pull) before including this core"
#endif

/* --- CH4 input-capture digital filter (IC4F): one selection for every clock,
 *     living here because the shared core is the one place all seven families
 *     are processed.  The F4 backend uses this same ladder through the core
 *     (it overrides only the DMA register spelling, not the timer setup), and
 *     so does H5 (GPDMA CTR1/CTR2/REQSEL overrides only).
 *
 *     ICxF is a 4-bit ladder.  The values this port actually programs, decoded
 *     from the RM0090 table (the same one ST's stm32f0xx_ll_tim.h spells out),
 *     are:
 *
 *       ICxF_1                 = 0b0001 = fCK_INT/16, N=1
 *       ICxF_0|ICxF_1          = 0b0011 = fCK_INT/4,  N=1
 *       ICxF_0|ICxF_1|ICxF_2   = 0b0111 = fCK_INT/4,  N=2
 *       ICxF_3                 = 0b1000 = fCK_INT/32, N=3
 *       ICxF_0|ICxF_1|ICxF_3   = 0b1011 = fCK_INT/4,  N=3
 *
 *     So the >168MHz entry is fCK_INT/4 N=3, a *shorter* window than the
 *     168MHz entry's fCK_INT/32 N=3.  That is deliberate and was validated at
 *     180MHz on the full 7-example matrix (see port/stm32f4/HARDWARE-NOTES.md).
 *     A "~500ns target" does not exist at these kernel clocks: the ladder's
 *     finest window is fCK_INT/16 (5.6ns at 180MHz), so reasoning should start
 *     from the decoded values.
 *
 *     Backends feed the macro into TIM_CCMR2(...) unchanged.  The default can
 *     be overridden from the build (-DOW_PORT_IC4F_ARGS=...) to sweep the
 *     filter on a bench.
 *     Test: tests/test/test_timing.c::test_ic4f_matches_the_documented_tier() */
#ifndef OW_PORT_IC4F_ARGS
#if (OW_PORT_SYSCLK_MHZ) <= 8
#define OW_PORT_IC4F_ARGS IC4F_1 /* 0b0001 = fCK_INT/16, N=1 */
#elif (OW_PORT_SYSCLK_MHZ) <= 16
#define OW_PORT_IC4F_ARGS IC4F_0, IC4F_1 /* 0b0011 = fCK_INT/4, N=1 */
#elif (OW_PORT_SYSCLK_MHZ) <= 72
#define OW_PORT_IC4F_ARGS IC4F_0, IC4F_1, IC4F_2 /* 0b0111 = fCK_INT/4, N=2 */
#elif (OW_PORT_SYSCLK_MHZ) <= 168
#define OW_PORT_IC4F_ARGS IC4F_3 /* 0b1000 = fCK_INT/32, N=3 */
#else
#define OW_PORT_IC4F_ARGS IC4F_0, IC4F_1, IC4F_3 /* 0b1011 = fCK_INT/4, N=3 */
#endif
#endif

/* OC3 output-compare preload (OC3PE) knob, see ow_port.h for the rationale.
 * Or-ed into the CCMR2 mask on the write/capture paths that rely on it
 * (capture, read_pair, single-slot write); the DMA-fed paths leave it off.
 * The default (1 = preload on) is identical to the historical hard-coded
 * behaviour; override with -DOW_PORT_OC3PE=0/-DOW_PORT_OC3PE=1 to sweep it on
 * a bench. OW_PORT_OC3PE_ARGS is the single bit value contributed to the
 * mask. */
#ifndef OW_PORT_OC3PE
#define OW_PORT_OC3PE 1
#endif
#if OW_PORT_OC3PE
#define OW_PORT_OC3PE_ARGS TIM_CCMR2_OC3PE
#else
#define OW_PORT_OC3PE_ARGS 0
#endif

/* Prescaler for 1 us resolution: PSC = SYSCLK / 1MHz - 1, from the shared
 * OW_PORT_SYSCLK_MHZ.  INVARIANT: TIM1's kernel clock must equal SYSCLK.
 * How each family gets there is family-specific and documented in its
 * backend: F0/F1/F3/G0/G4 leave the APB prescaler feeding TIM1 at /1, so
 * TIM1 = PCLK = SYSCLK directly; F4 programs PPRE2=/2 (PPRE1=/4 or /2) and
 * relies on the STM32 x2 doubling, so TIM1 = 2 x PCLK2 = SYSCLK either way.
 * Which APB the timer sits on, and why that family's prescalers are what
 * they are, is documented in each backend; it is checked by
 * tests/test/test_timing.c::test_apb_prescaler_div1_for_tim1(). */
#define OW_PORT_TIM_PRESCALER ((OW_PORT_SYSCLK_MHZ) - 1u)
_Static_assert(OW_PORT_TIM_PRESCALER <= 0xFFFFu,
               "TIM prescaler exceeds 16-bit PSC register width");

/* DMA control-value accessors (port-unification vocabulary): the exact
 * CR/CCR word each DMA-programming site writes, split by transfer class.
 * Defaults are the DMA1 (channel) spelling; the F4 shim overrides all
 * three (stream spelling + CHSEL).  Each equals the per-site expression it
 * will replace in the body migration:
 *  - RX16: 16-bit capture (reset/presence, read pair, merged capture);
 *  - RX8: 8-bit capture (scratchpad reads): DMA1 keeps a 16-bit
 *    peripheral read narrowed to 8-bit memory, while F4 direct mode forces
 *    the memory width to PSIZE, so F4 reads CCR4 bytes (no PSIZE bit);
 *  - TX: CCR3 reload: the memory width follows ow_pulse_t (8-bit
 *    everywhere except F4's 16-bit halfwords), hence MSIZE_0 only on F4.
 * Pinned per target by tests/test/test_dma_contract.c::test_dma_cr_value_macros. */
#ifndef OW_PORT_DMA_CR_RX16
#define OW_PORT_DMA_CR_RX16 (DMA_CCR_MINC | DMA_CCR_PSIZE_0 | DMA_CCR_MSIZE_0 | DMA_CCR_EN)
#endif
#ifndef OW_PORT_DMA_CR_RX8
#define OW_PORT_DMA_CR_RX8 (DMA_CCR_MINC | DMA_CCR_PSIZE_0 | DMA_CCR_EN)
#endif
#ifndef OW_PORT_DMA_CR_TX
#define OW_PORT_DMA_CR_TX DMA_CCR(DIR, MINC, PSIZE_0, EN)
#endif

/* DMA disable + programming statement macros (port-unification vocabulary):
 * the register-name level differs per DMA IP (DMA1 channels: CCR/CPAR/CMAR/
 * CNDTR; F4 streams: CR/PAR/M0AR/NDTR + CHSEL inside the CR word above), so
 * the bodies call these instead of spelling registers.  Defaults are the
 * DMA1 form; the F4 shim overrides DISABLE (stream CR + flag store) and the
 * PROG pair (stream spelling).  Pinned end to end by
 * tests/test/test_dma_contract.c (per-operation CCR/CPAR/CMAR/CNDTR table). */
#ifndef OW_PORT_DMA_EN_BIT
#define OW_PORT_DMA_EN_BIT DMA_CCR_EN
#endif
#ifndef OW_PORT_DMA_DISABLE_CAPTURE
#define OW_PORT_DMA_DISABLE_CAPTURE() ow_port_dma_disable(&OW_PORT_DMA_CAPTURE.CCR)
#endif
#ifndef OW_PORT_DMA_DISABLE_FEED
#define OW_PORT_DMA_DISABLE_FEED() ow_port_dma_disable(&OW_PORT_DMA_FEED.CCR)
#endif
#ifndef OW_PORT_DMA_PROG_CAPTURE
#define OW_PORT_DMA_PROG_CAPTURE(dst, count, cr)       \
    do {                                               \
        OW_PORT_DMA_CAPTURE.CPAR = (uint32_t)&T1.CCR4; \
        OW_PORT_DMA_CAPTURE.CMAR = (uint32_t)(dst);    \
        OW_PORT_DMA_CAPTURE.CNDTR = (count);           \
        OW_PORT_DMA_CAPTURE.CCR = (cr);                \
    } while (0)
#endif
#ifndef OW_PORT_DMA_PROG_FEED
#define OW_PORT_DMA_PROG_FEED(src, count, cr)       \
    do {                                            \
        OW_PORT_DMA_FEED.CPAR = (uint32_t)&T1.CCR3; \
        OW_PORT_DMA_FEED.CMAR = (uint32_t)(src);    \
        OW_PORT_DMA_FEED.CNDTR = (count);           \
        OW_PORT_DMA_FEED.CCR = (cr);                \
    } while (0)
#endif

/* Disable-path contract: the wait algorithm below is DMA-IP-agnostic (write
 * 0, poll the EN bit to 0, bounded) and intentionally shared; each backend
 * supplies only the operands (control register + EN bit) through its DISABLE
 * macros and OW_PORT_DMA_EN_BIT.  Duplicating the loop per backend would
 * trade this single auditable copy for six drifting ones to hide register
 * names that the PROG defaults already spell openly. */
/**
 * @brief Disable a DMA channel and wait for EN to retire before re-arm
 * @param[in] ccr Address of the channel's CCR register (`&OW_PORT_DMA_CAPTURE.CCR`
 *        or `&OW_PORT_DMA_FEED.CCR`; the F4 DISABLE macros pass their stream's
 *        CR the same way)
 * @note DMA1 clears EN by hardware only when CNDTR drains, so a capture
 *       underrun (fewer physical bus edges than armed transfers, e.g. a
 *       no-presence reset schedules OW_PORT_CAPTURE_BUF_SIZE=2 captures but
 *       only the master-release edge arrives) leaves the channel armed
 *       mid-transfer — CNDTR > 0, EN still set — when the timer operation
 *       completes. Reprogramming PAR/CMAR/NDTR while EN is set is outside the
 *       RM0008/RM0091 programming-model flow, so the disable is acknowledged
 *       with a bounded wait on CCR.EN instead of the bare CCR=0 the historical
 *       code used. On every healthy path (the previous transfer drained, EN
 *       already 0) the wait is zero iterations. The host model cannot
 *       distinguish this from the bare CCR=0 (no disable latency, no
 *       register-access accessors); those tests document the contract and the
 *       real-hardware bench proves the wait.
 *       The parameter is the CCR field rather than a channel pointer so the
 *       core need not name the channel type (the ST headers spell it
 *       `DMA_Channel_TypeDef`, the host mocks `DMA1_Channel_TypeDef`).
 */
__STATIC_FORCEINLINE void ow_port_dma_disable(volatile uint32_t* ccr) {
    *ccr = 0; /* request disable */
    if (*ccr & OW_PORT_DMA_EN_BIT) {
        uint32_t guard = 1000u;
        while ((*ccr & OW_PORT_DMA_EN_BIT) != 0u && guard-- != 0u) {
        }
    }
}

/**
 * @brief Force a timer update event, leaving UIF set
 * @note Explicit start: EGR=UG with no SR clear, so the owner (measurement
 *       state machine) sees UIF set and advances on its next poll. Nothing
 *       calls this implicitly: after init and after every finished operation
 *       the timer stays idle until the application requests the next one.
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
    (void)T1.SR; /* flush posted APB writes so UG sets UIF before SR=0 clears it
                  * (same mechanism as the F4 backend: the read round-trips the
                  * bus and proves the peripheral answered post-write) */
    T1.SR = 0; /* UIF (and any stale CCxIF) cleared: fresh op gets a clean completion flag */
}

/**
 * @brief Enable clocks, configure the timer prescaler and the open-drain bus pin
 * @pre Internal after-reset bootstrap only. This is not a reinit or recovery
 *      hook and must not be called while a TIM1/DMA operation is active.
 * @note Does not stop/reset TIM1 or the DMA channels, and does not clear stale
 *       low-power state. The timer stays stopped: no update event is forced,
 *       so the first operation only starts when the application requests it.
 */
__STATIC_FORCEINLINE void ow_port_init(void) {
    OW_PORT_ENABLE_BUS_CLOCKS();
    T1.PSC = OW_PORT_TIM_PRESCALER;
    T1.BDTR = TIM_BDTR(MOE);
    OW_PORT_CONFIG_BUS_PIN();
}

/**
 * @brief Non-blocking completion check for the scheduled operation
 * @return 1 if finished (update flag set and cleared), 0 while still running
 */
__STATIC_FORCEINLINE uint8_t ow_port_bus_done(void) {
    if (T1.SR & TIM_SR(UIF)) {
        /* No software bus release needed: every operation returns the line to
         * idle HIGH in hardware. DMA-fed writes (ow_port_feed,
         * ow_port_write_then_read) append ONEWIRE_RELEASE_PULSE to the CCR3
         * feed, and the direct-write/capture operations (reset, read, single
         * slot) use an OC3PE preload of ONEWIRE_RELEASE_PULSE — both applied
         * exactly when the one-pulse timer stops. */
#if OW_PORT_LOW_POWER
        /* The update event both interrupts the low-power WFE sleep and, via
         * SEVONPEND, raises an NVIC pending bit. UIE also latches a pending
         * bit at every ow_port_update_event() re-arm (EGR=UG). Clear the
         * pending flag here so the next __WFE() truly sleeps; otherwise the
         * pending bit would make __WFE() return immediately forever (silent
         * degradation back to a busy-loop). Clearing UIF below also retires
         * the long-stage condition read by ow_port_long_wait_pending(). */
        NVIC_ClearPendingIRQ(OW_PORT_TIM1_UPD_IRQn);
#endif
        T1.SR = 0;
        /* Retire the schedule explicitly: on hardware OPM already stopped the
         * counter, but stating it here keeps the completion state honest
         * without relying on that side effect (and keeps the host mock, which
         * does not model OPM auto-stop, in agreement with hardware). */
        T1.CR1 &= (uint32_t)~TIM_CR1(CEN);
        return 1u;
    }
    return 0u;
}

#if OW_PORT_LOW_POWER
/**
 * @brief Whether the currently scheduled operation is a "long" stage
 * @return 1 while a long stage (conversion, scratchpad read, EEPROM hold-off,
 *         multi-slot write) is armed and unfinished, 0 otherwise
 * @note Derived from the timer the port itself programs: running (CEN),
 *       unfinished (!UIF) and longer than OW_PORT_LONG_STAGE_US. There is no
 *       stored flag to keep in sync — the schedule is the state — so every
 *       arm path (capture, feed, timer wait) shares the one rule and cannot
 *       drift from it. A low-power application checks this, then calls
 *       ow_port_sleep_until_done() when it is set, instead of busy-polling.
 */
__STATIC_FORCEINLINE uint8_t ow_port_long_wait_pending(void) {
    if (!(T1.CR1 & TIM_CR1(CEN))) {
        return 0u;
    }
    if (T1.SR & TIM_SR(UIF)) {
        return 0u; /* armed but already complete: nothing left to sleep on */
    }
    return (uint8_t)(((uint32_t)T1.RCR + 1u) * (uint32_t)T1.ARR > OW_PORT_LONG_STAGE_US);
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
 *       defined, by the active-drive write path. The pin never leaves
 *       alternate-function (TIM1_CH3); only the output-stage topology changes.
 */
__STATIC_FORCEINLINE void ow_port_set_pin_mode(uint8_t push_pull) {
    OW_PORT_SET_PIN_MODE(push_pull);
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
    T1.CCMR2 = TIM_CCMR2(OC3M_0, OC3M_1, OC3M_2, CC4S_1, OW_PORT_IC4F_ARGS) | OW_PORT_OC3PE_ARGS;
    T1.CCER = TIM_CCER(CC3E, CC4E);
#if OW_PORT_LOW_POWER
    T1.DIER = TIM_DIER(CC4DE, UIE);
    /* No duration decision here: ow_port_long_wait_pending() derives it from
     * the caller's ARR/RCR, so every capture path (reset, read, long
     * scratchpad read) shares the one OW_PORT_LONG_STAGE_US rule. */
#else
    T1.DIER = TIM_DIER(CC4DE);
#endif
    ow_port_update_event();
    T1.CCR3 = ONEWIRE_RELEASE_PULSE;
    OW_PORT_DMA_DISABLE_CAPTURE();
    OW_PORT_ROUTE_CAPTURE();
    OW_PORT_DMA_PROG_CAPTURE(dst, count, ((width == 16) ? OW_PORT_DMA_CR_RX16 : OW_PORT_DMA_CR_RX8));
    T1.CR1 = TIM_CR1(OPM, CEN);
}

/**
 * @brief Transmit a command sequence of arbitrary length using DMA
 * @param[in] cmd Pointer to command sequence in pulse duration format
 * @param[in] slots Number of bit slots (bits) to transmit, 1..ONEWIRE_MAX_SLOTS.
 *                  Out-of-range values are rejected: TIM1 RCR is 8-bit
 *                  (RCR = slots - 1).
 * @return 1 if the feed was scheduled, 0 if `slots` is out of range (nothing
 *         is scheduled).
 * @note The buffer must hold `slots + 1` entries and the entry at index
 *       `slots` must be ONEWIRE_RELEASE_PULSE: the final CC2-triggered DMA
 *       transfer feeds that value into CCR3 during the last slot, so the
 *       one-pulse timer stops with the line already released to idle HIGH
 *       (hardware bus release — no software CCR3 write needed afterwards).
 */
__STATIC_FORCEINLINE uint8_t ow_port_feed(const ow_pulse_t* cmd, uint16_t slots) {
    if (slots == 0u || slots > ONEWIRE_MAX_SLOTS) {
        return 0u;
    }
    T1.RCR = slots - 1;
    T1.ARR = ONEWIRE_ONE_PULSE + ONEWIRE_ZERO_PULSE + ONEWIRE_GUARD_BAND;
    T1.CCR3 = cmd[0];
    T1.CCR2 = ONEWIRE_ONE_PULSE + ONEWIRE_ZERO_PULSE;
    T1.CCMR2 = TIM_CCMR2(OC3M_0, OC3M_1, OC3M_2);
    T1.CCER = TIM_CCER(CC3E);
#if OW_PORT_LOW_POWER
    T1.DIER = TIM_DIER(CC2DE, UIE);
#else
    T1.DIER = TIM_DIER(CC2DE);
#endif
    ow_port_update_event();
    OW_PORT_DMA_DISABLE_FEED();
    OW_PORT_ROUTE_FEED();
    /* Feed slots 2..N, then the ONEWIRE_RELEASE_PULSE (bus release) */
    OW_PORT_DMA_PROG_FEED(&cmd[1], slots, OW_PORT_DMA_CR_TX);
    T1.CR1 = TIM_CR1(OPM, CEN);
    return 1u;
}

/**
 * @brief Start a hardware-timed wait (conversion wait / EEPROM hold-off)
 * @param[in] arr Auto-reload value (one timer period in µs)
 * @param[in] rcr Repetition counter (number of periods - 1)
 */
__STATIC_FORCEINLINE void ow_port_start_timer(uint16_t arr, uint8_t rcr) {
    T1.ARR = arr;
    T1.RCR = rcr;
#if OW_PORT_LOW_POWER
    if ((uint32_t)(rcr + 1u) * (uint32_t)arr > OW_PORT_LONG_STAGE_US) {
        /* long stage: conversion / EEPROM hold-off. Enable the update
         * interrupt so the pending bit wakes __WFE() via SEVONPEND.
         * ow_port_capture() already sets UIE for the capture stages, but
         * start_timer() must too, else WFE sleeps forever. */
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
 *                   entry at index `slots` must be ONEWIRE_RELEASE_PULSE
 *                   (hardware bus release)
 * @param[in] slots Number of bit slots to transmit, 1..ONEWIRE_MAX_SLOTS.
 *                  Out-of-range values are rejected (8-bit RCR limit).
 * @return 1 if the write was scheduled, 0 if `slots` is out of range (nothing
 *         is scheduled).
 */
__STATIC_FORCEINLINE uint8_t ow_port_write_slots(const ow_pulse_t* pulses, uint16_t slots) {
    if (slots == 0u || slots > ONEWIRE_MAX_SLOTS) {
        return 0u;
    }
#if OW_DRIVE_ACTIVE
    ow_port_set_pin_mode(1); /* active-drive write: master drives both levels */
#endif
    if (slots == 1) {
        /* Single slot: no DMA needed, avoids a zero-length DMA transaction */
        T1.RCR = 0; /* Single slot, no repetition */
        T1.ARR = ONEWIRE_ONE_PULSE + ONEWIRE_ZERO_PULSE + ONEWIRE_GUARD_BAND; /* Total bit slot time */
        T1.CCR3 = pulses[0]; /* Pulse duration encodes the bit */
        /* OC3PE plus a ONEWIRE_RELEASE_PULSE preload release the bus at the
         * terminal update event, exactly when the one-pulse timer stops
         * (hardware bus release). */
        T1.CCMR2 = TIM_CCMR2(OC3M_0, OC3M_1, OC3M_2) | OW_PORT_OC3PE_ARGS;
        T1.CCER = TIM_CCER(CC3E);
#if OW_PORT_LOW_POWER
        T1.DIER = TIM_DIER(UIE); /* no DMA for a single bit slot; keep UIE for WFE */
#else
        T1.DIER = 0; /* No DMA for a single bit slot */
#endif
        ow_port_update_event();
        T1.CCR3 = ONEWIRE_RELEASE_PULSE; /* Preload release pulse -> line idles HIGH when the timer stops */
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
    T1.CCMR2 = TIM_CCMR2(OC3M_0, OC3M_1, OC3M_2, CC4S_1, OW_PORT_IC4F_ARGS) | OW_PORT_OC3PE_ARGS;
    T1.CCER = TIM_CCER(CC3E, CC4E);
#if OW_PORT_LOW_POWER
    T1.DIER = TIM_DIER(CC4DE, UIE);
#else
    T1.DIER = TIM_DIER(CC4DE);
#endif
    ow_port_update_event();
    T1.CCR3 = ONEWIRE_RELEASE_PULSE; /* Clear the output-compare value (CCR4 capture is independent) */
    OW_PORT_DMA_DISABLE_CAPTURE();
    OW_PORT_ROUTE_CAPTURE();
    OW_PORT_DMA_PROG_CAPTURE(pair_pulses, 2, OW_PORT_DMA_CR_RX16);
    T1.CR1 = TIM_CR1(OPM, CEN);
}

/* Merged-pass arm order: one order serves all families (direction pulse
 * armed first, DMA programmed after the update event).  Two per-family knobs
 * lived here briefly and were both removed after full F446 hardware matrices
 * proved them don't-care: DMA-after-UG behaves like DMA-before-UG, and the
 * early direction-pulse arm behaves like arming once before CEN (14 cells
 * each, 2026-10-06). */
/**
 * @brief Schedule a merged single-slot write followed by a two-slot read pair
 * @param[in] bit Direction bit to write in slot 1 (0 or 1)
 * @param[in] pulse3 Buffer for the three captured slots (write-slot capture,
 *                   id pulse, cmp pulse)
 * @param[in] read_pulse CCR3 reloads for read slots 2-3 (+ ONEWIRE_RELEASE_PULSE)
 */
__STATIC_FORCEINLINE void ow_port_write_then_read(uint8_t bit, volatile uint16_t* pulse3,
                                                  const ow_pulse_t* read_pulse) {
#if OW_DRIVE_ACTIVE
    ow_port_set_pin_mode(0); /* merged write+read stays open-drain so the read half is safe */
#endif
    const ow_pulse_t write_pulse = bit ? ONEWIRE_ONE_PULSE : ONEWIRE_ZERO_PULSE;
    OW_PORT_MARKER_TOGGLE(); /* opt-in LA hook (no-op by default): merged pass starts here */
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
#if OW_PORT_LOW_POWER
    T1.DIER = TIM_DIER(UIE); /* keep UIE for WFE while the DMA requests are apart */
#else
    T1.DIER = 0;
#endif
    ow_port_update_event();
    /* Capture DMA: write-slot capture plus the id/cmp pulse pair into the buffer.
     * Spelled through the PROG vocabulary (DMA1 defaults above write the same
     * CPAR/CMAR/CNDTR/CCR sequence), so a backend on different DMA IP only
     * overrides the macros, not this body. */
    OW_PORT_DMA_DISABLE_CAPTURE();
    OW_PORT_ROUTE_CAPTURE();
    OW_PORT_DMA_PROG_CAPTURE(pulse3, 3, OW_PORT_DMA_CR_RX16);
    /* Feed DMA: reload CCR3 with the read pulse for slots 2-3, then write
     * ONEWIRE_RELEASE_PULSE during slot 3 so the one-pulse timer stops with
     * the line released to idle HIGH (hardware bus release). */
    OW_PORT_DMA_DISABLE_FEED();
    OW_PORT_ROUTE_FEED();
    OW_PORT_DMA_PROG_FEED(read_pulse, 3, OW_PORT_DMA_CR_TX);
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
 * @param[in] bytes Number of bytes to read, 1..ONEWIRE_MAX_READ_BYTES.
 *                  Out-of-range values are rejected (8-bit RCR limit: 256 slots).
 * @return 1 if the read was scheduled, 0 if `bytes` is out of range (nothing
 *         is scheduled).
 */
__STATIC_FORCEINLINE uint8_t ow_port_read_data(volatile uint8_t* dst, uint8_t bytes) {
    if (bytes == 0u || bytes > ONEWIRE_MAX_READ_BYTES) {
        return 0u;
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
 *       Engaged: OTYPER switches to push-pull so the AF output stage drives
 *       the line HIGH actively, sourcing the current parasite devices need
 *       during temperature conversion and EEPROM programming windows. CCR3
 *       is zeroed (via UG, since OC3PE buffers it) so the stopped counter
 *       (CNT=0) holds the PWM mode 2 output ACTIVE=HIGH; without this CCR3
 *       keeps the last slot's pulse value, CNT=0 < CCR3 forces INACTIVE=LOW,
 *       and the bus is driven LOW instead of HIGH, starving the sensors.
 *       Released: OTYPER restores open-drain, the AF output (still ACTIVE
 *       from CCR3=0) releases the line so it floats HIGH via the external
 *       pull-up. No BSRR or MODER writes needed, and ODR is irrelevant in
 *       AF mode. The UG sets UIF, which the next operation clears at its
 *       own re-arm.
 */
__STATIC_FORCEINLINE void ow_port_strong_pullup(uint8_t on) {
    if (on) {
        T1.CCR3 = 0;
        T1.EGR = TIM_EGR(UG);
    }
    ow_port_set_pin_mode(on);
}

#endif /* OW_PORT_TIM_DMA_H */
