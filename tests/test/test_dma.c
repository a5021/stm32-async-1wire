/* ============================================================
 *  test_dma.c - DMA buffer / transfer-contract tests
 *
 *  Proves, against the behavioural DMA model (hw_model.c) and the
 *  STM32 register set, the memory-side contract of every DMA
 *  operation the driver schedules:
 *
 *    TX (memory -> peripheral, DMA1 ch3 / CCR3 feed):
 *      - CMAR points at the exact pulse buffer the caller supplied
 *        (the driver feeds from cmd[1], slot 1 preloaded direct);
 *      - values are read from that buffer, in order, one element per
 *        transfer (8-bit MSIZE), and never from a neighbouring buffer;
 *      - addresses advance by exactly MSIZE;
 *      - guard bytes around the buffer are never read;
 *      - CNDTR fully drains: 0 transfers left, CCR.EN cleared;
 *      - two distinct misconfigurations are *observable*:
 *          . CNDTR > slot budget  -> channel left armed afterwards;
 *          . CNDTR and RCR both +1 -> the extra transfer reads the
 *            guard byte instead of being silently masked.
 *
 *    RX (peripheral -> memory, DMA1 ch4 / CCR4 capture):
 *      - CMAR points at the exact destination buffer;
 *      - values land at the correct addresses, stepping by MSIZE
 *        (16-bit MINC for captures, 8-bit for byte reads);
 *      - only the permitted range of the buffer is modified; guard
 *        bytes around it are never written;
 *      - an over-programmed CNDTR walks into the guard zone instead
 *        of being truncated by the model.
 *
 *  Plus the cross-cutting invariants:
 *      - DIR reflects the memory<->peripheral direction and the
 *        model moves data in that direction;
 *      - the consistent chain RCR -> slots -> transfers -> buffer size
 *        holds, including operations where slots != transfers:
 *        reset (2 captures in 1 slot), Search ROM merged ops (3 slots,
 *        3x8-bit feed + 3x16-bit captures), Match-ROM config write
 *        (104 slots for 13 bytes);
 *      - MSIZE really changes address stepping, not just a register;
 *      - single-slot writes use no DMA at all (a deliberate contract,
 *        not a zero-length DMA transaction).
 *
 *  All operations run on the host against the mock: no HAL/LL, no
 *  real STM32 required.
 * ============================================================ */

#include "ds18b20.h"
#include "ds18b20_test_access.h"
#include "hw_model.h"
#include "mock_target.h"
#include "onewire.h"
#include "ow_port.h"
#include "unity.h"
#include <string.h>

#define ONE ow_one_pulse_us
#define ZERO ow_zero_pulse_us

#define DMA_GUARD 8u /* guard bytes on each side of a buffer under test */
#define RX_BYTES_9 9u /* DS18B20_SCRATCHPAD_LEN (private to src/ds18b20.c) */
#define RX_BITS_72 72u /* RX_BYTES_9 * 8 */

/* ------------------------- guard-zone helpers ------------------------- */

static void fill_guards(uint8_t* before, uint8_t* after, uint8_t v) {
    for (uint8_t i = 0; i < DMA_GUARD; i++) {
        before[i] = v;
        after[i] = v;
    }
}

static void assert_guards(const uint8_t* before, const uint8_t* after, uint8_t v) {
    for (uint8_t i = 0; i < DMA_GUARD; i++) {
        TEST_ASSERT_EQUAL_UINT8(v, before[i]);
        TEST_ASSERT_EQUAL_UINT8(v, after[i]);
    }
}

/* ------------------------- buffer layouts ------------------------- */

typedef struct {
    uint8_t guard_before[DMA_GUARD];
    uint8_t pulses[ONEWIRE_BITS_PER_BYTE + 1]; /* slots + trailing bus release 0 */
    uint8_t guard_after[DMA_GUARD];
} dma_tx_t;

typedef struct {
    uint8_t guard_before[DMA_GUARD];
    uint16_t edge[4]; /* capacity beyond the 2-3 transfers actually used */
    uint8_t guard_after[DMA_GUARD];
} dma_rx16_t;

typedef struct {
    uint8_t guard_before[DMA_GUARD];
    uint8_t buf[16];
    uint8_t guard_after[DMA_GUARD];
} dma_rx8_t;

typedef struct {
    uint8_t guard_before[DMA_GUARD];
    uint8_t buf[72]; /* a full 9-byte scratchpad read = 72 pulse slots */
    uint8_t guard_after[DMA_GUARD];
} dma_rx72_t;

/* ------------------------- capture sources ------------------------- */

static uint16_t two_val16_src(uint32_t idx) {
    return idx == 0 ? 0x1234u : 0x5678u;
}

static uint16_t two_val8_src(uint32_t idx) {
    /* distinct bytes whose LSBs prove the element step (0x06 -> 0x08) */
    return idx == 0 ? 0x0506u : 0x0708u;
}

static uint16_t byte_seq_src(uint32_t idx) {
    return (uint16_t)((idx & 0x7Fu) + 1u); /* 1..72 */
}

static uint16_t presence_src(uint32_t idx) {
    return idx == 0 ? 510u : 700u;
}

static uint16_t one_val_src(uint32_t idx) {
    (void)idx;
    return 0x0777u;
}

static uint16_t merged_src(uint32_t idx) {
    return (uint16_t)(0x1000u * (idx + 1u)); /* 0x1000, 0x2000, 0x3000 */
}

/* ------------------------- single-op runner -------------------------
 * Drives exactly one scheduled hardware operation to its terminal update
 * event, failing the test if the timer was not running or the op did not
 * complete within its own slot count. */
