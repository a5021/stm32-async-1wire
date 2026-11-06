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
 *   STRESS_BURSTS number of write frames per run, default 100 (the search
 *                 mode runs one search per boot; the HIL takes level 0 as the
 *                 baseline)
 *   STRESS_WORDS  halfword transfers per memory-to-memory burst, default 512
 *   STRESS_CAPTURE 1 = run the search/capture mode instead of the write rig
 *   STRESS_INJECT_US  test-only: hold the feed (write mode) or capture (search
 *                     mode) DMA channel out of service for about this many
 *                     microseconds, then restore it. In write mode the window
 *                     is anchored at the frame's first CC2 request (the slot-1
 *                     end marker), so N measures the lateness of the first feed
 *                     transfer; in search/capture mode it starts with the reset,
 *                     because the capture channel is the one under test. This
 *                     is a channel-disable injection: the bench checks
 *                     whether the resulting delay breaks the timeline. It does
 *                     not assume a particular request-latching rule. The
 *                     library is not modified. F0/F4 clear the channel enable;
 *                     H5 suspends the GPDMA channel because clearing EN does
 *                     not drop an already pending request.
 *   STRESS_MARKER    1 = drive PA11 high for each frame (or the search pass).
 *                     The HIL wires PA11 to the logic analyzer's D1 and uses
 *                     the marker edges to delimit frames, so startup glitches
 *                     and a capture that ends mid-frame are ignored.
 *
 * The default mode is the write rig: a fixed 64-slot pattern, feed DMA only.
 * `STRESS_CAPTURE=1` switches to a Search ROM pass: the DS18B20 search reads
 * the bus through the capture DMA (channel 4), so the same load now has to
 * coexist with the capture drain as well as the feed. One search runs per
 * boot; the HIL takes the level-0 run as the baseline. SEARCH_MAX caps the
 * search at eight DS18B20s, so the count/checksum is not a full bus census
 * beyond that.
 *
 * Stress levels (the same in both modes):
 *
 *   0  none - measures the driver's own margin
 *   1  CPU hammers the SRAM array in the onewire_bus_done() poll loop
 *   2  one finite mem-to-memory burst on the first load channel every ~1 ms
 *   3  a finite mem-to-memory burst on the first load channel, restarted as
 *      soon as the previous one drains (the channel stays busy for the frame)
 *   4  a finite mem-to-memory burst on the second load channel, restarted as
 *      soon as it drains (circular mode is not allowed in memory-to-memory)
 *   5  level 4 plus the CPU hammer
 *
 * The F0, F4 and H5 backends implement the load generator: F0 uses DMA1
 * channels 2/1, F4 uses DMA2 streams 0/1 with DIR=0b10 (memory-to-memory), and
 * H5 uses GPDMA1 channels 0/1 in direct mode with a software request. Every
 * other family compiles this example and runs level 0 with `stress=unsupported`
 * in the banner, so the all-family build matrix stays green while the
 * measurement is still being designed. The load channels keep the default DMA
 * priority (on H5 that is CCR.PRIO 0; STRESS_GPDMA_PRIO selects another class);
 * STRESS_INJECT_US is a test-only channel-disable window, not a priority
 * change. In write mode it is anchored at the first CC2 request: an equivalent
 * window measured from the frame start spends its first 65 us before the feed
 * has asked for anything, which is why it could not affect the timeline below
 * that value.
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
#elif defined(OW_FAMILY_F4)
#include "stm32f4xx.h"
#elif defined(OW_FAMILY_H5)
#include "stm32h5xx.h"
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

/* Test-only isolation knobs for the level-5 CPU hammer. `STRESS_CPU_WORDS`
 * defaults to the DMA burst size; changing it needs a separate destination
 * array, which `STRESS_SPLIT_HAMMER=1` allocates. Neither affects a normal
 * build (both default to the original shared-array behaviour). */
#ifndef STRESS_CPU_WORDS
#define STRESS_CPU_WORDS STRESS_WORDS
#endif

#ifndef STRESS_SPLIT_HAMMER
#define STRESS_SPLIT_HAMMER 0
#endif

/* Test-only load-channel choice, F0 and F4 only: 0 = the historical low-number
 * load (F0 DMA1 channels 2/1, F4 DMA2 streams 0/1), which outranks the feed at
 * equal priority; 1 = the same load on the high numbers (F0 channels 5/6, F4
 * streams 5/6), where the feed and capture win. This is what separates the
 * stream/channel arbitration rank from raw bus bandwidth. */
#ifndef STRESS_LOAD_HIGH
#define STRESS_LOAD_HIGH 0
#endif

#ifndef STRESS_CAPTURE
#define STRESS_CAPTURE 0
#endif

#ifndef STRESS_INJECT_US
#define STRESS_INJECT_US 0
#endif

/* Optional logic-analyzer marker (LA D1). The pin is a build knob because the
 * bench wiring differs: F446/H503 use PA11, the F030 TSSOP20 board has PA11
 * bonded to SWD and uses PA2 instead. The HIL selects it per bench profile. */
#ifndef STRESS_MARKER
#define STRESS_MARKER 0
#endif

#ifndef STRESS_MARKER_PIN
#define STRESS_MARKER_PIN 11
#endif

/* Telemetry identity: the HIL compares this magic, the effective level/clock in
 * word 18 and the injection flags in word 19 before trusting a capture, so a
 * stale image or a reset that did not take is not analysed as a bus failure. */
#define STRESS_RUN_MAGIC 0x31574D4Bu

#define FRAME_GAP_MS 2u

