/**
 * @file main.c
 * @brief DMA feed pressure rig: measure the memory-subsystem headroom the
 *        CCR3 feed channel needs while the bus is running
 *
 * This is a diagnostic example, not a demo. It transmits a known 64-slot
 * pattern (8 bytes, then the trailing hardware-release entry) back to back
 * while a selectable memory-subsystem load runs next to the driver, and
 * reports the two things that can be observed without a logic analyzer:
 *
 *   - `cndtr_late`: the CCR3 feed channel still had transfers pending when
 *     the bus reported completion. That is complete starvation: at least one
 *     slot was never fed, so the pulse for it repeated and the final release
 *     may be missing.
 *   - `dma_error`: the feed channel latched a transfer error.
 *
 * A stretched pulse or a duplicated edge - the earlier, subtler failure -
 * does not move either counter. That is what a logic analyzer captures; see
 * `tests/hil/dma_pressure.py`, which bisects ONEWIRE_GUARD_BAND per stress
 * level and takes the smallest guard with a clean per-slot geometry as an
 * upper bound on the feed latency (guard 1 is the timer's measurement floor).
 *
 * Build knobs (all overridable from `make ... EXT="-D...=..."`):
 *
 *   STRESS_LEVEL  0..5, default 0 (see the table below)
 *   STRESS_BURSTS number of frames/search passes per run, default 100
 *   STRESS_WORDS  halfword transfers per memory-to-memory burst, default 512
 *   STRESS_CAPTURE 1 = run the search/capture mode instead of the write rig
 *   STRESS_INJECT_US  test-only: hold the feed (write mode) or capture (search
 *                     mode) DMA channel disabled for about this many
 *                     microseconds at the start of the pass, then restore it.
 *                     The pending request is serviced late - the negative
 *                     control for "does the harness catch a deliberately
 *                     delayed transfer?". The library is not modified.
 *
 * The default mode is the write rig: a fixed 64-slot pattern, feed DMA only.
 * `STRESS_CAPTURE=1` switches to a Search ROM pass: the DS18B20 search reads
 * the bus through the capture DMA (channel 4), so the same load now has to
 * coexist with the capture drain as well as the feed. The baseline ROM set is
 * taken once without load; every stressed pass must return the same count and
 * the same ROMs, otherwise the capture drain lost a deadline.
 *
 * Stress levels (the same in both modes):
 *
 *   0  none - measures the driver's own margin
 *   1  CPU hammers the SRAM array in the onewire_bus_done() poll loop
 *   2  one finite mem-to-memory burst on DMA1 channel 2 about every 1 ms
 *   3  a finite mem-to-memory burst on channel 2, restarted as soon as the
 *      previous one drains (the channel stays busy for the whole frame)
 *   4  a continuous circular mem-to-memory transfer on DMA1 channel 1
 *   5  level 4 plus the CPU hammer
 *
 * Only the STM32F0 backend implements the load generator; every other family
 * compiles this example and runs level 0 with `stress=unsupported` in the
 * banner, so the all-family build matrix stays green while the measurement
 * is still being designed. The load channels keep the default DMA priority;
 * STRESS_INJECT_US is a test-only channel-disable window, not a priority
 * change.
 *
 * The UART is silent while the frames run. The banner before the run and the
 * `DONE` line after it are the only output, so the console cannot become a
 * load of its own (the F0 console is polled and each byte costs about
 * 8.7 us at 115200 baud).
 *
 * A starvation event ends the sweep: after a missed feed the line may be
 * parked low, and a frame that starts from there measures a corrupt initial
 * state rather than the feed latency.
 */

#include "app.h"

#if defined(OW_FAMILY_F0)
#include "stm32f0xx.h"
#endif

#include <stdint.h>

/* ------------------------------------------------------------------
 *  Build-time knobs
 * ------------------------------------------------------------------ */

#ifndef STRESS_LEVEL
#define STRESS_LEVEL 0
#endif

#ifndef STRESS_BURSTS
#define STRESS_BURSTS 100
#endif

#ifndef STRESS_WORDS
#define STRESS_WORDS 512
#endif

#ifndef STRESS_CAPTURE
#define STRESS_CAPTURE 0
#endif

#ifndef STRESS_INJECT_US
#define STRESS_INJECT_US 0
#endif

#define FRAME_GAP_MS 2u