static void run_op(void) {
    uint32_t slots = (uint32_t)(mock_tim1.RCR & 0xFFu) + 1u;
    TEST_ASSERT_TRUE(mock_tim1.CR1 & TIM_CR1_CEN);
    TEST_ASSERT_TRUE(hw_run_until_uif(slots));
}

/* ============================================================
 *  1. TX: exact buffer, order, MINC, guards, CNDTR exhaustion
 * ============================================================ */

void test_dma_tx_reads_exact_buffer_in_order(void) {
    static dma_tx_t a, b;
    static const uint8_t pat_a[ONEWIRE_BITS_PER_BYTE + 1] =
        {0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88, 0x00};
    static const uint8_t pat_b[ONEWIRE_BITS_PER_BYTE + 1] =
        {0x21, 0x22, 0x23, 0x24, 0x25, 0x26, 0x27, 0x28, 0x00};
    fill_guards(a.guard_before, a.guard_after, 0xA5);
    fill_guards(b.guard_before, b.guard_after, 0xA5);
    memcpy(a.pulses, pat_a, sizeof(pat_a));
    memcpy(b.pulses, pat_b, sizeof(pat_b));
    hw_register_buf(&a.pulses[1]); /* the driver feeds from cmd[1]... */
    hw_register_buf(&b.pulses[1]);

    /* first buffer: CMAR must address exactly its slot-1 element */
    onewire_write_slots(a.pulses, ONEWIRE_BITS_PER_BYTE);
    TEST_ASSERT_EQUAL_UINT32((uint32_t)(uintptr_t)&a.pulses[1], mock_feed_ch.CMAR);
    TEST_ASSERT_EQUAL_UINT32((uint32_t)(uintptr_t)&mock_tim1.CCR3, mock_feed_ch.CPAR);
    TEST_ASSERT_EQUAL_UINT32(ONEWIRE_BITS_PER_BYTE, mock_feed_ch.CNDTR);
    /* memory -> peripheral, 8-bit memory element, increment on */
    TEST_ASSERT_BITS_HIGH(DMA_CCR_EN | DMA_CCR_DIR | DMA_CCR_MINC | DMA_CCR_PSIZE_0,
                          mock_feed_ch.CCR);
    TEST_ASSERT_BITS_LOW(DMA_CCR_MSIZE_0 | DMA_CCR_MSIZE_1, mock_feed_ch.CCR);

    run_op();
    TEST_ASSERT_EQUAL_UINT32(0u, mock_feed_ch.CNDTR); /* fully exhausted */
    TEST_ASSERT_BITS_LOW(DMA_CCR_EN, mock_feed_ch.CCR);

    /* every transfer carried exactly the buffer element at that offset:
       one element per transfer, in order, starting past the preloaded slot 1 */
    const hw_ccr1_feed_log_t* log = hw_ccr1_feed_log();
    TEST_ASSERT_EQUAL_UINT8(ONEWIRE_BITS_PER_BYTE, log->count);
    for (uint8_t i = 0; i < ONEWIRE_BITS_PER_BYTE; i++) {
        TEST_ASSERT_EQUAL_UINT8(pat_a[i + 1], log->values[i]);
    }
    /* trailing bus release: the final transfer must be the 0 the caller wrote */
    TEST_ASSERT_EQUAL_UINT8(0u, log->values[ONEWIRE_BITS_PER_BYTE - 1]);
    assert_guards(a.guard_before, a.guard_after, 0xA5);

    /* second buffer: identical geometry but the data must follow B */
    onewire_write_slots(b.pulses, ONEWIRE_BITS_PER_BYTE);
    TEST_ASSERT_EQUAL_UINT32((uint32_t)(uintptr_t)&b.pulses[1], mock_feed_ch.CMAR);
    run_op();
    log = hw_ccr1_feed_log();
    TEST_ASSERT_EQUAL_UINT8(ONEWIRE_BITS_PER_BYTE, log->count);
    for (uint8_t i = 0; i < ONEWIRE_BITS_PER_BYTE; i++) {
        TEST_ASSERT_EQUAL_UINT8(pat_b[i + 1], log->values[i]);
    }
    assert_guards(b.guard_before, b.guard_after, 0xA5);
}

void test_dma_tx_never_reads_neighbouring_buffer(void) {
    static dma_tx_t a, b;
    static const uint8_t pat_a[ONEWIRE_BITS_PER_BYTE + 1] =
        {0x31, 0x32, 0x33, 0x34, 0x35, 0x36, 0x37, 0x38, 0x00};
    static const uint8_t pat_b[ONEWIRE_BITS_PER_BYTE + 1] =
        {0xB1, 0xB2, 0xB3, 0xB4, 0xB5, 0xB6, 0xB7, 0xB8, 0x00};
    fill_guards(a.guard_before, a.guard_after, 0xA5);
    fill_guards(b.guard_before, b.guard_after, 0xA5);
    memcpy(a.pulses, pat_a, sizeof(pat_a));
    memcpy(b.pulses, pat_b, sizeof(pat_b));
    hw_register_buf(&a.pulses[1]);
    hw_register_buf(&b.pulses[1]);

    onewire_write_slots(a.pulses, ONEWIRE_BITS_PER_BYTE);
    run_op();

    /* both buffers are registered before the model resolves CMAR, so a driver
       bug pointing CMAR at b would resolve to b and leak b's values here */
    const hw_ccr1_feed_log_t* log = hw_ccr1_feed_log();
    for (uint8_t i = 0; i < ONEWIRE_BITS_PER_BYTE; i++) {
        TEST_ASSERT_EQUAL_UINT8(pat_a[i + 1], log->values[i]);
        for (uint8_t k = 0; k < ONEWIRE_BITS_PER_BYTE; k++) {
            TEST_ASSERT_TRUE(log->values[i] != pat_b[k]); /* no cross-buffer data */
        }
    }
    assert_guards(a.guard_before, a.guard_after, 0xA5);
}