#if (STRESS_LEVEL < 0) || (STRESS_LEVEL > 5)
#error "STRESS_LEVEL must be 0..5 (0 = none, 5 = ch1 burst stream + CPU hammer)"
#endif
#if (STRESS_BURSTS < 1) || (STRESS_BURSTS > 1000000)
#error "STRESS_BURSTS must be 1..1000000"
#endif
#if (STRESS_WORDS < 1) || (STRESS_WORDS > 65535)
#error "STRESS_WORDS must be 1..65535 (DMA CNDTR is 16-bit)"
#endif
#if (STRESS_CPU_WORDS < 1) || (STRESS_CPU_WORDS > 65535)
#error "STRESS_CPU_WORDS must be 1..65535"
#endif
#if (STRESS_SPLIT_HAMMER != 0) && (STRESS_SPLIT_HAMMER != 1)
#error "STRESS_SPLIT_HAMMER must be 0 or 1"
#endif
#if (STRESS_LOAD_HIGH != 0) && (STRESS_LOAD_HIGH != 1)
#error "STRESS_LOAD_HIGH must be 0 or 1"
#endif
#if STRESS_LOAD_HIGH && !defined(OW_FAMILY_F0) && !defined(OW_FAMILY_F4)
#error "STRESS_LOAD_HIGH is an F0/F4 channel/stream-selection knob"
#endif

#if !STRESS_SPLIT_HAMMER && (STRESS_CPU_WORDS != STRESS_WORDS)
#error "STRESS_CPU_WORDS != STRESS_WORDS requires STRESS_SPLIT_HAMMER=1"
#endif
#if (STRESS_CAPTURE != 0) && (STRESS_CAPTURE != 1)
#error "STRESS_CAPTURE must be 0 (write rig) or 1 (search/capture mode)"
#endif
#if (STRESS_INJECT_US < 0) || (STRESS_INJECT_US > 5000)
#error "STRESS_INJECT_US must be 0..5000 (test-only channel-disable injection)"
#endif
#if (STRESS_MARKER != 0) && (STRESS_MARKER != 1)
#error "STRESS_MARKER must be 0 or 1"
#endif
#if (STRESS_MARKER_PIN < 0) || (STRESS_MARKER_PIN > 15)
#error "STRESS_MARKER_PIN must be a PA0..PA15 number"
#endif
#if STRESS_MARKER && (STRESS_MARKER_PIN == 10 || STRESS_MARKER_PIN == 9 || STRESS_MARKER_PIN == 4)
#error "STRESS_MARKER_PIN collides with the bus (PA10), the console (PA9) or the LED (PA4)"
#endif
#if STRESS_MARKER && (STRESS_MARKER_PIN == 13 || STRESS_MARKER_PIN == 14)
#error "STRESS_MARKER_PIN collides with SWD (PA13/PA14)"
#endif
#if STRESS_MARKER && !defined(OW_FAMILY_H5) && !defined(OW_FAMILY_F4) && !defined(OW_FAMILY_F0)
#error "STRESS_MARKER is wired on the F0/F4/H5 benches only"
#endif
#if STRESS_MARKER && defined(OW_FAMILY_F0) && (STRESS_MARKER_PIN == 11)
#error "PA11 is not bonded out on the F030 TSSOP20; use PA2 (STRESS_MARKER_PIN=2)"
#endif

/* Families with a load generator. Every other family compiles the example and
 * runs level 0 with `stress=unsupported` in the banner. */
#if defined(OW_FAMILY_F0) || defined(OW_FAMILY_F4) || defined(OW_FAMILY_H5)
#define STRESS_SUPPORTED 1
#else
#define STRESS_SUPPORTED 0
#endif

#if STRESS_SUPPORTED
#define EFFECTIVE_LEVEL STRESS_LEVEL
#else
#define EFFECTIVE_LEVEL 0
#endif

/* Marker pin: push-pull output, idle low, high for one frame/search. */
#if STRESS_MARKER
#define STRESS_MARKER_MODER (3u << (STRESS_MARKER_PIN * 2u))
#define STRESS_MARKER_BIT (1u << STRESS_MARKER_PIN)
static void stress_marker_init(void) {
    GPIOA->MODER = (GPIOA->MODER & ~STRESS_MARKER_MODER) | (1u << (STRESS_MARKER_PIN * 2u));
    GPIOA->OTYPER &= ~STRESS_MARKER_BIT;
    GPIOA->BSRR = STRESS_MARKER_BIT << 16u; /* idle low */
}
static inline void stress_marker_high(void) { GPIOA->BSRR = STRESS_MARKER_BIT; }
static inline void stress_marker_low(void) { GPIOA->BSRR = STRESS_MARKER_BIT << 16u; }
#else
static void stress_marker_init(void) {}
static inline void stress_marker_high(void) {}
static inline void stress_marker_low(void) {}
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
 *    4 aborted    5 ch1_starts   6 burst_starts 7 cpu_passes
 *    8 search_runs 9 search_bad  10 rom_crc     11 search_count
 *    12 inject_us 13 capture_mode 14 sink_count 15 stage
 *    16 stop_timeout 17 run magic 18 level/clock 19 inject_flags
 *    20 start_timeout 21 load_error
 */
#define TELEMETRY_WORDS 22u
volatile uint32_t dma_pressure_telemetry[TELEMETRY_WORDS];

/* ------------------------------------------------------------------
 *  Memory-subsystem load (F0/F4)
 * ------------------------------------------------------------------ */

#if STRESS_SUPPORTED

static uint32_t ch1_starts;
static uint32_t burst_starts;
static uint32_t cpu_passes;
static uint32_t stress_inject_flags;

/* One fixed source word and a large destination array. Both transfers are
 * 16-bit, so one burst is STRESS_WORDS halfword reads plus STRESS_WORDS
 * halfword writes - roughly one bus slot of work with the default size. */
static uint16_t stress_src_word = 0xA5A5u;
static volatile uint16_t stress_dst[STRESS_WORDS];
#if STRESS_SPLIT_HAMMER
static volatile uint16_t stress_cpu_dst[STRESS_CPU_WORDS];
#define STRESS_HAMMER_ARRAY stress_cpu_dst
#else
#define STRESS_HAMMER_ARRAY stress_dst
#endif

