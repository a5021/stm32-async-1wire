/* ============================================================
 *  test_dma_contract.c - per-operation DMA register contract
 *
 *  The DMA tests in test_dma.c verify each operation with its own
 *  hand-written assertions. This file formalises the *same* facts
 *  (and a few more) as one table: what the driver must have
 *  programmed into the mock TIM1/DMA registers before the timer
 *  starts, for every scheduleable hardware operation:
 *
 *    operation         dir  CPAR        CMAR          MSIZE      CNDTR       RCR
 *    ----------------- ---- ----------- ------------- ---------- ----------- ----
 *    reset             P->M CCR4        reset buffer  16         2           0
 *    read pair         P->M CCR4        pair buffer   16         2           1
 *    read data (N B)   P->M CCR4        user buffer   8          N*8         N*8-1
 *    write (N slots)   M->P CCR3        cmd+1         8          N           N-1
 *    merged w+r        both CCR3/CCR4   exact builds  8 / 16     3 / 3       2
 *    Match-ROM write   M->P CCR3        res pulses+1  8          104         103
 *
 *  plus the boundaries (write 256 / read 32 bytes -> RCR 255) and the
 *  single-bit write, whose whole contract is that *no* DMA channel is
 *  armed at all.
 *
 *  Each row also asserts the post-op accounting: every scheduled
 *  transfer fired and both channels drained to 0 / EN clear.
 * ============================================================ */

#include "ds18b20.h"
#include "ds18b20_test_access.h"
#include "hw_model.h"
#include "mock_target.h"
#include "onewire.h"
#include "ow_port.h"
#include "unity.h"
#include <string.h>

#define ONE ONEWIRE_ONE_PULSE
#define ZERO ONEWIRE_ZERO_PULSE

static uint16_t presence_src(uint32_t idx) {
    return idx == 0 ? 510u : 700u;
}

/* CCR bit fields a correct operation must program (see table above). */
#define FEED_8BIT_HI (DMA_CCR_EN | DMA_CCR_DIR | DMA_CCR_MINC | DMA_CCR_PSIZE_0)
#define FEED_8BIT_LO (DMA_CCR_MSIZE_0 | DMA_CCR_MSIZE_1)
#define CAP_16BIT_HI (DMA_CCR_EN | DMA_CCR_MINC | DMA_CCR_PSIZE_0 | DMA_CCR_MSIZE_0)
#define CAP_16BIT_LO (DMA_CCR_DIR | DMA_CCR_MSIZE_1)
#define CAP_8BIT_HI (DMA_CCR_EN | DMA_CCR_MINC | DMA_CCR_PSIZE_0)
#define CAP_8BIT_LO (DMA_CCR_DIR | DMA_CCR_MSIZE_0 | DMA_CCR_MSIZE_1)

/* ------------------------------------------------------------------ */

typedef struct {
    const char* spec;
    void (*schedule)(void); /* drive the driver to the armed state */

    uint32_t exp_rcr;

    /* feed (CCR3) channel; exp_*_cmar == 0 means "must be disarmed" */
    uintptr_t exp_feed_cmar;
    uintptr_t exp_feed_cpar;
    uint32_t exp_feed_cndtr;
    uint32_t exp_feed_ccr_hi;
    uint32_t exp_feed_ccr_lo;

    /* capture (CCR4) channel; exp_*_cmar == 0 means "must be disarmed" */
    uintptr_t exp_cap_cmar;
    uintptr_t exp_cap_cpar;
    uint32_t exp_cap_cndtr;
    uint32_t exp_cap_ccr_hi;
    uint32_t exp_cap_ccr_lo;

    uint32_t exp_dier_lo; /* DMA-enable bits that MUST be off for this op */

    /* accounting after the timer ran to its terminal update */
    uint32_t exp_feed_xfer;
    uint32_t exp_cap_xfer;
} dma_contract_row_t;

/* ---------------------- row schedules ---------------------- */

static uint8_t w8[ONEWIRE_BITS_PER_BYTE + 1];
static uint8_t w256[ONEWIRE_MAX_SLOTS + 1];

static void sched_write_8(void) {
    memset(w8, ONE, sizeof(w8));
    w8[ONEWIRE_BITS_PER_BYTE] = 0u;
    hw_register_buf(&w8[1]);
    onewire_write_slots(w8, ONEWIRE_BITS_PER_BYTE);
}

static void sched_write_256(void) {
    memset(w256, ZERO, sizeof(w256));
    w256[ONEWIRE_MAX_SLOTS] = 0u;
    hw_register_buf(&w256[1]);
    onewire_write_slots(w256, ONEWIRE_MAX_SLOTS);
}

static uint16_t rst_buf[OW_PORT_CAPTURE_BUF_SIZE];

static void sched_reset(void) {
    hw_register_buf(rst_buf);
    onewire_reset(rst_buf);
}

static uint16_t pair_buf[2];