void test_dma_tx_leftover_transfer_detected(void) {
    static dma_tx_t g;
    static const uint8_t pat[ONEWIRE_BITS_PER_BYTE + 1] =
        {0x41, 0x42, 0x43, 0x44, 0x45, 0x46, 0x47, 0x48, 0x00};
    fill_guards(g.guard_before, g.guard_after, 0xA5);
    memcpy(g.pulses, pat, sizeof(pat));
    hw_register_buf(&g.pulses[1]);

    onewire_write_slots(g.pulses, ONEWIRE_BITS_PER_BYTE);
    mock_feed_ch.CNDTR += 1u; /* over-run: 9 transfers against an 8-slot op */

    TEST_ASSERT_TRUE(hw_run_until_uif(ONEWIRE_BITS_PER_BYTE));

    /* the timer only ever feeds one transfer per slot, so the ninth transfer
       can never fire here: the observation is a channel left armed - CNDTR
       not drained and EN still set after the operation finished. A correct
       op must never leave the channel in that state. */
    TEST_ASSERT_EQUAL_UINT32(1u, mock_feed_ch.CNDTR);
    TEST_ASSERT_BITS_HIGH(DMA_CCR_EN, mock_feed_ch.CCR);
    assert_guards(g.guard_before, g.guard_after, 0xA5);
}

void test_dma_tx_overrun_reads_guard(void) {
    static dma_tx_t g;
    static const uint8_t pat[ONEWIRE_BITS_PER_BYTE + 1] =
        {0x41, 0x42, 0x43, 0x44, 0x45, 0x46, 0x47, 0x48, 0x00};
    fill_guards(g.guard_before, g.guard_after, 0xA5);
    memcpy(g.pulses, pat, sizeof(pat));
    hw_register_buf(&g.pulses[1]);

    onewire_write_slots(g.pulses, ONEWIRE_BITS_PER_BYTE);
    /* consistent off-by-one: the driver claimed 9 slots for a buffer table
       that only holds 8 entries, so CNDTR is also 9 */
    mock_feed_ch.CNDTR = ONEWIRE_BITS_PER_BYTE + 1u;
    mock_tim1.RCR = ONEWIRE_BITS_PER_BYTE; /* 9 slots instead of 8 */

    run_op();
    TEST_ASSERT_EQUAL_UINT32(0u, mock_feed_ch.CNDTR); /* now fully drained */

    /* the extra transfer must not be swallowed: it reads the guard byte that
       immediately follows the 9-byte table and surfaces in the output log */
    const hw_ccr1_feed_log_t* log = hw_ccr1_feed_log();
    TEST_ASSERT_EQUAL_UINT8(ONEWIRE_BITS_PER_BYTE + 1u, log->count);
    TEST_ASSERT_EQUAL_UINT8(0xA5u, log->values[ONEWIRE_BITS_PER_BYTE]);
    /* a correct op would have placed the trailing bus-release 0 here instead */
    TEST_ASSERT_TRUE(log->values[ONEWIRE_BITS_PER_BYTE] != 0u);
}

/* ============================================================
 *  2. RX: exact destination, MINC/MSIZE stepping, guards,
 *     only-permitted-range modified
 * ============================================================ */

void test_dma_rx_read_pair_16bit_destination(void) {
    static dma_rx16_t g;
    fill_guards(g.guard_before, g.guard_after, 0xA5);
    for (uint8_t i = 0; i < 4; i++) {
        g.edge[i] = 0xEEEE; /* pre-fill: untouched elements must stay 0xEEEE */
    }
    hw_register_buf(g.edge);
    hw_set_capture_source(two_val16_src);

    onewire_read_pair(g.edge); /* capture 2 slots, 16-bit MSIZE */
    TEST_ASSERT_EQUAL_UINT32((uint32_t)(uintptr_t)&g.edge[0], mock_dma1_ch4.CMAR);
    TEST_ASSERT_EQUAL_UINT32((uint32_t)(uintptr_t)&mock_tim1.CCR4, mock_dma1_ch4.CPAR);
    TEST_ASSERT_EQUAL_UINT32(2u, mock_dma1_ch4.CNDTR);
    TEST_ASSERT_BITS_HIGH(DMA_CCR_EN | DMA_CCR_MINC | DMA_CCR_PSIZE_0 | DMA_CCR_MSIZE_0,
                          mock_dma1_ch4.CCR);
    TEST_ASSERT_BITS_LOW(DMA_CCR_DIR, mock_dma1_ch4.CCR); /* peripheral -> memory */
    TEST_ASSERT_BITS_LOW(DMA_CCR_MSIZE_1, mock_dma1_ch4.CCR); /* 16-bit, not 32-bit */

    run_op();
    TEST_ASSERT_EQUAL_UINT32(0u, mock_dma1_ch4.CNDTR);
    TEST_ASSERT_BITS_LOW(DMA_CCR_EN, mock_dma1_ch4.CCR);
    TEST_ASSERT_EQUAL_UINT32(2u, hw_capture_count());

    /* element 0 <- capture 0 at byte offset 0, element 1 <- capture 1 at
       byte offset 2 (16-bit MINC): the next transfer stepped 2 bytes */
    TEST_ASSERT_EQUAL_UINT16(0x1234u, g.edge[0]);
    TEST_ASSERT_EQUAL_UINT16(0x5678u, g.edge[1]);
    TEST_ASSERT_EQUAL_UINT16(0xEEEEu, g.edge[2]); /* outside the permitted range */
    TEST_ASSERT_EQUAL_UINT16(0xEEEEu, g.edge[3]);
    assert_guards(g.guard_before, g.guard_after, 0xA5);
}