#if defined(OW_FAMILY_F0)

#if !STRESS_LOAD_HIGH
#define STRESS_CH1_FLAGS \
    (DMA_IFCR_CGIF1 | DMA_IFCR_CTCIF1 | DMA_IFCR_CHTIF1 | DMA_IFCR_CTEIF1)
#define STRESS_CH2_FLAGS \
    (DMA_IFCR_CGIF2 | DMA_IFCR_CTCIF2 | DMA_IFCR_CHTIF2 | DMA_IFCR_CTEIF2)
#define STRESS_LOAD0_DMA DMA1_Channel2
#define STRESS_LOAD1_DMA DMA1_Channel1
#define STRESS_LOAD0_FLAGS STRESS_CH2_FLAGS
#define STRESS_LOAD1_FLAGS STRESS_CH1_FLAGS
#define STRESS_LOAD0_TEIF DMA_ISR_TEIF2
#define STRESS_LOAD1_TEIF DMA_ISR_TEIF1
#else
/* Test-only: channels 5/6 sit above the feed (3) and capture (4) at equal
 * priority, so this isolates arbitration rank from raw bus bandwidth. The
 * F030F4 has only DMA1 channels 1-5, so the second load uses channel 4 in
 * capture mode (CCR4 is not drained: the write rig runs no capture). */
#define STRESS_CH5_FLAGS \
    (DMA_IFCR_CGIF5 | DMA_IFCR_CTCIF5 | DMA_IFCR_CHTIF5 | DMA_IFCR_CTEIF5)
#define STRESS_CH4_FLAGS \
    (DMA_IFCR_CGIF4 | DMA_IFCR_CTCIF4 | DMA_IFCR_CHTIF4 | DMA_IFCR_CTEIF4)
#define STRESS_LOAD0_DMA DMA1_Channel5
#define STRESS_LOAD1_DMA DMA1_Channel4
#define STRESS_LOAD0_FLAGS STRESS_CH5_FLAGS
#define STRESS_LOAD1_FLAGS STRESS_CH4_FLAGS
#define STRESS_LOAD0_TEIF DMA_ISR_TEIF5
#define STRESS_LOAD1_TEIF DMA_ISR_TEIF4
#endif
#if STRESS_LOAD_HIGH && STRESS_CAPTURE
#error "STRESS_LOAD_HIGH uses channel 4 for the F0 load, so it cannot run in capture mode"
#endif

/* Finite mem-to-memory burst on the first load channel. At equal priority the
 * classic DMA1 arbiter favours the lower channel number, so the default
 * channel 2 (levels 2/3) and channel 1 (levels 4/5) outrank the feed on
 * channel 3; STRESS_LOAD_HIGH moves the load to channels 5/4 for the reverse
 * case (the F030F4 has no channel 6, so the second load rides channel 4, which
 * the write rig does not use for capture). */
/* Stop the channel and wait until EN reads back clear. A channel that does not
 * stop is left alone: reprogramming it would race a transfer that is still
 * running, so the caller reports the timeout instead. */
static uint8_t stress_channel_idle(DMA_Channel_TypeDef* ch) {
    ch->CCR = 0u;
    uint32_t guard = 1000u;
    while ((ch->CCR & DMA_CCR_EN) != 0u && guard-- != 0u) {
    }
    return (ch->CCR & DMA_CCR_EN) == 0u;
}

static uint8_t stress_burst_start(void) {
    burst_starts++;
    if (!stress_channel_idle(STRESS_LOAD0_DMA)) {
        return 0u;
    }
    DMA1->IFCR = STRESS_LOAD0_FLAGS;
    STRESS_LOAD0_DMA->CPAR = (uint32_t)&stress_src_word;
    STRESS_LOAD0_DMA->CMAR = (uint32_t)stress_dst;
    STRESS_LOAD0_DMA->CNDTR = (uint32_t)STRESS_WORDS;
    STRESS_LOAD0_DMA->CCR = DMA_CCR_MEM2MEM | DMA_CCR_MINC | DMA_CCR_PSIZE_0 |
                            DMA_CCR_MSIZE_0 | DMA_CCR_EN;
    return 1u;
}

/* Circular mode is not allowed with memory-to-memory (RM0091), so level 4/5
 * restart this finite burst from the service loop; that is a valid continuous
 * stream of transfers. */
static uint8_t stress_ch1_burst_start(void) {
    ch1_starts++;
    if (!stress_channel_idle(STRESS_LOAD1_DMA)) {
        return 0u;
    }
    DMA1->IFCR = STRESS_LOAD1_FLAGS;
    STRESS_LOAD1_DMA->CPAR = (uint32_t)&stress_src_word;
    STRESS_LOAD1_DMA->CMAR = (uint32_t)stress_dst;
    STRESS_LOAD1_DMA->CNDTR = (uint32_t)STRESS_WORDS;
    STRESS_LOAD1_DMA->CCR = DMA_CCR_MEM2MEM | DMA_CCR_MINC |
                            DMA_CCR_PSIZE_0 | DMA_CCR_MSIZE_0 | DMA_CCR_EN;
    return 1u;
}

static uint8_t stress_burst_done(void) { return STRESS_LOAD0_DMA->CNDTR == 0u; }
static uint8_t stress_ch1_done(void) { return STRESS_LOAD1_DMA->CNDTR == 0u; }

/* Disable one channel. Returns 1 when EN cleared within the bounded wait, 0 on
 * timeout. On timeout CNDTR is left alone: the channel is not known to be
 * stopped, and pretending otherwise would hide the failure. */
static uint8_t stress_stop_channel(DMA_Channel_TypeDef* ch, uint32_t flags) {
    if (!stress_channel_idle(ch)) {
        return 0u;
    }
    ch->CNDTR = 0u;
    DMA1->IFCR = flags;
    return 1u;
}