#if (STRESS_LEVEL < 0) || (STRESS_LEVEL > 5)
#error "STRESS_LEVEL must be 0..5 (0 = none, 5 = circular DMA hog + CPU hammer)"
#endif
#if (STRESS_BURSTS < 1) || (STRESS_BURSTS > 1000000)
#error "STRESS_BURSTS must be 1..1000000"
#endif
#if (STRESS_WORDS < 1) || (STRESS_WORDS > 65535)
#error "STRESS_WORDS must be 1..65535 (DMA CNDTR is 16-bit)"
#endif
#if (STRESS_CAPTURE != 0) && (STRESS_CAPTURE != 1)
#error "STRESS_CAPTURE must be 0 (write rig) or 1 (search/capture mode)"
#endif
#if (STRESS_INJECT_US < 0) || (STRESS_INJECT_US > 5000)
#error "STRESS_INJECT_US must be 0..5000 (test-only high-priority hog)"
#endif

/* Non-F0 families compile the whole example but cannot generate the load. */
#if defined(OW_FAMILY_F0)
#define EFFECTIVE_LEVEL STRESS_LEVEL
#else
#define EFFECTIVE_LEVEL 0
#endif

/* ------------------------------------------------------------------
 *  Pattern (write mode only)
 * ------------------------------------------------------------------ */

#if !STRESS_CAPTURE
#define PATTERN_BYTES 8u
#define PATTERN_SLOTS (PATTERN_BYTES * ONEWIRE_BITS_PER_BYTE)

/* Mixed run lengths of ones and zeros, so a repeated slot (the signature of
 * a late feed) changes the decoded pattern in more than one place. */
static const uint8_t pattern[PATTERN_BYTES] = {0x00u, 0xFFu, 0xAAu, 0x55u,
                                               0x0Fu, 0xF0u, 0x3Cu, 0xC3u};

/* Transmit buffer: PATTERN_SLOTS entries plus the trailing release entry
 * that must equal ONEWIRE_RELEASE_PULSE. */
static ow_pulse_t frame[PATTERN_SLOTS + 1u];
#endif

/* ------------------------------------------------------------------
 *  Telemetry
 * ------------------------------------------------------------------
 *  The same counters the `DONE` line prints, mirrored in RAM so a bench
 *  without a console can read them with a debugger after the sweep (for
 *  example `STM32_Programmer_CLI -c port=SWD -r32 <addr> 64`, with the
 *  address from `arm-none-eabi-nm`). Index:
 *
 *    0 frames     1 cndtr_late   2 cndtr_max   3 dma_error
 *    4 aborted    5 hog_starts   6 burst_starts 7 cpu_passes
 *    8 search_runs 9 search_bad  10 baseline  11 search_last
 *    12 inject_us 13 capture_mode 14 no_device 15 stage
 */
#define TELEMETRY_WORDS 16u
volatile uint32_t dma_pressure_telemetry[TELEMETRY_WORDS];

/* ------------------------------------------------------------------
 *  Memory-subsystem load (F0 only)
 * ------------------------------------------------------------------ */

#if defined(OW_FAMILY_F0)

static uint32_t hog_starts;
static uint32_t burst_starts;
static uint32_t cpu_passes;

/* One fixed source word (peripheral increment off) and a large destination
 * array (memory increment on). Both transfers are 16-bit, so one burst is
 * STRESS_WORDS halfword reads plus STRESS_WORDS halfword writes - roughly
 * one bus slot of work at 48 MHz with the default size. */
static uint16_t stress_src_word = 0xA5A5u;
static volatile uint16_t stress_dst[STRESS_WORDS];

/* Finite mem-to-memory burst on DMA1 channel 2 (the second-lowest channel
 * number; at equal priority the classic DMA1 arbiter favours lower
 * channels). */
static void stress_burst_start(void) {
    burst_starts++;
    DMA1_Channel2->CCR = 0u;
    DMA1->IFCR = DMA_IFCR_CGIF2 | DMA_IFCR_CTCIF2 | DMA_IFCR_CHTIF2 | DMA_IFCR_CTEIF2;
    DMA1_Channel2->CPAR = (uint32_t)&stress_src_word;
    DMA1_Channel2->CMAR = (uint32_t)stress_dst;
    DMA1_Channel2->CNDTR = (uint32_t)STRESS_WORDS;
    DMA1_Channel2->CCR = DMA_CCR_MEM2MEM | DMA_CCR_MINC | DMA_CCR_PSIZE_0 |
                         DMA_CCR_MSIZE_0 | DMA_CCR_EN;
}

/* Continuous circular mem-to-memory transfer on DMA1 channel 1. It never
 * drains by itself: only stress_stop() ends it. */