static void sched_read_pair(void) {
    hw_register_buf(pair_buf);
    onewire_read_pair(pair_buf);
}

static uint8_t rx8[8];

static void sched_read_data_1(void) {
    hw_register_buf(rx8);
    onewire_read_data(rx8, 1);
}

static uint8_t rx32[ONEWIRE_MAX_READ_BYTES * ONEWIRE_BITS_PER_BYTE];

static void sched_read_data_32(void) {
    hw_register_buf(rx32);
    onewire_read_data(rx32, ONEWIRE_MAX_READ_BYTES);
}

static void sched_merged(void) {
    test_bus_write_then_read(0); /* 3 slots: direction write + id/cmp read */
}

static void sched_match_rom_104(void) {
    static uint8_t rom[DS18B20_ROM_BYTES] = {0x28, 0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF, 0x00};
    hw_set_capture_source(presence_src); /* internal reset must see a presence pulse */
    ds18b20_init();
    ds18b20_test_reset_ctx();
    ds18b20_test_reset_resolution();
    ds18b20_select(rom);
    ds18b20_set_resolution(9); /* schedules the reset op... */
    static const uint32_t slots = 1u;
    TEST_ASSERT_TRUE(hw_run_until_uif(slots)); /* ...complete it first */
    /* advancing the state machine arms the Match-ROM 104-slot config write */
    TEST_ASSERT_EQUAL_UINT8(0u, ds18b20_set_resolution_poll()); /* still running */
}

static void sched_single_bit_write(void) {
    onewire_write_bit(1);
}

/* ---------------------- contract table ---------------------- */
/* The merged / Match-ROM rows carry 0 here: their exact CMAR target is the
 * driver's internal buffer, filled in at runtime below. */

static dma_contract_row_t k_contracts[] = {
    {
        "write_slots 8 (TX)",
        sched_write_8,
        /* rcr */ 7u,
        (uintptr_t)&w8[1],
        (uintptr_t)&mock_tim1.CCR3,
        8u,
        FEED_8BIT_HI,
        FEED_8BIT_LO,
        0u,
        0u,
        0u,
        0u,
        0u,
        MOCK_TIM_CAP_DE,
        8u,
        0u,
    },
    {
        "write_slots 256 (TX max)",
        sched_write_256,
        255u,
        (uintptr_t)&w256[1],
        (uintptr_t)&mock_tim1.CCR3,
        256u,
        FEED_8BIT_HI,
        FEED_8BIT_LO,
        0u,
        0u,
        0u,
        0u,
        0u,
        MOCK_TIM_CAP_DE,
        256u,
        0u,
    },
    {
        "reset (RX)",
        sched_reset,
        0u,
        0u,
        0u,
        0u,
        0u,
        0u,
        (uintptr_t)&rst_buf[0],
        (uintptr_t)&mock_tim1.CCR4,
        2u,
        CAP_16BIT_HI,
        CAP_16BIT_LO,
        MOCK_TIM_FEED_DE,
        0u,
        2u,
    },
    {
        "read pair (RX)",
        sched_read_pair,
        1u,
        0u,
        0u,
        0u,
        0u,
        0u,
        (uintptr_t)&pair_buf[0],
        (uintptr_t)&mock_tim1.CCR4,
        2u,
        CAP_16BIT_HI,
        CAP_16BIT_LO,
        MOCK_TIM_FEED_DE,
        0u,
        2u,
    },
    {
        "read data 1 byte (RX)",
        sched_read_data_1,
        7u,
        0u,
        0u,
        0u,
        0u,
        0u,
        (uintptr_t)&rx8[0],
        (uintptr_t)&mock_tim1.CCR4,
        8u,
        CAP_8BIT_HI,
        CAP_8BIT_LO,
        MOCK_TIM_FEED_DE,
        0u,
        8u,
    },
    {
        "read data 32 bytes (RX max)",
        sched_read_data_32,
        255u,
        0u,
        0u,
        0u,
        0u,
        0u,
        (uintptr_t)&rx32[0],
        (uintptr_t)&mock_tim1.CCR4,
        256u,
        CAP_8BIT_HI,
        CAP_8BIT_LO,
        MOCK_TIM_FEED_DE,
        0u,
        256u,
    },
    {
        "merged write+read (search)",
        sched_merged,
        2u,
        0u,
        (uintptr_t)&mock_tim1.CCR3, /* CMAR patched at runtime */
        3u,
        FEED_8BIT_HI,
        FEED_8BIT_LO,
        0u,
        (uintptr_t)&mock_tim1.CCR4, /* CMAR patched at runtime */
        3u,
        CAP_16BIT_HI,
        CAP_16BIT_LO,
        0u, /* both DE bits are set for this op */
        3u,
        3u,
    },
    {
        "Match-ROM config write 104",
        sched_match_rom_104,
        103u,
        0u,
        (uintptr_t)&mock_tim1.CCR3, /* CMAR patched at runtime */
        104u,
        FEED_8BIT_HI,
        FEED_8BIT_LO,
        0u,
        0u,
        0u,
        0u,
        0u,
        MOCK_TIM_CAP_DE,
        104u,
        0u,
    },
    {
        "single-bit write (no DMA)",
        sched_single_bit_write,
        0u,
        0u,
        0u,
        0u,
        0u,
        0u, /* feed disarmed */
        0u,
        0u,
        0u,
        0u,
        0u, /* capture disarmed */
        MOCK_TIM_FEED_DE | MOCK_TIM_CAP_DE,
        0u,
        0u,
    },
};