static uint8_t stress_stop(void) {
    uint8_t ok = stress_stop_channel(STRESS_LOAD1_DMA, STRESS_LOAD1_FLAGS);
    ok &= stress_stop_channel(STRESS_LOAD0_DMA, STRESS_LOAD0_FLAGS);
    return ok;
}

#if STRESS_CAPTURE
static volatile uint32_t* stress_capture_control(void) {
    return &DMA1_Channel4->CCR;
}
#else
static volatile uint32_t* stress_feed_control(void) {
    return &DMA1_Channel3->CCR;
}
#endif

#define STRESS_INJECT_OFF(save) 0u
#define STRESS_DMA_EN DMA_CCR_EN

static uint8_t stress_inject_off_ok(volatile uint32_t* ctrl) {
    uint32_t guard = 1000u;
    while ((*ctrl & STRESS_DMA_EN) != 0u && guard-- != 0u) {
    }
    return (*ctrl & STRESS_DMA_EN) == 0u;
}
static uint8_t stress_inject_on_ok(volatile uint32_t* ctrl, uint32_t save) {
    uint32_t guard = 1000u;
    while ((*ctrl & STRESS_DMA_EN) == 0u && guard-- != 0u) {
    }
    /* EN may auto-clear when the starved transfer finishes; the other control
     * bits still prove that the saved register value was written back. */
    return (*ctrl & ~STRESS_DMA_EN) == (save & ~STRESS_DMA_EN);
}

#if !STRESS_CAPTURE
/* Transfers still queued on the CCR3 feed channel. Zero after completion is
 * the driver's contract; anything else means the feed was starved. */
static uint32_t feed_remaining(void) { return DMA1_Channel3->CNDTR; }

static uint8_t feed_error_latched(void) {
    return (DMA1->ISR & DMA_ISR_TEIF3) ? 1u : 0u;
}
#endif

/* A load channel that latches a transfer error stops moving data while the
 * feed keeps running. Without this check the cell would be reported as clean
 * under a load that had already died. Read before any flag clear. */
static uint8_t stress_load_error(void) {
    return (DMA1->ISR & (STRESS_LOAD0_TEIF | STRESS_LOAD1_TEIF)) ? 1u : 0u;
}

#elif defined(OW_FAMILY_F4)

/* Stream 0/1 carry the two load patterns by default. Memory-to-memory is
 * DIR=0b10 on the F4 stream (there is no MEM2MEM bit, unlike the F0 channel
 * CCR). Both streams stay at the default low software priority: at equal
 * priority the F4 arbiter favours the lower stream number, so stream 0/1
 * outrank the feed on stream 2 and the capture on stream 4. That is the
 * contention this rig is meant to create; STRESS_LOAD_HIGH=1 moves the load to
 * streams 5/6 for the priority isolation experiment. */
#if !STRESS_LOAD_HIGH
#define STRESS_STREAM0_FLAGS                                   \
    (DMA_LIFCR_CFEIF0 | DMA_LIFCR_CDMEIF0 | DMA_LIFCR_CTEIF0 | \
     DMA_LIFCR_CHTIF0 | DMA_LIFCR_CTCIF0)
#define STRESS_STREAM1_FLAGS                                   \
    (DMA_LIFCR_CFEIF1 | DMA_LIFCR_CDMEIF1 | DMA_LIFCR_CTEIF1 | \
     DMA_LIFCR_CHTIF1 | DMA_LIFCR_CTCIF1)
#define STRESS_LOAD_IFCR DMA2->LIFCR
#define STRESS_LOAD0_STREAM DMA2_Stream0
#define STRESS_LOAD1_STREAM DMA2_Stream1
#define STRESS_LOAD0_FLAGS STRESS_STREAM0_FLAGS
#define STRESS_LOAD1_FLAGS STRESS_STREAM1_FLAGS
#else
/* Test-only: streams 5/6 sit above the feed (2) and capture (4) at equal
 * priority, so this isolates priority from raw bus bandwidth. */
#define STRESS_STREAM5_FLAGS                                   \
    (DMA_HIFCR_CFEIF5 | DMA_HIFCR_CDMEIF5 | DMA_HIFCR_CTEIF5 | \
     DMA_HIFCR_CHTIF5 | DMA_HIFCR_CTCIF5)
#define STRESS_STREAM6_FLAGS                                   \
    (DMA_HIFCR_CFEIF6 | DMA_HIFCR_CDMEIF6 | DMA_HIFCR_CTEIF6 | \
     DMA_HIFCR_CHTIF6 | DMA_HIFCR_CTCIF6)
#define STRESS_LOAD_IFCR DMA2->HIFCR
#define STRESS_LOAD0_STREAM DMA2_Stream5
#define STRESS_LOAD1_STREAM DMA2_Stream6
#define STRESS_LOAD0_FLAGS STRESS_STREAM5_FLAGS
#define STRESS_LOAD1_FLAGS STRESS_STREAM6_FLAGS
#endif

static uint8_t stress_stream_start(DMA_Stream_TypeDef* stream, uint32_t flags) {
    stream->CR = 0u;
    uint32_t guard = 1000u;
    while ((stream->CR & DMA_SxCR_EN) != 0u && guard-- != 0u) {
    }
    if ((stream->CR & DMA_SxCR_EN) != 0u) {
        /* The stream did not stop; arming it now would race the transfer that
         * is still in flight. Report the timeout and leave the registers be. */
        return 0u;
    }
    STRESS_LOAD_IFCR = flags;
    stream->PAR = (uint32_t)&stress_src_word;
    stream->M0AR = (uint32_t)stress_dst;
    stream->NDTR = (uint32_t)STRESS_WORDS;
    stream->CR = DMA_SxCR_DIR_1 | DMA_SxCR_MINC | DMA_SxCR_PSIZE_0 |
                 DMA_SxCR_MSIZE_0 | DMA_SxCR_EN;
    return 1u;
}