void test_dma_rx_byte_read_8bit_minc(void) {
    static dma_rx8_t g;
    fill_guards(g.guard_before, g.guard_after, 0xA5);
    memset(g.buf, 0xEE, sizeof(g.buf));
    hw_register_buf(g.buf);
    hw_set_capture_source(two_val8_src);

    onewire_read_data(g.buf, 1); /* 1 byte = 8 slots, 8-bit MSIZE */
    TEST_ASSERT_EQUAL_UINT32(8u, mock_dma1_ch4.CNDTR);
    TEST_ASSERT_EQUAL_UINT32(7u, mock_tim1.RCR);
    TEST_ASSERT_BITS_HIGH(DMA_CCR_EN | DMA_CCR_MINC | DMA_CCR_PSIZE_0, mock_dma1_ch4.CCR);
    TEST_ASSERT_BITS_LOW(DMA_CCR_MSIZE_0 | DMA_CCR_MSIZE_1, mock_dma1_ch4.CCR); /* 8-bit */
    TEST_ASSERT_BITS_LOW(DMA_CCR_DIR, mock_dma1_ch4.CCR);

    run_op();
    TEST_ASSERT_EQUAL_UINT32(0u, mock_dma1_ch4.CNDTR);
    TEST_ASSERT_EQUAL_UINT32(8u, hw_capture_count());

    /* 8-bit MINC: capture 0 -> buf[0], capture 1 -> buf[1] - one byte apart.
       A 16-bit step would have left the LSB of source 0 (0x05) in buf[1]. */
    TEST_ASSERT_EQUAL_UINT8(0x06u, g.buf[0]);
    TEST_ASSERT_EQUAL_UINT8(0x08u, g.buf[1]);
    for (uint8_t i = 8; i < 16; i++) {
        TEST_ASSERT_EQUAL_UINT8(0xEEu, g.buf[i]); /* untouched tail */
    }
    assert_guards(g.guard_before, g.guard_after, 0xA5);
}

void test_dma_rx_full_scratchpad_fills_buffer(void) {
    static dma_rx72_t g;
    fill_guards(g.guard_before, g.guard_after, 0xA5);
    memset(g.buf, 0xEE, sizeof(g.buf));
    hw_register_buf(g.buf);
    hw_set_capture_source(byte_seq_src);

    onewire_read_data(g.buf, RX_BYTES_9); /* 9 bytes = 72 transfers */
    TEST_ASSERT_EQUAL_UINT32(RX_BITS_72, mock_dma1_ch4.CNDTR);
    TEST_ASSERT_EQUAL_UINT32(RX_BITS_72 - 1u, mock_tim1.RCR);

    run_op();
    TEST_ASSERT_EQUAL_UINT32(0u, mock_dma1_ch4.CNDTR);
    TEST_ASSERT_BITS_LOW(DMA_CCR_EN, mock_dma1_ch4.CCR);
    TEST_ASSERT_EQUAL_UINT32(RX_BITS_72, hw_capture_count());

    /* full exhaustion: all 72 bytes of the permitted range were written,
       one per transfer, in order 1..72 */
    for (uint8_t i = 0; i < RX_BITS_72; i++) {
        TEST_ASSERT_EQUAL_UINT8((uint8_t)(i + 1), g.buf[i]);
    }
    assert_guards(g.guard_before, g.guard_after, 0xA5);
}

void test_dma_rx_overrun_is_observable(void) {
    static dma_rx16_t g; /* capacity 4 x 16-bit = 8 bytes */
    fill_guards(g.guard_before, g.guard_after, 0xA5);
    for (uint8_t i = 0; i < 4; i++) {
        g.edge[i] = 0xEEEE;
    }
    hw_register_buf(g.edge);
    hw_set_capture_source(two_val16_src);

    ds18b20_test_arm_capture(g.edge, 2, 16);
    mock_dma1_ch4.CNDTR = 5u; /* config error: 5 x 16-bit = 10 bytes > 8-byte buffer */

    TEST_ASSERT_TRUE(hw_run_until_uif(8));
    TEST_ASSERT_EQUAL_UINT32(0u, mock_dma1_ch4.CNDTR);

    /* the model must NOT silently truncate: the two surplus transfers walk
       into the guard zone instead of being masked, catching the overrun */
    TEST_ASSERT_EQUAL_UINT16(0x1234u, g.edge[0]);
    TEST_ASSERT_EQUAL_UINT16(0x5678u, g.edge[1]);
    uint8_t guard_hit = 0;
    for (uint8_t i = 0; i < DMA_GUARD; i++) {
        if (g.guard_after[i] != 0xA5u) {
            guard_hit = 1;
        }
    }
    TEST_ASSERT_TRUE(guard_hit); /* overflow visible -> regression would fail */
}