static void stress_hog_start(void) {
    hog_starts++;
    DMA1_Channel1->CCR = 0u;
    DMA1->IFCR = DMA_IFCR_CGIF1 | DMA_IFCR_CTCIF1 | DMA_IFCR_CHTIF1 | DMA_IFCR_CTEIF1;
    DMA1_Channel1->CPAR = (uint32_t)&stress_src_word;
    DMA1_Channel1->CMAR = (uint32_t)stress_dst;
    DMA1_Channel1->CNDTR = (uint32_t)STRESS_WORDS;
    DMA1_Channel1->CCR = DMA_CCR_MEM2MEM | DMA_CCR_CIRC | DMA_CCR_MINC |
                         DMA_CCR_PSIZE_0 | DMA_CCR_MSIZE_0 | DMA_CCR_EN;
}

/* Disable one channel and leave it ready for the next frame: write CNDTR back
 * to zero as well, so a burst cut mid-transfer cannot make the next frame's
 * `CNDTR == 0` poll false forever. The EN wait mirrors the port's own bounded
 * disable: a mem-to-memory channel clears EN after the in-flight transfer. */
static void stress_stop_channel(DMA_Channel_TypeDef* ch, uint32_t flags) {
    ch->CCR = 0u;
    uint32_t guard = 1000u;
    while ((ch->CCR & DMA_CCR_EN) != 0u && guard-- != 0u) {
    }
    ch->CNDTR = 0u;
    DMA1->IFCR = flags;
}

static void stress_stop(void) {
    stress_stop_channel(DMA1_Channel1, DMA_IFCR_CGIF1 | DMA_IFCR_CTCIF1 |
                                           DMA_IFCR_CHTIF1 | DMA_IFCR_CTEIF1);
    stress_stop_channel(DMA1_Channel2, DMA_IFCR_CGIF2 | DMA_IFCR_CTCIF2 |
                                           DMA_IFCR_CHTIF2 | DMA_IFCR_CTEIF2);
}

/* Test-only negative control: hold the feed (write mode) or capture (search
 * mode) DMA channel disabled for about `us` microseconds, then restore its
 * control register. Any request raised while the channel is off stays pending,
 * so the transfer is genuinely late - not a wrong count or a wrong route.
 * The library is not modified, and no channel priority is changed: this is a
 * deliberate software delay in the test image. */
static void stress_inject(uint32_t us) {
#if STRESS_CAPTURE
    volatile uint32_t* ccr = &DMA1_Channel4->CCR;
#else
    volatile uint32_t* ccr = &DMA1_Channel3->CCR;
#endif
    const uint32_t save = *ccr;
    *ccr = 0u;
    /* A volatile decrement is about four cycles; divide by four so the knob is
     * roughly microseconds at the 48/8 MHz clocks. The HIL sweeps the knob and
     * reports the first value that breaks the transaction, so exact duration
     * matters less than monotonicity. */
    volatile uint32_t n = us * (OW_SYSCLK_MHZ / 4u);
    while (n != 0u) {
        n--;
    }
    *ccr = save;
}

/* CPU-side SRAM hammer: every access is a real load/store because the array
 * is volatile, and the range is wide enough that the DMA's own SRAM traffic
 * shares the same bus for the whole pass. */
static void cpu_hammer(void) {
    cpu_passes++;
    for (uint32_t i = 0; i < STRESS_WORDS; i++) {
        stress_dst[i] = (uint16_t)(stress_dst[i] + 1u);
    }
}

static void stress_begin(uint32_t level) {
    if (STRESS_INJECT_US != 0u) {
        stress_inject((uint32_t)STRESS_INJECT_US);
    }
    if (level >= 4u) {
        stress_hog_start();
    }
}

static void stress_service(uint32_t level, uint32_t* next_ms) {
    if (level == 2u) {
        /* One burst per millisecond; app_millis() is a SysTick read, not a
         * wait, so the loop keeps polling the bus between bursts. */
        uint32_t now = app_millis();
        if (DMA1_Channel2->CNDTR == 0u && (int32_t)(now - *next_ms) >= 0) {
            stress_burst_start();
            *next_ms = now + 1u;
        }
    } else if (level == 3u) {
        /* Back-to-back finite bursts: channel 2 is busy whenever the CPU
         * can re-arm it, so it competes with the feed for nearly every slot. */
        if (DMA1_Channel2->CNDTR == 0u) {
            stress_burst_start();
        }
    }
    if (level == 1u || level == 5u) {
        cpu_hammer();
    }
}

static void stress_end(void) { stress_stop(); }

#if !STRESS_CAPTURE
/* Transfers still queued on the CCR3 feed channel. Zero after completion is
 * the driver's contract; anything else means the feed was starved. */
static uint32_t feed_remaining(void) { return DMA1_Channel3->CNDTR; }

static uint8_t feed_error_latched(void) {
    return (DMA1->ISR & DMA_ISR_TEIF3) ? 1u : 0u;
}
#endif