static uint8_t stress_burst_start(void) {
    burst_starts++;
    return stress_stream_start(STRESS_LOAD0_STREAM, STRESS_LOAD0_FLAGS);
}

/* Circular mode is not allowed in memory-to-memory on F4 either (RM0090), so
 * level 4/5 restart this finite burst from the service loop. The second load
 * stream is the next equal-priority contender after the first. */
static uint8_t stress_ch1_burst_start(void) {
    ch1_starts++;
    return stress_stream_start(STRESS_LOAD1_STREAM, STRESS_LOAD1_FLAGS);
}

static uint8_t stress_burst_done(void) { return STRESS_LOAD0_STREAM->NDTR == 0u; }
static uint8_t stress_ch1_done(void) { return STRESS_LOAD1_STREAM->NDTR == 0u; }

static uint8_t stress_stop_stream(DMA_Stream_TypeDef* stream, uint32_t flags) {
    stream->CR = 0u;
    uint32_t guard = 1000u;
    while ((stream->CR & DMA_SxCR_EN) != 0u && guard-- != 0u) {
    }
    if ((stream->CR & DMA_SxCR_EN) != 0u) {
        return 0u;
    }
    stream->NDTR = 0u;
    STRESS_LOAD_IFCR = flags;
    return 1u;
}

static uint8_t stress_stop(void) {
    uint8_t ok = stress_stop_stream(STRESS_LOAD1_STREAM, STRESS_LOAD1_FLAGS);
    ok &= stress_stop_stream(STRESS_LOAD0_STREAM, STRESS_LOAD0_FLAGS);
    return ok;
}

#if STRESS_CAPTURE
static volatile uint32_t* stress_capture_control(void) {
    return &DMA2_Stream4->CR;
}
#else
static volatile uint32_t* stress_feed_control(void) {
    return &DMA2_Stream2->CR;
}
#endif

#define STRESS_INJECT_OFF(save) 0u
#define STRESS_DMA_EN DMA_SxCR_EN

static uint8_t stress_inject_off_ok(volatile uint32_t* ctrl) {
    uint32_t guard = 1000u;
    while ((*ctrl & STRESS_DMA_EN) != 0u && guard-- != 0u) {
    }
    return (*ctrl & STRESS_DMA_EN) == 0u;
}
static uint8_t stress_inject_on_ok(volatile uint32_t* ctrl, uint32_t save) {
    uint32_t guard = 1000u;
    while ((*ctrl & STRESS_DMA_EN) == 0u && guard-- != 0u) {
    }
    /* EN may auto-clear when the starved transfer finishes; the other control
     * bits still prove that the saved register value was written back. */
    return (*ctrl & ~STRESS_DMA_EN) == (save & ~STRESS_DMA_EN);
}

#if !STRESS_CAPTURE
static uint32_t feed_remaining(void) { return DMA2_Stream2->NDTR; }

static uint8_t feed_error_latched(void) {
    return (DMA2->LISR & DMA_LISR_TEIF2) ? 1u : 0u;
}
#endif

/* See the F0 note: a dead load channel must fail the cell, not end the load
 * silently. Streams 0/1 report in LISR, streams 5/6 in HISR. */
#if !STRESS_LOAD_HIGH
static uint8_t stress_load_error(void) {
    return (DMA2->LISR & (DMA_LISR_TEIF0 | DMA_LISR_TEIF1)) ? 1u : 0u;
}
#else
static uint8_t stress_load_error(void) {
    return (DMA2->HISR & (DMA_HISR_TEIF5 | DMA_HISR_TEIF6)) ? 1u : 0u;
}
#endif

#elif defined(OW_FAMILY_H5)

/* GPDMA direct mode: CTR1 carries the widths and increments, CTR2 the software
 * request, CBR1.BNDT the source-byte count. Channel number does not break a
 * tie on this controller: arbitration is by CCR.PRIO, where PRIO 3 is a fixed
 * priority class and PRIO 0/1/2 share a weighted round-robin; channels of one
 * class are served round-robin. The load uses the default PRIO 0 unless
 * STRESS_GPDMA_PRIO is set, which is the equal-priority experiment; a higher
 * value is the priority experiment. BNDT counts source bytes, so a halfword
 * transfer needs twice the word count and the 16-bit field caps the burst at
 * 32767 halfwords. */
#ifndef STRESS_GPDMA_PRIO
#define STRESS_GPDMA_PRIO 0u
#endif
#if (STRESS_GPDMA_PRIO > 3)
#error "STRESS_GPDMA_PRIO must be 0..3"
#endif
#if (STRESS_WORDS < 1) || (STRESS_WORDS > 32767)
#error "GPDMA CBR1.BNDT is a 16-bit byte count: at most 32767 halfwords"
#endif

#define STRESS_GPDMA_FLAGS                                    \
    ((1u << DMA_CFCR_TCF_Pos) | (1u << DMA_CFCR_HTF_Pos) |    \
     (1u << DMA_CFCR_DTEF_Pos) | (1u << DMA_CFCR_ULEF_Pos) |  \
     (1u << DMA_CFCR_USEF_Pos) | (1u << DMA_CFCR_SUSPF_Pos) | \
     (1u << DMA_CFCR_TOF_Pos))
#define STRESS_GPDMA_CCR \
    (DMA_CCR_EN | ((uint32_t)STRESS_GPDMA_PRIO << DMA_CCR_PRIO_Pos))
#define STRESS_GPDMA_ERRORS \
    (DMA_CSR_DTEF | DMA_CSR_ULEF | DMA_CSR_USEF | DMA_CSR_TOF)

/* Take a live channel down the sequence the reference manual gives: request a
 * suspend in CCR, wait for the hardware acknowledgement in CSR.SUSPF, then
 * reset the channel. RESET self-clears when the channel is idle; only then is
 * it safe to reprogram it. Clearing EN alone does not stop a pending GPDMA
 * request. */