void test_dma_rx_overrun_8bit_walks_guard(void) {
    static dma_rx8_t g; /* buf[16]: a 1-byte read only permits 8 transfers */
    fill_guards(g.guard_before, g.guard_after, 0xA5);
    memset(g.buf, 0xEE, sizeof(g.buf));
    hw_register_buf(g.buf);
    hw_set_capture_source(two_val8_src);

    onewire_read_data(g.buf, 1); /* 8 slots, 8-bit MSIZE, CNDTR = 8 */
    /* config error: 20 8-bit transfers against a 16-byte buffer */
    mock_dma1_ch4.CNDTR = 20u;
    mock_tim1.RCR = 19u; /* claim a matching slot budget so the op can drain */

    run_op();
    TEST_ASSERT_EQUAL_UINT32(0u, mock_dma1_ch4.CNDTR);
    TEST_ASSERT_BITS_LOW(DMA_CCR_EN, mock_dma1_ch4.CCR);
    TEST_ASSERT_EQUAL_UINT32(20u, hw_capture_count());

    /* 8-bit MINC stepped byte-by-byte inside the permitted range... */
    TEST_ASSERT_EQUAL_UINT8(0x06u, g.buf[0]);
    TEST_ASSERT_EQUAL_UINT8(0x08u, g.buf[1]);
    /* ...then the surplus transfers walked into the guard zone: exact bytes */
    TEST_ASSERT_EQUAL_UINT8(0xA5u, g.guard_before[DMA_GUARD - 1]); /* leading intact */
    TEST_ASSERT_EQUAL_UINT8(0x08u, g.guard_after[0]); /* 17th byte *was* written */
    TEST_ASSERT_EQUAL_UINT8(0x08u, g.guard_after[3]); /* 20th byte *was* written */
    TEST_ASSERT_EQUAL_UINT8(0xA5u, g.guard_after[4]); /* beyond the overrun: safe */
}

/* ============================================================
 *  3. Direction: DIR register bit + behavioural data flow
 * ============================================================ */

void test_dma_tx_direction_memory_to_peripheral(void) {
    static dma_tx_t g;
    static const uint8_t pat[ONEWIRE_BITS_PER_BYTE + 1] =
        {0x51, 0x52, 0x53, 0x54, 0x55, 0x56, 0x57, 0x58, 0x00};
    fill_guards(g.guard_before, g.guard_after, 0xA5);
    memcpy(g.pulses, pat, sizeof(pat));
    hw_register_buf(&g.pulses[1]);

    uint8_t snapshot[ONEWIRE_BITS_PER_BYTE + 1];
    onewire_write_slots(g.pulses, ONEWIRE_BITS_PER_BYTE);
    memcpy(snapshot, g.pulses, sizeof(snapshot));

    TEST_ASSERT_BITS_HIGH(DMA_CCR_DIR, mock_feed_ch.CCR); /* memory -> peripheral */
    run_op();

    /* behavioural direction: the data moved OUT of memory. The source buffer
       is untouched, the peripheral-facing output received the values. */
    TEST_ASSERT_TRUE(memcmp(snapshot, g.pulses, sizeof(snapshot)) == 0);
    const hw_ccr1_feed_log_t* log = hw_ccr1_feed_log();
    TEST_ASSERT_EQUAL_UINT8(ONEWIRE_BITS_PER_BYTE, log->count);
    TEST_ASSERT_EQUAL_UINT8(0x52u, log->values[0]);
    /* the terminal DMA reload (trailing 0) is effective in the output */
    TEST_ASSERT_EQUAL_UINT16(0u, hw_effective_ccr1());
    assert_guards(g.guard_before, g.guard_after, 0xA5);
}

void test_dma_rx_direction_peripheral_to_memory(void) {
    static dma_rx16_t g;
    fill_guards(g.guard_before, g.guard_after, 0xA5);
    for (uint8_t i = 0; i < 4; i++) {
        g.edge[i] = 0xEEEE;
    }
    hw_register_buf(g.edge);
    hw_set_capture_source(two_val16_src);

    onewire_read_pair(g.edge);
    TEST_ASSERT_BITS_LOW(DMA_CCR_DIR, mock_dma1_ch4.CCR); /* peripheral -> memory */
    run_op();

    /* behavioural direction: the data moved INTO memory through the capture
       register (CCR4); only the permitted range of the buffer changed */
    TEST_ASSERT_EQUAL_UINT32(0x5678u, mock_tim1.CCR4); /* last capture value */
    TEST_ASSERT_EQUAL_UINT16(0x1234u, g.edge[0]);
    TEST_ASSERT_EQUAL_UINT16(0x5678u, g.edge[1]);
    TEST_ASSERT_EQUAL_UINT16(0xEEEEu, g.edge[2]);
    assert_guards(g.guard_before, g.guard_after, 0xA5);
}

/* ============================================================
 *  4. Per-operation transfer geometry + CNDTR contract
 * ============================================================ */

void test_dma_reset_capture_geometry(void) {
    static uint16_t edge[OW_PORT_CAPTURE_BUF_SIZE];
    hw_register_buf(edge);
    hw_set_capture_source(presence_src);

    onewire_reset(edge);
    /* ONE slot but TWO captures: the reset bus-turnaround is 2 edges in 1 slot */
    TEST_ASSERT_EQUAL_UINT32(0u, mock_tim1.RCR);
    TEST_ASSERT_EQUAL_UINT32(OW_PORT_CAPTURE_BUF_SIZE, mock_dma1_ch4.CNDTR);
    TEST_ASSERT_EQUAL_UINT32((uint32_t)(uintptr_t)&edge[0], mock_dma1_ch4.CMAR);
    TEST_ASSERT_BITS_HIGH(DMA_CCR_EN | DMA_CCR_MINC | DMA_CCR_PSIZE_0 | DMA_CCR_MSIZE_0,
                          mock_dma1_ch4.CCR);

    run_op();
    TEST_ASSERT_EQUAL_UINT32(0u, mock_dma1_ch4.CNDTR);
    TEST_ASSERT_BITS_LOW(DMA_CCR_EN, mock_dma1_ch4.CCR);
    TEST_ASSERT_EQUAL_UINT32(OW_PORT_CAPTURE_BUF_SIZE, hw_capture_count());
    TEST_ASSERT_EQUAL_UINT16(510u, edge[0]);
    TEST_ASSERT_EQUAL_UINT16(700u, edge[1]);
}