#else /* !OW_FAMILY_F0 */

#if !STRESS_CAPTURE
static const uint32_t hog_starts = 0u;
static const uint32_t burst_starts = 0u;
static const uint32_t cpu_passes = 0u;
#endif

static void stress_begin(uint32_t level) { (void)level; }
static void stress_service(uint32_t level, uint32_t* next_ms) {
    (void)level;
    (void)next_ms;
}
static void stress_end(void) {}
#if !STRESS_CAPTURE
static uint32_t feed_remaining(void) { return 0u; }
static uint8_t feed_error_latched(void) { return 0u; }
#endif

#endif /* OW_FAMILY_F0 */

/* ------------------------------------------------------------------
 *  Search/capture mode (STRESS_CAPTURE)
 * ------------------------------------------------------------------
 *  The DS18B20 Search ROM pass reads the bus through the capture DMA. The
 *  baseline ROM set is taken once without load; every stressed pass must
 *  return the same count and the same ROMs. A late capture drain corrupts a
 *  read bit, which changes the search tree, the ROM or the count.
 */

#if STRESS_CAPTURE
#define SEARCH_MAX 8u

static uint32_t search_count;
static uint32_t search_rom_crc;

/* FNV-1a over the found ROMs, in search order. The HIL takes the level-0
 * (unloaded) value as the baseline and requires every stressed cell to match
 * both the count and this checksum. The search engine already rejects a ROM
 * whose CRC-8 is wrong; the checksum catches a changed-but-valid tree. */
static uint8_t search_sink(const uint8_t* rom) {
    for (uint8_t i = 0; i < DS18B20_ROM_BYTES; i++) {
        search_rom_crc ^= rom[i];
        search_rom_crc *= 16777619u;
    }
    search_count++;
    return 0u;
}
#endif /* STRESS_CAPTURE */

/* ------------------------------------------------------------------
 *  Reporting
 * ------------------------------------------------------------------ */

static void print_banner(void) {
    uart_write_str("# hil: app=dma_pressure mode=");
#if STRESS_CAPTURE
    uart_write_str("search");
#else
    uart_write_str("write");
#endif
    uart_write_str(" level=");
    uart_write_int((int)EFFECTIVE_LEVEL);
#if !defined(OW_FAMILY_F0)
    uart_write_str(" requested=");
    uart_write_int((int)STRESS_LEVEL);
    uart_write_str(" stress=unsupported");
#endif
    uart_write_str(" guard=");
    uart_write_int((int)ONEWIRE_GUARD_BAND);
    uart_write_str(" clock=");
    uart_write_int((int)OW_SYSCLK_MHZ);
#if !STRESS_CAPTURE
    uart_write_str(" bursts=");
    uart_write_int((int)STRESS_BURSTS);
#endif
    uart_write_str(" words=");
    uart_write_int((int)STRESS_WORDS);
#if !STRESS_CAPTURE
    uart_write_str(" slots=");
    uart_write_int((int)PATTERN_SLOTS);
    uart_write_str(" pattern=");
    for (uint8_t i = 0; i < PATTERN_BYTES; i++) {
        uart_write_hex(pattern[i]);
    }
#endif
    if (STRESS_INJECT_US != 0u) {
        uart_write_str(" inject=");
        uart_write_int((int)STRESS_INJECT_US);
    }
    uart_write_str("\r\n");
}

#if !STRESS_CAPTURE
/* A deterministic idle gap between passes: wait for FRAME_GAP_MS observed
 * millisecond ticks, not a wall-clock deadline, so the logic analyzer gets a
 * boundary to split two passes on. */
static void idle_gap(void) {
    uint32_t seen = app_millis();
    uint32_t waited = 0u;
    while (waited < FRAME_GAP_MS) {
        uint32_t now = app_millis();
        if (now != seen) {
            waited += now - seen;
            seen = now;
        }
        uart_poll_tx();
    }
}
#endif

/* ------------------------------------------------------------------
 *  Main
 * ------------------------------------------------------------------ */