static uint8_t stress_gpdma_idle(DMA_Channel_TypeDef* ch) {
    if ((ch->CCR & DMA_CCR_EN) == 0u) {
        return 1u;
    }
    ch->CCR |= DMA_CCR_SUSP;
    uint32_t guard = 1000u;
    while ((ch->CSR & DMA_CSR_SUSPF) == 0u && guard-- != 0u) {
    }
    if ((ch->CSR & DMA_CSR_SUSPF) == 0u) {
        return 0u;
    }
    /* Suspension stops new requests; the channel still has to reach idle
     * before a reset is defined. */
    guard = 1000u;
    while ((ch->CSR & DMA_CSR_IDLEF) == 0u && guard-- != 0u) {
    }
    if ((ch->CSR & DMA_CSR_IDLEF) == 0u) {
        return 0u;
    }
    ch->CCR |= DMA_CCR_RESET;
    guard = 1000u;
    while ((ch->CCR & DMA_CCR_EN) != 0u && guard-- != 0u) {
    }
    if ((ch->CCR & DMA_CCR_EN) != 0u) {
        return 0u;
    }
    ch->CCR = 0u; /* drop the reset and suspend request bits */
    ch->CFCR = STRESS_GPDMA_FLAGS;
    return 1u;
}

static uint8_t stress_gpdma_start(DMA_Channel_TypeDef* ch) {
    if (!stress_gpdma_idle(ch)) {
        return 0u;
    }
    ch->CLBAR = 0u; /* direct register mode, no linked list */
    ch->CFCR = STRESS_GPDMA_FLAGS;
    ch->CSAR = (uint32_t)&stress_src_word;
    ch->CDAR = (uint32_t)stress_dst;
    ch->CBR1 = (uint32_t)STRESS_WORDS * 2u; /* BNDT is a source-byte count */
    ch->CTR2 = (1u << DMA_CTR2_SWREQ_Pos); /* software-triggered mem-to-mem */
    ch->CTR1 = (1u << DMA_CTR1_SDW_LOG2_Pos) |
               (1u << DMA_CTR1_DDW_LOG2_Pos) |
               (1u << DMA_CTR1_DINC_Pos);
    ch->CCR = STRESS_GPDMA_CCR;
    return 1u;
}

static uint8_t stress_burst_start(void) {
    burst_starts++;
    return stress_gpdma_start(GPDMA1_Channel0);
}

/* Circular mode is not useful here either; level 4/5 restart the finite burst
 * from the service loop. Channel 1 is the second load channel; with equal
 * PRIO the two share their class round-robin with the feed/capture. */
static uint8_t stress_ch1_burst_start(void) {
    ch1_starts++;
    return stress_gpdma_start(GPDMA1_Channel1);
}

static uint8_t stress_burst_done(void) {
    return (GPDMA1_Channel0->CBR1 & DMA_CBR1_BNDT_Msk) == 0u;
}
static uint8_t stress_ch1_done(void) {
    return (GPDMA1_Channel1->CBR1 & DMA_CBR1_BNDT_Msk) == 0u;
}

static uint8_t stress_stop_channel(DMA_Channel_TypeDef* ch) {
    if (!stress_gpdma_idle(ch)) {
        return 0u;
    }
    ch->CFCR = STRESS_GPDMA_FLAGS;
    return 1u;
}

static uint8_t stress_stop(void) {
    uint8_t ok = stress_stop_channel(GPDMA1_Channel1);
    ok &= stress_stop_channel(GPDMA1_Channel0);
    return ok;
}

/* See the F0 note: a load channel that stops on an error must fail the cell.
 * DTEF covers data-transfer errors; ULEF/USEF/TOF catch a malformed setting,
 * a rejected user setting and a trigger overrun. */
static uint8_t stress_load_error(void) {
    return (((GPDMA1_Channel0->CSR | GPDMA1_Channel1->CSR) &
             STRESS_GPDMA_ERRORS) != 0u)
               ? 1u
               : 0u;
}

#if STRESS_CAPTURE
#define STRESS_INJECT_CHANNEL GPDMA1_Channel3
static volatile uint32_t* stress_capture_control(void) {
    return &STRESS_INJECT_CHANNEL->CCR;
}
#else
#define STRESS_INJECT_CHANNEL GPDMA1_Channel2
static volatile uint32_t* stress_feed_control(void) {
    return &STRESS_INJECT_CHANNEL->CCR;
}
#endif

/* GPDMA does not drop a pending request when EN is cleared; suspend the
 * channel instead and keep the rest of CCR (including EN) intact. CCR.SUSP is
 * only the request: the acknowledgement is CSR.SUSPF, and the hold window
 * starts after that flag, not after the write. Clearing SUSP at the end
 * resumes the channel. SUSPF is sticky, so CFCR clears it once resumed. */
#define STRESS_INJECT_OFF(save) ((save) | DMA_CCR_SUSP)
#define STRESS_DMA_EN DMA_CCR_EN

static uint8_t stress_inject_off_ok(volatile uint32_t* ctrl) {
    (void)ctrl;
    DMA_Channel_TypeDef* ch = STRESS_INJECT_CHANNEL;
    uint32_t guard = 1000u;
    while ((ch->CSR & DMA_CSR_SUSPF) == 0u && guard-- != 0u) {
    }
    return (ch->CSR & DMA_CSR_SUSPF) != 0u;
}
static uint8_t stress_inject_on_ok(volatile uint32_t* ctrl, uint32_t save) {
    DMA_Channel_TypeDef* ch = STRESS_INJECT_CHANNEL;
    ch->CFCR = DMA_CFCR_SUSPF;
    return ((ch->CCR & DMA_CCR_SUSP) == 0u) &&
           ((*ctrl & ~STRESS_DMA_EN) == (save & ~STRESS_DMA_EN));
}