void test_dma_write_then_read_merged_geometry(void) {
    hw_set_capture_source(merged_src);

    /* onewire_set_timing_profile() populates search_read_pulse (the CCR3 feed
       source for slots 2-3) via onewire_init() -> ds18b20_init(). run() may
       never call it before this test, so initialise here for determinism. */
    ds18b20_init();

    test_bus_write_then_read(0); /* 3 slots: write direction bit + read id/cmp */
    TEST_ASSERT_EQUAL_UINT32(2u, mock_tim1.RCR);
    TEST_ASSERT_EQUAL_UINT32(3u, mock_dma1_ch4.CNDTR); /* 3 x 16-bit captures */
    TEST_ASSERT_EQUAL_UINT32(3u, mock_feed_ch.CNDTR); /* 3 x 8-bit feed reloads */
    TEST_ASSERT_BITS_HIGH(DMA_CCR_EN | DMA_CCR_MINC | DMA_CCR_PSIZE_0 | DMA_CCR_MSIZE_0,
                          mock_dma1_ch4.CCR);
    TEST_ASSERT_BITS_LOW(DMA_CCR_DIR, mock_dma1_ch4.CCR); /* peripheral -> memory */
    TEST_ASSERT_BITS_HIGH(DMA_CCR_EN | DMA_CCR_DIR | DMA_CCR_MINC, mock_feed_ch.CCR);
    TEST_ASSERT_EQUAL_UINT32((uint32_t)(uintptr_t)&mock_tim1.CCR4, mock_dma1_ch4.CPAR);
    TEST_ASSERT_EQUAL_UINT32((uint32_t)(uintptr_t)&mock_tim1.CCR3, mock_feed_ch.CPAR);

    run_op();
    TEST_ASSERT_EQUAL_UINT32(0u, mock_dma1_ch4.CNDTR);
    TEST_ASSERT_EQUAL_UINT32(0u, mock_feed_ch.CNDTR);
    TEST_ASSERT_BITS_LOW(DMA_CCR_EN, mock_dma1_ch4.CCR);
    TEST_ASSERT_BITS_LOW(DMA_CCR_EN, mock_feed_ch.CCR);
    TEST_ASSERT_EQUAL_UINT32(3u, hw_capture_count());
    TEST_ASSERT_EQUAL_UINT8(3u, hw_ccr1_feed_log()->count);

    /* feed reloads the read pulses for slots 2..3 and the trailing release 0 */
    const hw_ccr1_feed_log_t* log = hw_ccr1_feed_log();
    TEST_ASSERT_EQUAL_UINT16(ONE, log->values[0]);
    TEST_ASSERT_EQUAL_UINT16(ONE, log->values[1]);
    TEST_ASSERT_EQUAL_UINT16(0u, log->values[2]);

    /* captures landed sequentially in the merged 16-bit edge buffer */
    TEST_ASSERT_EQUAL_UINT16(0x1000u, test_search_edge(0));
    TEST_ASSERT_EQUAL_UINT16(0x2000u, test_search_edge(1));
    TEST_ASSERT_EQUAL_UINT16(0x3000u, test_search_edge(2));
}

void test_dma_single_bit_write_uses_no_dma(void) {
    onewire_write_bit(1);
    /* the single-slot write path programs CCR3 directly: by design there is
       never a DMA feed (and thus never CNDTR > 0) for one slot. */
    TEST_ASSERT_TRUE(mock_tim1.CR1 & TIM_CR1_CEN);
    TEST_ASSERT_EQUAL_UINT32(0u, mock_tim1.RCR);
    TEST_ASSERT_EQUAL_UINT32(0u, mock_feed_ch.CNDTR);
    TEST_ASSERT_BITS_LOW(DMA_CCR_EN, mock_feed_ch.CCR);
    TEST_ASSERT_BITS_LOW(MOCK_TIM_FEED_DE, mock_tim1.DIER);
    /* the capture channel must be equally untouched: no capture DMA armed */
    TEST_ASSERT_EQUAL_UINT32(0u, mock_dma1_ch4.CNDTR);
    TEST_ASSERT_BITS_LOW(DMA_CCR_EN | DMA_CCR_MINC | DMA_CCR_MSIZE_0, mock_dma1_ch4.CCR);
    TEST_ASSERT_BITS_LOW(MOCK_TIM_CAP_DE, mock_tim1.DIER);

    run_op();
    TEST_ASSERT_EQUAL_UINT8(0u, hw_ccr1_feed_log()->count); /* zero transfers */
}