int main(void) {
    app_init(); // clock, UART and LED GPIO
#if STRESS_CAPTURE
    ds18b20_init(); // also calls onewire_init(); the search context needs it
#if OW_PARASITE_POWER
    ds18b20_set_parasite(1); // the bench bus is parasite-powered
#endif
#else
    onewire_init(); // TIM1, DMA1 and the bus pin
#endif

#if !STRESS_CAPTURE
    for (uint8_t i = 0; i < PATTERN_BYTES; i++) {
        onewire_encode_byte(&frame[i * ONEWIRE_BITS_PER_BYTE], pattern[i]);
    }
    frame[PATTERN_SLOTS] = ONEWIRE_RELEASE_PULSE;
#endif

    print_banner();
    uart_flush(); // the UART stays silent for the whole measurement

#if STRESS_CAPTURE
    /* One Search ROM pass under the load. The HIL takes the level-0 (unloaded)
     * count/checksum as the baseline and requires the stressed cells to match
     * it; a late capture drain changes a read bit, the tree or the ROMs. */
    search_count = 0u;
    search_rom_crc = 2166136261u;
    ds18b20_search_start(search_sink, SEARCH_MAX);
    if (!ds18b20_search_poll()) {
        stress_begin(EFFECTIVE_LEVEL);
        uint32_t next_ms = app_millis();
        while (!ds18b20_search_poll()) {
            stress_service(EFFECTIVE_LEVEL, &next_ms);
        }
        stress_end();
    }
    uint32_t found = ds18b20_search_count();
    for (uint8_t i = 0; i < TELEMETRY_WORDS; i++) {
        dma_pressure_telemetry[i] = 0u;
    }
    dma_pressure_telemetry[8] = 1u;
    dma_pressure_telemetry[10] = search_rom_crc;
    dma_pressure_telemetry[11] = found;
    dma_pressure_telemetry[12] = STRESS_INJECT_US;
    dma_pressure_telemetry[13] = 1u;
    dma_pressure_telemetry[14] = search_count;
    dma_pressure_telemetry[15] = 5u;

    uart_write_str("DONE mode=search count=");
    uart_write_int((int)found);
    uart_write_str(" rom_crc=");
    uart_write_hex((uint8_t)(search_rom_crc >> 24));
    uart_write_hex((uint8_t)(search_rom_crc >> 16));
    uart_write_hex((uint8_t)(search_rom_crc >> 8));
    uart_write_hex((uint8_t)search_rom_crc);
    uart_write_str(" inject=");
    uart_write_int((int)STRESS_INJECT_US);
    uart_write_str("\r\n");
    uart_flush();
    for (;;) {
        uart_poll_tx();
    }
#else
    uint32_t frames = 0u;
    uint32_t cndtr_late = 0u;
    uint32_t cndtr_max = 0u;
    uint8_t aborted = 0u;
    uint8_t dma_error = 0u;

    while (frames < (uint32_t)STRESS_BURSTS) {
        if (!onewire_write_slots(frame, PATTERN_SLOTS)) {
            break;
        }
        stress_begin(EFFECTIVE_LEVEL);

        /* Poll, do not wait: this is the application slot the load lives in.
         * The feed itself is hardware-timed and does not need the CPU. */
        uint32_t next_ms = app_millis();
        while (!onewire_bus_done()) {
            stress_service(EFFECTIVE_LEVEL, &next_ms);
        }
        stress_end();

        frames++;
        if (feed_error_latched()) {
            dma_error = 1u;
        }
        uint32_t remaining = feed_remaining();
        if (remaining != 0u) {
            cndtr_late++;
            if (remaining > cndtr_max) {
                cndtr_max = remaining;
            }
            /* The line may be parked low after a missing release, so a
             * further frame would not start from idle HIGH. Stop here. */
            aborted = 1u;
            break;
        }

        idle_gap();
    }

    for (uint8_t i = 0; i < TELEMETRY_WORDS; i++) {
        dma_pressure_telemetry[i] = 0u;
    }
    dma_pressure_telemetry[0] = frames;
    dma_pressure_telemetry[1] = cndtr_late;
    dma_pressure_telemetry[2] = cndtr_max;
    dma_pressure_telemetry[3] = dma_error;
    dma_pressure_telemetry[4] = aborted;
    dma_pressure_telemetry[5] = hog_starts;
    dma_pressure_telemetry[6] = burst_starts;
    dma_pressure_telemetry[7] = cpu_passes;
    dma_pressure_telemetry[12] = STRESS_INJECT_US;
    dma_pressure_telemetry[13] = 0u;

    uart_write_str("DONE frames=");
    uart_write_int((int)frames);
    uart_write_str(" cndtr_late=");
    uart_write_int((int)cndtr_late);
    uart_write_str(" cndtr_max=");
    uart_write_int((int)cndtr_max);
    uart_write_str(" dma_error=");
    uart_write_int((int)dma_error);
    uart_write_str(" aborted=");
    uart_write_int((int)aborted);
    uart_write_str(" inject=");
    uart_write_int((int)STRESS_INJECT_US);
    uart_write_str("\r\n");
    uart_flush();

    for (;;) {
        uart_poll_tx();
    }
#endif
}