#if !STRESS_CAPTURE
/* BNDT is a source-byte count; the feed source is byte memory, so it equals
 * the number of transfers still queued. */
static uint32_t feed_remaining(void) {
    return GPDMA1_Channel2->CBR1 & DMA_CBR1_BNDT_Msk;
}

static uint8_t feed_error_latched(void) {
    return (GPDMA1_Channel2->CSR & DMA_CSR_DTEF) ? 1u : 0u;
}
#endif

#endif /* OW_FAMILY_F0 || OW_FAMILY_F4 || OW_FAMILY_H5 */

/* A volatile decrement is about four cycles; divide by four so the knob is
 * roughly microseconds at the bench clocks. The HIL sweeps the knob and
 * reports the first value that breaks the transaction, so exact duration
 * matters less than monotonicity. */
static void stress_delay_us(uint32_t us) {
    volatile uint32_t n = us * (OW_SYSCLK_MHZ / 4u);
    while (n != 0u) {
        n--;
    }
}

/* Test-only negative control: hold the feed (write mode) or capture (search
 * mode) DMA channel out of service, then restore its control register. This is
 * a channel-disable injection: the bench checks whether the resulting delay
 * breaks the timeline. It does not assume a particular request-latching rule
 * for every peripheral DMA. The library is not modified and no priority bit is
 * changed. F0/F4 write 0 to the enable bit; H5 keeps EN and sets SUSP instead
 * because a GPDMA request already latched is not dropped by clearing EN.
 *
 * Write mode anchors the window at the frame's first CC2 request, the slot-1
 * end marker at ONEWIRE_ONE_PULSE + ONEWIRE_ZERO_PULSE. The channel is disabled
 * before that event and held off for `us` after it, so the knob measures the
 * lateness of the first feed transfer. The old frame-start window spent its
 * first 65 us before the feed had asked for anything, so values below 65 us
 * could not affect the timeline at all.
 *
 * Search mode keeps the reset-anchored window: the capture channel is the one
 * under test there, and its first request belongs to the reset/presence
 * capture, not to a CC2 slot-end event. */
static void stress_inject(uint32_t us) {
    stress_inject_flags = 1u; /* attempted */
#if STRESS_CAPTURE
    volatile uint32_t* ctrl = stress_capture_control();
    const uint32_t save = *ctrl;
    if ((save & STRESS_DMA_EN) == 0u) {
        stress_inject_flags |= 8u; /* channel was not running */
    }
    *ctrl = STRESS_INJECT_OFF(save);
    if (stress_inject_off_ok(ctrl)) {
        stress_inject_flags |= 2u;
    }
    stress_delay_us(us);
    *ctrl = save;
    if (stress_inject_on_ok(ctrl, save)) {
        stress_inject_flags |= 4u;
    }
#else
    volatile uint32_t* ctrl = stress_feed_control();
    const uint32_t save = *ctrl;
    if ((save & STRESS_DMA_EN) == 0u) {
        stress_inject_flags |= 8u; /* channel was not running */
    }
    *ctrl = STRESS_INJECT_OFF(save);
    if (stress_inject_off_ok(ctrl)) {
        stress_inject_flags |= 2u;
    }
    TIM1->SR = ~TIM_SR_CC2IF; /* drop a stale slot-end flag before the wait */
    while ((TIM1->SR & TIM_SR_CC2IF) == 0u) {
    }
    stress_delay_us(us);
    *ctrl = save;
    if (stress_inject_on_ok(ctrl, save)) {
        stress_inject_flags |= 4u;
    }
#endif
}

/* CPU-side SRAM hammer: every access is a real load/store because the array
 * is volatile, and the range is wide enough that the DMA's own SRAM traffic
 * shares the same bus for the whole pass. */
static void cpu_hammer(void) {
    cpu_passes++;
    for (uint32_t i = 0; i < STRESS_CPU_WORDS; i++) {
        STRESS_HAMMER_ARRAY[i] = (uint16_t)(STRESS_HAMMER_ARRAY[i] + 1u);
    }
}

static void stress_begin(uint32_t level) {
    (void)level;
    if (STRESS_INJECT_US != 0u) {
        stress_inject((uint32_t)STRESS_INJECT_US);
    }
}

/* Returns 1 when the load is still running as asked, 0 when a channel did not
 * stop before a restart, and 2 when a load channel latched an error: the cell
 * must fail either way, because the announced load is no longer there. */
static uint8_t stress_service(uint32_t level, uint32_t* next_ms) {
    if (level >= 2u && stress_load_error()) {
        return 2u;
    }
    if (level == 2u) {
        /* One burst per millisecond; app_millis() is a SysTick read, not a
         * wait, so the loop keeps polling the bus between bursts. */
        uint32_t now = app_millis();
        if (stress_burst_done() && (int32_t)(now - *next_ms) >= 0) {
            if (!stress_burst_start()) {
                return 0u;
            }
            *next_ms = now + 1u;
        }
    } else if (level == 3u) {
        /* Back-to-back finite bursts on the first load stream. */
        if (stress_burst_done() && !stress_burst_start()) {
            return 0u;
        }
    } else if (level >= 4u) {
        /* Back-to-back finite bursts on the second load channel. Circular
         * mem-to-memory is not allowed, so the load is made from restartable
         * finite transfers. */
        if (stress_ch1_done() && !stress_ch1_burst_start()) {
            return 0u;
        }
    }
    if (level == 1u || level == 5u) {
        cpu_hammer();
    }
    return 1u;
}

static uint8_t stress_end(void) { return stress_stop(); }

#else /* !STRESS_SUPPORTED */

#if !STRESS_CAPTURE
static const uint32_t ch1_starts = 0u;
static const uint32_t burst_starts = 0u;
static const uint32_t cpu_passes = 0u;
#endif
static const uint32_t stress_inject_flags = 0u;