void test_dma_cndtr_one_transfer(void) {
    static dma_rx16_t g;
    fill_guards(g.guard_before, g.guard_after, 0xA5);
    for (uint8_t i = 0; i < 4; i++) {
        g.edge[i] = 0xEEEE;
    }
    hw_register_buf(g.edge);
    hw_set_capture_source(one_val_src);

    ds18b20_test_arm_capture(g.edge, 1, 16); /* minimal transfer count */
    TEST_ASSERT_EQUAL_UINT32(1u, mock_dma1_ch4.CNDTR);
    TEST_ASSERT_BITS_HIGH(DMA_CCR_EN | DMA_CCR_MINC | DMA_CCR_PSIZE_0 | DMA_CCR_MSIZE_0,
                          mock_dma1_ch4.CCR);

    run_op();
    TEST_ASSERT_EQUAL_UINT32(0u, mock_dma1_ch4.CNDTR);
    TEST_ASSERT_EQUAL_UINT32(1u, hw_capture_count());
    TEST_ASSERT_EQUAL_UINT16(0x0777u, g.edge[0]);
    TEST_ASSERT_EQUAL_UINT16(0xEEEEu, g.edge[1]); /* single transfer only */
    assert_guards(g.guard_before, g.guard_after, 0xA5);
}

/* ============================================================
 *  5. Roles / slots -> transfers -> buffer size consistency
 * ============================================================ */

void test_dma_match_rom_resolution_writes_104_slots(void) {
    uint8_t rom[DS18B20_ROM_BYTES] = {0x28, 0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF, 0x00};
    hw_set_capture_source(presence_src);

    ds18b20_init();
    ds18b20_test_reset_ctx();
    ds18b20_test_reset_resolution();
    ds18b20_select(rom); /* Match ROM mode */
    TEST_ASSERT_EQUAL_UINT8(1u, ds18b20_test_get_address_mode());

    ds18b20_set_resolution(9); /* reset scheduled */
    TEST_ASSERT_TRUE(mock_tim1.CR1 & TIM_CR1_CEN);
    TEST_ASSERT_EQUAL_UINT32(0u, mock_tim1.RCR);

    /* op 1: reset - 1 slot, 2 captures */
    run_op();
    TEST_ASSERT_EQUAL_UINT32(OW_PORT_CAPTURE_BUF_SIZE, hw_capture_count());

    /* op 2: Match-ROM config write. 13 bytes x 8 bits = 104 slots. The number
       of DMA transfers equals the number of SLOTS (104), not the number of
       BYTES (13) - the buffer holds 104 pulses + 1 release pulse. */
    TEST_ASSERT_EQUAL_UINT8(0u, ds18b20_set_resolution_poll()); /* still running */
    TEST_ASSERT_TRUE(mock_tim1.CR1 & TIM_CR1_CEN);
    TEST_ASSERT_EQUAL_UINT32(103u, mock_tim1.RCR); /* 104 slots */
    TEST_ASSERT_EQUAL_UINT32(104u, mock_feed_ch.CNDTR);
    TEST_ASSERT_BITS_HIGH(DMA_CCR_EN | DMA_CCR_DIR | DMA_CCR_MINC, mock_feed_ch.CCR);

    run_op();
    TEST_ASSERT_EQUAL_UINT32(0u, mock_feed_ch.CNDTR);
    TEST_ASSERT_BITS_LOW(DMA_CCR_EN, mock_feed_ch.CCR);

    /* the DMA feed delivered the pre-built 104-slot table exactly: transfer i
       carried pulses[i+1], and the last slot got the trailing release 0 */
    {
        const hw_ccr1_feed_log_t* log = hw_ccr1_feed_log();
        TEST_ASSERT_EQUAL_UINT8(104u, log->count);
        for (uint8_t i = 0; i < 104; i++) {
            TEST_ASSERT_EQUAL_UINT8(ds18b20_test_get_res_pulse((uint8_t)(i + 1)),
                                    (uint8_t)log->values[i]);
        }
        TEST_ASSERT_EQUAL_UINT8(0u, (uint8_t)log->values[103]); /* release zero */
    }

    /* the two DMA modes only moved the output: the pulse table is intact */
    TEST_ASSERT_EQUAL_UINT8(0u, ds18b20_test_get_res_pulse(104)); /* trailing 0 */

    /* change completes and hands the timer back with the resolution applied */
    TEST_ASSERT_EQUAL_UINT8(0u, ds18b20_set_resolution_poll()); /* WRITE -> DONE */
    TEST_ASSERT_EQUAL_UINT8(1u, ds18b20_set_resolution_poll()); /* DONE -> finished */
    TEST_ASSERT_EQUAL_UINT8(9u, ds18b20_get_resolution());
}

/* ----------------------- single-device search ----------------------- */

static uint8_t dma_g_rom[8];
static uint8_t dma_g_wr_bit; /* bit whose pair the next merged write+read returns */
static uint8_t dma_found_roms[4][8];
static uint8_t dma_found_count;

static uint8_t dma_search_sink(const uint8_t* rom) {
    memcpy(dma_found_roms[dma_found_count++], rom, DS18B20_ROM_BYTES);
    return 0;
}

/* Same answer pattern as tests/test/test_search.c: infer the running
 * operation from the mock timer and answer its captures for one device. */
static uint16_t search_one_dev_src(uint32_t idx) {
    uint8_t rcr = (uint8_t)mock_tim1.RCR;
    if (rcr == 0) {
        return idx == 0 ? 510u : 700u; /* reset + presence pulse */
    }
    if (rcr == 1) { /* first read pair: bit 1 */
        uint8_t b = (dma_g_rom[0] >> 0) & 1u;
        return (idx == 0) ? (b ? ONE : ZERO) : (b ? ZERO : ONE);
    }
    uint8_t byte = (dma_g_wr_bit - 1u) / 8u;
    uint8_t bit = (dma_g_wr_bit - 1u) % 8u;
    uint8_t b = (dma_g_rom[byte] >> bit) & 1u;
    if (idx == 0) return 0u;
    if (idx == 1) return b ? ONE : ZERO;
    dma_g_wr_bit++; /* last capture of this op: next op answers the next bit */
    return b ? ZERO : ONE;
}