/* ----------------------- one row runner ----------------------- */

static void run_contract_row(const dma_contract_row_t* row) {
    row->schedule();

    TEST_ASSERT_EQUAL_UINT32(row->exp_rcr, (uint32_t)(mock_tim1.RCR & 0xFFu));

    /* feed channel: exact CMAR/CPAR/CNDTR, required CCR bits */
    if (row->exp_feed_cmar != 0u) {
        TEST_ASSERT_EQUAL_UINT32((uint32_t)row->exp_feed_cmar, mock_feed_ch.CMAR);
        TEST_ASSERT_EQUAL_UINT32((uint32_t)row->exp_feed_cpar, mock_feed_ch.CPAR);
        TEST_ASSERT_EQUAL_UINT32(row->exp_feed_cndtr, mock_feed_ch.CNDTR);
        TEST_ASSERT_BITS_HIGH(row->exp_feed_ccr_hi, mock_feed_ch.CCR);
        TEST_ASSERT_BITS_LOW(row->exp_feed_ccr_lo, mock_feed_ch.CCR);
    } else {
        TEST_ASSERT_EQUAL_UINT32(0u, mock_feed_ch.CNDTR);
        TEST_ASSERT_BITS_LOW(DMA_CCR_EN, mock_feed_ch.CCR);
    }

    /* capture channel */
    if (row->exp_cap_cmar != 0u) {
        TEST_ASSERT_EQUAL_UINT32((uint32_t)row->exp_cap_cmar, mock_dma1_ch4.CMAR);
        TEST_ASSERT_EQUAL_UINT32((uint32_t)row->exp_cap_cpar, mock_dma1_ch4.CPAR);
        TEST_ASSERT_EQUAL_UINT32(row->exp_cap_cndtr, mock_dma1_ch4.CNDTR);
        TEST_ASSERT_BITS_HIGH(row->exp_cap_ccr_hi, mock_dma1_ch4.CCR);
        TEST_ASSERT_BITS_LOW(row->exp_cap_ccr_lo, mock_dma1_ch4.CCR);
    } else {
        TEST_ASSERT_EQUAL_UINT32(0u, mock_dma1_ch4.CNDTR);
        TEST_ASSERT_BITS_LOW(DMA_CCR_EN, mock_dma1_ch4.CCR);
    }

    /* the DMA-enable bits this operation must NOT use */
    TEST_ASSERT_BITS_LOW(row->exp_dier_lo, mock_tim1.DIER);

    /* run the timer to its terminal update and verify the accounting:
       every scheduled transfer fired, everything drained */
    uint32_t slots = (uint32_t)(mock_tim1.RCR & 0xFFu) + 1u;
    TEST_ASSERT_TRUE(mock_tim1.CR1 & TIM_CR1_CEN);
    TEST_ASSERT_TRUE(hw_run_until_uif(slots));
    TEST_ASSERT_EQUAL_UINT32(0u, mock_feed_ch.CNDTR);
    TEST_ASSERT_BITS_LOW(DMA_CCR_EN, mock_feed_ch.CCR);
    TEST_ASSERT_EQUAL_UINT32(0u, mock_dma1_ch4.CNDTR);
    TEST_ASSERT_BITS_LOW(DMA_CCR_EN, mock_dma1_ch4.CCR);
    TEST_ASSERT_EQUAL_UINT32(row->exp_feed_xfer, hw_ccr3_feed_log()->total);
    TEST_ASSERT_EQUAL_UINT32(row->exp_cap_xfer, hw_capture_count());
}

void test_dma_contract_table(void) {
    /* exact internal-buffer CMAR targets (addresses are runtime constants) */
    k_contracts[6].exp_feed_cmar = (uintptr_t)test_search_read_pulse_addr();
    k_contracts[6].exp_cap_cmar = (uintptr_t)test_search_pulse3_addr();
    k_contracts[7].exp_feed_cmar = (uintptr_t)test_res_pulses_feed_addr();

    for (uint32_t i = 0u; i < (uint32_t)(sizeof(k_contracts) / sizeof(k_contracts[0])); i++) {
        run_contract_row(&k_contracts[i]);
    }
}

void run_test_dma_contract(void) {
    TEST_RUN(test_dma_contract_table);
}