static void stress_begin(uint32_t level) { (void)level; }
static uint8_t stress_load_error(void) { return 0u; }
static uint8_t stress_service(uint32_t level, uint32_t* next_ms) {
    (void)level;
    (void)next_ms;
    return 1u;
}
static uint8_t stress_end(void) { return 1u; }
#if !STRESS_CAPTURE
static uint32_t feed_remaining(void) { return 0u; }
static uint8_t feed_error_latched(void) { return 0u; }
#endif

#endif /* STRESS_SUPPORTED */

/* ------------------------------------------------------------------
 *  Search/capture mode (STRESS_CAPTURE)
 * ------------------------------------------------------------------
 *  The DS18B20 Search ROM pass reads the bus through the capture DMA. The
 *  baseline ROM set is taken once without load; every stressed pass must
 *  return the same count and the same ROMs. A late capture drain corrupts a
 *  read bit, which changes the search tree, the ROM or the count.
 */

#if STRESS_CAPTURE
/* At most eight DS18B20s are counted; the checksum covers that window. */
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
#if !STRESS_SUPPORTED
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
    stress_marker_init(); // PA11/D1, idle low
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
    uint8_t stop_timeout = 0u;
    uint8_t start_timeout = 0u;
    uint8_t load_error = 0u;
    stress_marker_high();
    ds18b20_search_start(search_sink, SEARCH_MAX);
    if (!ds18b20_search_poll()) {
        stress_begin(EFFECTIVE_LEVEL);
        uint32_t next_ms = app_millis();
        while (!ds18b20_search_poll()) {
            uint8_t status = stress_service(EFFECTIVE_LEVEL, &next_ms);
            if (status != 1u) {
                if (status == 2u) {
                    load_error = 1u;
                } else {
                    start_timeout = 1u;
                }
                break;
            }
        }
    }
    if (EFFECTIVE_LEVEL >= 2u && stress_load_error()) {
        load_error = 1u;
    }
    if (!stress_end()) {
        stop_timeout = 1u;
    }
    stress_marker_low();
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
    dma_pressure_telemetry[16] = stop_timeout;
    dma_pressure_telemetry[17] = STRESS_RUN_MAGIC;
    dma_pressure_telemetry[18] = ((uint32_t)EFFECTIVE_LEVEL << 24) |
                                 ((uint32_t)OW_SYSCLK_MHZ & 0xFFFFu);
    dma_pressure_telemetry[19] = stress_inject_flags;
    dma_pressure_telemetry[20] = start_timeout;
    dma_pressure_telemetry[21] = load_error;

    uart_write_str("DONE mode=search count=");
    uart_write_int((int)found);
    uart_write_str(" rom_crc=");
    uart_write_hex((uint8_t)(search_rom_crc >> 24));
    uart_write_hex((uint8_t)(search_rom_crc >> 16));
    uart_write_hex((uint8_t)(search_rom_crc >> 8));
    uart_write_hex((uint8_t)search_rom_crc);
    uart_write_str(" inject=");
    uart_write_int((int)STRESS_INJECT_US);
    uart_write_str(" stop_timeout=");
    uart_write_int((int)stop_timeout);
    uart_write_str(" start_timeout=");
    uart_write_int((int)start_timeout);
    uart_write_str(" load_error=");
    uart_write_int((int)load_error);
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
    uint8_t stop_timeout = 0u;
    uint8_t start_timeout = 0u;
    uint8_t load_error = 0u;

    while (frames < (uint32_t)STRESS_BURSTS) {
        stress_marker_high();
        if (!onewire_write_slots(frame, PATTERN_SLOTS)) {
            stress_marker_low();
            break;
        }
        stress_begin(EFFECTIVE_LEVEL);

        /* Poll, do not wait: this is the application slot the load lives in.
         * The feed itself is hardware-timed and does not need the CPU. */
        uint32_t next_ms = app_millis();
        while (!onewire_bus_done()) {
            uint8_t status = stress_service(EFFECTIVE_LEVEL, &next_ms);
            if (status != 1u) {
                if (status == 2u) {
                    load_error = 1u;
                } else {
                    start_timeout = 1u;
                }
                break;
            }
        }
        if (EFFECTIVE_LEVEL >= 2u && stress_load_error()) {
            load_error = 1u;
        }
        uint8_t stop_ok = stress_end();
        stress_marker_low();
        if (!stop_ok) {
            stop_timeout = 1u;
            aborted = 1u;
            break;
        }
        if (start_timeout || load_error) {
            aborted = 1u;
            break;
        }

        frames++;
        if (feed_error_latched()) {
            dma_error = 1u;
            aborted = 1u;
            break;
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
    dma_pressure_telemetry[5] = ch1_starts;
    dma_pressure_telemetry[6] = burst_starts;
    dma_pressure_telemetry[7] = cpu_passes;
    dma_pressure_telemetry[12] = STRESS_INJECT_US;
    dma_pressure_telemetry[13] = 0u;
    dma_pressure_telemetry[16] = stop_timeout;
    dma_pressure_telemetry[17] = STRESS_RUN_MAGIC;
    dma_pressure_telemetry[18] = ((uint32_t)EFFECTIVE_LEVEL << 24) |
                                 ((uint32_t)OW_SYSCLK_MHZ & 0xFFFFu);
    dma_pressure_telemetry[19] = stress_inject_flags;
    dma_pressure_telemetry[20] = start_timeout;
    dma_pressure_telemetry[21] = load_error;

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
    uart_write_str(" stop_timeout=");
    uart_write_int((int)stop_timeout);
    uart_write_str(" start_timeout=");
    uart_write_int((int)start_timeout);
    uart_write_str(" load_error=");
    uart_write_int((int)load_error);
    uart_write_str("\r\n");
    uart_flush();

    for (;;) {
        uart_poll_tx();
    }
#endif
}