void test_dma_search_transfer_accounting(void) {
    uint8_t serial[7] = {0x28, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06};
    memcpy(dma_g_rom, serial, 7);
    dma_g_rom[7] = ds18b20_crc8(dma_g_rom, 7);

    dma_found_count = 0;
    dma_g_wr_bit = 2;
    hw_set_capture_source(search_one_dev_src);
    ds18b20_init(); /* deterministic timing/feed statics before the search walk */
    ds18b20_search_start(dma_search_sink, 1);

    uint32_t feed_total = 0, cap_total = 0;
    uint16_t guard = 0;
    for (;;) {
        if (ds18b20_search_poll()) {
            break;
        }
        if (mock_tim1.CR1 & TIM_CR1_CEN) {
            const uint32_t pre_feed = mock_feed_ch.CNDTR;
            const uint32_t pre_cap = mock_dma1_ch4.CNDTR;
            const uint32_t rcr = (uint32_t)(mock_tim1.RCR & 0xFFu);
            const uint32_t arr = (uint32_t)mock_tim1.ARR;

            run_op();
            /* a correct search operation must fully exhaust every DMA channel */
            TEST_ASSERT_EQUAL_UINT32(0u, mock_feed_ch.CNDTR);
            TEST_ASSERT_EQUAL_UINT32(0u, mock_dma1_ch4.CNDTR);
            TEST_ASSERT_BITS_LOW(DMA_CCR_EN, mock_feed_ch.CCR);
            TEST_ASSERT_BITS_LOW(DMA_CCR_EN, mock_dma1_ch4.CCR);
            /* the model performed exactly CNDTR transfers in each direction */
            TEST_ASSERT_EQUAL_UINT32(pre_feed, hw_ccr1_feed_log()->count);
            TEST_ASSERT_EQUAL_UINT32(pre_cap, hw_capture_count());

            /* slots -> transfers consistency for every search operation:
               - feed always transfers one element per slot (feed CNDTR == RCR+1);
               - capture transfers == RCR+1 slots, except the reset which
                 captures 2 edges in its single slot (CNDTR == 2 x (RCR+1)); */
            if (pre_feed > 0u) {
                TEST_ASSERT_EQUAL_UINT32(rcr + 1u, pre_feed);
            }
            if (pre_cap > 0u) {
                if (arr == OW_PORT_RESET_TIMEOUT) { /* reset: 1 slot, 2 captures */
                    TEST_ASSERT_EQUAL_UINT32(2u * (rcr + 1u), pre_cap);
                } else {
                    TEST_ASSERT_EQUAL_UINT32(rcr + 1u, pre_cap);
                }
            }
            feed_total += pre_feed;
            cap_total += pre_cap;
        }
        if (++guard > 500) {
            break;
        }
    }
    TEST_ASSERT_TRUE(guard <= 500);

    TEST_ASSERT_EQUAL_UINT8(1u, ds18b20_search_count());
    TEST_ASSERT_EQUAL_UINT8(1u, dma_found_count);
    for (uint8_t i = 0; i < DS18B20_ROM_BYTES; i++) {
        TEST_ASSERT_EQUAL_HEX8(dma_g_rom[i], dma_found_roms[0][i]);
    }

    /* 0xF0 command feed: 8 transfers. First id/cmp pair: read_pair capture 2.
       The 64 address bits walk through 63 merged write+read operations, each
       doing 3 feed (direction write reloads + read pulses + release 0) and
       3 capture (16-bit write-slot/id/cmp edges), plus the final single-slot
       write_bit for bit 64 - which uses no DMA.
         feed = 8 + 63*3,  capture = 2 (reset) + 2 (read pair) + 63*3 */
    TEST_ASSERT_EQUAL_UINT32(8u + 63u * 3u, feed_total);
    TEST_ASSERT_EQUAL_UINT32(2u + 2u + 63u * 3u, cap_total);
}

/* ============================================================
 *  Runner
 * ============================================================ */

void run_test_dma(void) {
    TEST_RUN(test_dma_tx_reads_exact_buffer_in_order);
    TEST_RUN(test_dma_tx_never_reads_neighbouring_buffer);
    TEST_RUN(test_dma_tx_leftover_transfer_detected);
    TEST_RUN(test_dma_tx_overrun_reads_guard);
    TEST_RUN(test_dma_rx_read_pair_16bit_destination);
    TEST_RUN(test_dma_rx_byte_read_8bit_minc);
    TEST_RUN(test_dma_rx_full_scratchpad_fills_buffer);
    TEST_RUN(test_dma_rx_overrun_is_observable);
    TEST_RUN(test_dma_rx_overrun_8bit_walks_guard);
    TEST_RUN(test_dma_tx_direction_memory_to_peripheral);
    TEST_RUN(test_dma_rx_direction_peripheral_to_memory);
    TEST_RUN(test_dma_reset_capture_geometry);
    TEST_RUN(test_dma_write_then_read_merged_geometry);
    TEST_RUN(test_dma_single_bit_write_uses_no_dma);
    TEST_RUN(test_dma_cndtr_one_transfer);
    TEST_RUN(test_dma_match_rom_resolution_writes_104_slots);
    TEST_RUN(test_dma_search_transfer_accounting);
}