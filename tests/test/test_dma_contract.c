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

/* CCR bit fields a correct operation must program (see table above).
 * Direct mode forces the memory width to PSIZE: F4 feeds 16-bit halfwords
 * from the ow_pulse_t buffer (MSIZE_0) and drops PSIZE for 8-bit captures,
 * F1 feeds/reads 8-bit cells (MSIZE_0 low) and keeps PSIZE_0 throughout.
 * H5 programs GPDMA: CCR carries EN only, the transfer class lives in
 * CTR1/CTR2 (pinned by test_dma_cr_value_macros below). */
#if defined(STM32H5)
#define WIDTH_MSIZE16 0u
#define WIDTH_PSIZE8 0u
#define FEED_8BIT_HI DMA_CCR_EN
#define FEED_8BIT_LO (~DMA_CCR_EN)
#define CAP_16BIT_HI DMA_CCR_EN
#define CAP_16BIT_LO (~DMA_CCR_EN)
#define CAP_8BIT_HI DMA_CCR_EN
#define CAP_8BIT_LO (~DMA_CCR_EN)
#elif defined(STM32F4)
#define WIDTH_MSIZE16 DMA_CCR_MSIZE_0
#define WIDTH_PSIZE8 0u
#define FEED_8BIT_HI (DMA_CCR_EN | DMA_CCR_DIR | DMA_CCR_MINC | DMA_CCR_PSIZE_0 | WIDTH_MSIZE16)
#define FEED_8BIT_LO (DMA_CCR_MSIZE_1)
#define CAP_16BIT_HI (DMA_CCR_EN | DMA_CCR_MINC | DMA_CCR_PSIZE_0 | DMA_CCR_MSIZE_0)
#define CAP_16BIT_LO (DMA_CCR_DIR | DMA_CCR_MSIZE_1)
#define CAP_8BIT_HI (DMA_CCR_EN | DMA_CCR_MINC | WIDTH_PSIZE8)
#define CAP_8BIT_LO (DMA_CCR_DIR | DMA_CCR_MSIZE_0 | DMA_CCR_MSIZE_1)
#else
#define WIDTH_MSIZE16 0u
#define WIDTH_PSIZE8 DMA_CCR_PSIZE_0
#define FEED_8BIT_HI (DMA_CCR_EN | DMA_CCR_DIR | DMA_CCR_MINC | DMA_CCR_PSIZE_0 | WIDTH_MSIZE16)
#define FEED_8BIT_LO (DMA_CCR_MSIZE_1)
#define CAP_16BIT_HI (DMA_CCR_EN | DMA_CCR_MINC | DMA_CCR_PSIZE_0 | DMA_CCR_MSIZE_0)
#define CAP_16BIT_LO (DMA_CCR_DIR | DMA_CCR_MSIZE_1)
#define CAP_8BIT_HI (DMA_CCR_EN | DMA_CCR_MINC | WIDTH_PSIZE8)
#define CAP_8BIT_LO (DMA_CCR_DIR | DMA_CCR_MSIZE_0 | DMA_CCR_MSIZE_1)
#endif

/* The request id a channel carries, read from the live registers through the
 * model - it knows each family's register layout (the DMAMUX channel on
 * G0/G4, the GPDMA transfer register on H5), and reports 0 where the request
 * map is fixed in silicon and nothing is programmed at all.
 *
 * The *expectation* comes from the guarantee ledger above, so the two live
 * families and the five vacuous ones are stated in one place. */
static uint32_t route_feed_id(void) {
    return hw_feed_request();
}

static uint32_t route_cap_id(void) {
    return hw_capture_request();
}

/* A pointer comparison with a message that names the buffer.
 *
 * The DMA registers hold a truncated 32-bit address, so two different
 * buffers can print as the same number on a 64-bit host, and the mock's
 * Unity shim prints the expression rather than the values. The model
 * resolves the address back to the real buffer, so the mismatch can name
 * it. */
static void assert_dma_buffer(const char* what, const void* expected, const void* got) {
    if (expected != got) {
        printf("FAIL %s:%d  %s: expected %p, got %p\n", __FILE__, __LINE__, what,
               expected, got);
        unity_failures++;
    }
}

/* ------------------------------------------------------------------
 *  The DMA guarantee ledger
 * ------------------------------------------------------------------
 *  Which DMA guarantees hold on the family under test, and which are
 *  vacuous here and why.
 *
 *  This is checked, not documented. A family that is not on this list
 *  fails the suite - a new port has to say what it proves - and a row
 *  that no longer matches what the family does fails too, so the ledger
 *  cannot rot silently.
 *
 *  The routing rows are the only family-dependent ones: the other
 *  guarantees (the exact source and destination, the transfer counts,
 *  the width, the direction, the increment, the repetition count, the
 *  temporal sequence) hold everywhere and are checked for every
 *  operation by the contract table below.
 */
typedef struct {
    const char* family;
    uint32_t feed_request; /* the request the feed channel must carry; 0 =
                               the family has a fixed request map and the
                               port programs nothing */
    uint32_t cap_request; /* the same for the capture channel */
    const char* routing; /* how the request is selected on this family */
} dma_ledger_row_t;

static const dma_ledger_row_t k_ledger[] = {
    /* F0/F1/F3/F4: the request map is fixed in the silicon (TIM1_CH2
     * reaches the feed channel, TIM1_CH4 the capture channel), so the port
     * programs nothing and there is nothing to assert. */
    {"f0", 0u, 0u, "fixed request map in the silicon"},
    {"f1", 0u, 0u, "fixed request map in the silicon"},
    {"f3", 0u, 0u, "fixed request map in the silicon"},
    {"f4", 0u, 0u, "fixed request map in the silicon"},
    /* G0/G4: no fixed map - the request is selected in the DMAMUX
     * channel (TIM1_CH2 -> 21/43, TIM1_CH4 -> 23/45). */
    {"g0", 21u, 23u, "DMAMUX channel request id"},
    {"g4", 43u, 45u, "DMAMUX channel request id"},
    /* H5: no DMAMUX - the GPDMA transfer register's REQSEL carries the
     * request (TIM1_CH2 -> 59, TIM1_CH4 -> 61). */
    {"h5", 59u, 61u, "GPDMA transfer register REQSEL"},
};

#if defined(STM32F0)
#define CURRENT_FAMILY "f0"
#elif defined(STM32F1)
#define CURRENT_FAMILY "f1"
#elif defined(STM32F3)
#define CURRENT_FAMILY "f3"
#elif defined(STM32F4)
#define CURRENT_FAMILY "f4"
#elif defined(STM32G0)
#define CURRENT_FAMILY "g0"
#elif defined(STM32G4)
#define CURRENT_FAMILY "g4"
#elif defined(STM32H5)
#define CURRENT_FAMILY "h5"
#else
#error "test_dma_contract: unknown family - add it to the DMA guarantee ledger"
#endif

static const dma_ledger_row_t* ledger_row(void) {
    for (unsigned i = 0; i < sizeof(k_ledger) / sizeof(k_ledger[0]); i++) {
        if (strcmp(k_ledger[i].family, CURRENT_FAMILY) == 0) {
            return &k_ledger[i];
        }
    }
    return NULL;
}

/* The request ids this family must carry, from the ledger. A family that is
 * not on the ledger yields 0, and the ledger test below says so loudly
 * rather than letting the checks pass vacuously. */
static uint32_t ledger_feed_request(void) {
    const dma_ledger_row_t* row = ledger_row();
    return (row != NULL) ? row->feed_request : 0u;
}

static uint32_t ledger_cap_request(void) {
    const dma_ledger_row_t* row = ledger_row();
    return (row != NULL) ? row->cap_request : 0u;
}

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

static ow_pulse_t w8[ONEWIRE_BITS_PER_BYTE + 1];
static ow_pulse_t w256[ONEWIRE_MAX_SLOTS + 1];

static void sched_write_8(void) {
    for (uint16_t i = 0; i < ONEWIRE_BITS_PER_BYTE; i++) {
        w8[i] = (ow_pulse_t)ONE;
    }
    w8[ONEWIRE_BITS_PER_BYTE] = 0u;
    hw_register_buf(&w8[1]);
    onewire_write_slots(w8, ONEWIRE_BITS_PER_BYTE);
}

static void sched_write_256(void) {
    for (uint16_t i = 0; i < ONEWIRE_MAX_SLOTS; i++) {
        w256[i] = (ow_pulse_t)ZERO;
    }
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

/* The temporal contract of one scheduled operation.
 *
 * The register checks say *what* the driver programmed; this says *when*:
 * the CCR3 reload follows the slot-end compare and never precedes it, the
 * slot boundaries fall where the repetition count says, and the operation
 * ends on the terminal update - with UIF set and the timer stopped.
 *
 * There are no per-row expectations: the counts follow from the row's own
 * fields (the repetition count gives the slots, the feed transfer count
 * gives the reloads), so every operation is checked the same way. */
static void assert_temporal_contract(uint32_t slots, uint32_t feeds) {
    uint32_t cc2 = 0, feed = 0, updates = 0, terminal = 0, idle = 0;
    hw_tim_event_t prev = HW_TIM_EV_IDLE;
    uint16_t slot_pulse = 0;
    int in_slot = 0;
    hw_tim_init();
    for (uint32_t k = 0; k < 8192u; k++) {
        hw_tim_event_t e = hw_tim_step();
        if (e == HW_TIM_EV_CC2) {
            cc2++;
            /* the compare must not have moved the pulse in effect: the
             * slot's value holds for the whole slot */
            if (in_slot) {
                TEST_ASSERT_EQUAL_UINT16(slot_pulse, hw_tim_active());
            }
        } else if (e == HW_TIM_EV_FEED) {
            feed++;
            /* a reload only ever follows the compare */
            TEST_ASSERT_EQUAL_INT(HW_TIM_EV_CC2, prev);
        } else if (e == HW_TIM_EV_UPDATE) {
            updates++;
            slot_pulse = hw_tim_active();
            in_slot = 1;
        } else if (e == HW_TIM_EV_TERMINAL) {
            terminal++;
        } else {
            idle++;
            break;
        }
        prev = e;
    }
    /* one slot-end compare per fed slot, and one reload right behind it */
    TEST_ASSERT_EQUAL_UINT32(feeds, cc2);
    TEST_ASSERT_EQUAL_UINT32(feeds, feed);
    /* one slot boundary between slots, the last overflow is the terminal */
    TEST_ASSERT_EQUAL_UINT32(slots - 1u, updates);
    TEST_ASSERT_EQUAL_UINT32(1u, terminal);
    TEST_ASSERT_EQUAL_UINT32(1u, idle);
    /* the operation is complete exactly at the terminal update */
    TEST_ASSERT_BITS_HIGH(TIM_SR_UIF, mock_tim1.SR);
    TEST_ASSERT_BITS_LOW(TIM_CR1_CEN, mock_tim1.CR1);
}

static void run_contract_row(const dma_contract_row_t* row) {
    /* Every row starts from a clean mock and a clean driver.
     *
     * The rows used to run back to back on shared state, and that hides
     * defects: the request routing is sticky (the DMAMUX channel keeps the
     * request id the previous operation programmed), so a row that forgot
     * to program its routing saw the previous row's value and passed. The
     * routing assertion only means something against a freshly reset mux.
     */
    hw_reset_all();
    ds18b20_test_register_buffers();
    ds18b20_test_reset_ctx();
    ds18b20_test_reset_search();
    ds18b20_test_reset_resolution();
    ds18b20_test_reset_txn();

    row->schedule();

    TEST_ASSERT_EQUAL_UINT32(row->exp_rcr, (uint32_t)(mock_tim1.RCR & 0xFFu));

    /* feed channel: exact CMAR/CPAR/CNDTR, required CCR bits */
    if (row->exp_feed_cmar != 0u) {
#if defined(STM32H5)
        /* GPDMA SAR/DAR are direction-relative: feed memory in CSAR. */
        TEST_ASSERT_EQUAL_UINT32((uint32_t)row->exp_feed_cmar, mock_feed_ch.CSAR);
        TEST_ASSERT_EQUAL_UINT32((uint32_t)row->exp_feed_cpar, mock_feed_ch.CDAR);
#else
        TEST_ASSERT_EQUAL_UINT32((uint32_t)row->exp_feed_cmar, mock_feed_ch.CMAR);
        TEST_ASSERT_EQUAL_UINT32((uint32_t)row->exp_feed_cpar, mock_feed_ch.CPAR);
#endif
        TEST_ASSERT_EQUAL_UINT32(row->exp_feed_cndtr, mock_feed_ch.CNDTR);
        TEST_ASSERT_BITS_HIGH(row->exp_feed_ccr_hi, mock_feed_ch.CCR);
        TEST_ASSERT_BITS_LOW(row->exp_feed_ccr_lo, mock_feed_ch.CCR);
        /* which buffer the driver actually pointed the feed at */
        assert_dma_buffer("feed source", (const void*)row->exp_feed_cmar,
                          hw_feed_source());
        /* the peripheral request this channel must carry */
        TEST_ASSERT_EQUAL_UINT32(ledger_feed_request(), route_feed_id());
    } else {
        TEST_ASSERT_EQUAL_UINT32(0u, mock_feed_ch.CNDTR);
        TEST_ASSERT_BITS_LOW(DMA_CCR_EN, mock_feed_ch.CCR);
    }

    /* capture channel */
    if (row->exp_cap_cmar != 0u) {
#if defined(STM32H5)
        /* GPDMA SAR/DAR are direction-relative: capture memory in CDAR. */
        TEST_ASSERT_EQUAL_UINT32((uint32_t)row->exp_cap_cmar, mock_dma1_ch4.CDAR);
        TEST_ASSERT_EQUAL_UINT32((uint32_t)row->exp_cap_cpar, mock_dma1_ch4.CSAR);
#else
        TEST_ASSERT_EQUAL_UINT32((uint32_t)row->exp_cap_cmar, mock_dma1_ch4.CMAR);
        TEST_ASSERT_EQUAL_UINT32((uint32_t)row->exp_cap_cpar, mock_dma1_ch4.CPAR);
#endif
        /* exp_cap_cndtr is a transfer count; H5 GPDMA CBR1.BNDT counts
         * source bytes (halfword CCR4 source = 2 per transfer). */
        TEST_ASSERT_EQUAL_UINT32(MOCK_CAP_CNDTR(row->exp_cap_cndtr), mock_dma1_ch4.CNDTR);
        TEST_ASSERT_BITS_HIGH(row->exp_cap_ccr_hi, mock_dma1_ch4.CCR);
        TEST_ASSERT_BITS_LOW(row->exp_cap_ccr_lo, mock_dma1_ch4.CCR);
        /* which buffer the driver actually pointed the capture at */
        assert_dma_buffer("capture destination", (const void*)row->exp_cap_cmar,
                          hw_capture_destination());
        /* the peripheral request this channel must carry */
        TEST_ASSERT_EQUAL_UINT32(ledger_cap_request(), route_cap_id());
    } else {
        TEST_ASSERT_EQUAL_UINT32(0u, mock_dma1_ch4.CNDTR);
        TEST_ASSERT_BITS_LOW(DMA_CCR_EN, mock_dma1_ch4.CCR);
    }

    /* the DMA-enable bits this operation must NOT use */
    TEST_ASSERT_BITS_LOW(row->exp_dier_lo, mock_tim1.DIER);

    uint32_t slots = (uint32_t)(mock_tim1.RCR & 0xFFu) + 1u;

    /* The temporal shape of the operation, walked on the model: the CCR3
     * reloads follow the slot-end compares, the slot boundaries fall where
     * the repetition count says, and the operation ends on the terminal
     * update. The walk *finishes* the operation, so the accounting run
     * below re-arms it. */
    assert_temporal_contract(slots, row->exp_feed_xfer);
    row->schedule();

    /* run the timer to its terminal update and verify the accounting:
       every scheduled transfer fired, everything drained */
    TEST_ASSERT_TRUE(mock_tim1.CR1 & TIM_CR1_CEN);
    TEST_ASSERT_TRUE(hw_run_until_uif(slots));
    TEST_ASSERT_EQUAL_UINT32(0u, mock_feed_ch.CNDTR);
    TEST_ASSERT_BITS_LOW(DMA_CCR_EN, mock_feed_ch.CCR);
    TEST_ASSERT_EQUAL_UINT32(0u, mock_dma1_ch4.CNDTR);
    TEST_ASSERT_BITS_LOW(DMA_CCR_EN, mock_dma1_ch4.CCR);
    TEST_ASSERT_EQUAL_UINT32(row->exp_feed_xfer, hw_ccr3_feed_log()->total);
    TEST_ASSERT_EQUAL_UINT32(row->exp_cap_xfer, hw_capture_count());
}

/* Port-unification vocabulary: pin the OW_PORT_DMA_CR_* values per target,
 * so a wrong default or override breaks here even before any body migrates
 * onto them.  The table above stays the end-to-end guard; this pins the
 * discriminating bits (PSIZE present/absent on 8-bit captures, MSIZE on
 * halfword memories, CHSEL on F4 streams). */
void test_dma_cr_value_macros(void) {
#if defined(STM32H5)
    /* GPDMA CTR1 templates (transfer-register-1 words, not CCR): capture
     * reads the halfword CCR4 into incrementing memory (16- or 8-bit
     * cells), feed reads incrementing byte memory into the fixed halfword
     * CCR3. */
    TEST_ASSERT_EQUAL_UINT32(
        (uint32_t)((1u << DMA_CTR1_SDW_LOG2_Pos) | (1u << DMA_CTR1_DDW_LOG2_Pos) |
                   (1u << DMA_CTR1_DINC_Pos)),
        (uint32_t)OW_PORT_DMA_CR_RX16);
    TEST_ASSERT_EQUAL_UINT32(
        (uint32_t)((1u << DMA_CTR1_SDW_LOG2_Pos) | (1u << DMA_CTR1_DINC_Pos)),
        (uint32_t)OW_PORT_DMA_CR_RX8);
    TEST_ASSERT_EQUAL_UINT32(0u, (uint32_t)(OW_PORT_DMA_CR_RX8 &
                                            (1u << DMA_CTR1_DDW_LOG2_Pos)));
    TEST_ASSERT_EQUAL_UINT32(
        (uint32_t)((1u << DMA_CTR1_SINC_Pos) | (1u << DMA_CTR1_DDW_LOG2_Pos)),
        (uint32_t)OW_PORT_DMA_CR_TX);
    /* REQSEL routing (no DMAMUX on H5): feed <- TIM1_CH2, capture <-
     * TIM1_CH4. Double-sourced from the H5 RM mapping table and CubeH5. */
    TEST_ASSERT_EQUAL_UINT32(59u, (uint32_t)OW_PORT_GPDMA_REQ_TIM1_CC2);
    TEST_ASSERT_EQUAL_UINT32(61u, (uint32_t)OW_PORT_GPDMA_REQ_TIM1_CH4);
#elif defined(STM32F4)
    TEST_ASSERT_EQUAL_UINT32(
        (uint32_t)(DMA_SxCR_MINC | DMA_SxCR_PSIZE_0 | DMA_SxCR_MSIZE_0 |
                   (6u << DMA_SxCR_CHSEL_Pos) | DMA_SxCR_EN),
        (uint32_t)OW_PORT_DMA_CR_RX16);
    TEST_ASSERT_EQUAL_UINT32(
        (uint32_t)(DMA_SxCR_MINC |
                   (6u << DMA_SxCR_CHSEL_Pos) | DMA_SxCR_EN),
        (uint32_t)OW_PORT_DMA_CR_RX8);
    TEST_ASSERT_EQUAL_UINT32(0u, (uint32_t)(OW_PORT_DMA_CR_RX8 & DMA_SxCR_PSIZE_0));
    TEST_ASSERT_EQUAL_UINT32(
        (uint32_t)(DMA_SxCR_DIR_0 | DMA_SxCR_MINC | DMA_SxCR_PSIZE_0 |
                   DMA_SxCR_MSIZE_0 |
                   (6u << DMA_SxCR_CHSEL_Pos) | DMA_SxCR_EN),
        (uint32_t)OW_PORT_DMA_CR_TX);
#else
    TEST_ASSERT_EQUAL_UINT32(
        (uint32_t)(DMA_CCR_MINC | DMA_CCR_PSIZE_0 | DMA_CCR_MSIZE_0 | DMA_CCR_EN),
        (uint32_t)OW_PORT_DMA_CR_RX16);
    TEST_ASSERT_EQUAL_UINT32(
        (uint32_t)(DMA_CCR_MINC | DMA_CCR_PSIZE_0 | DMA_CCR_EN),
        (uint32_t)OW_PORT_DMA_CR_RX8);
    TEST_ASSERT_EQUAL_UINT32(
        (uint32_t)(DMA_CCR_DIR | DMA_CCR_MINC | DMA_CCR_PSIZE_0 | DMA_CCR_EN),
        (uint32_t)OW_PORT_DMA_CR_TX);
#endif
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

/* The ledger must list this family, and what it lists must be what the
 * family actually does: the request ids are read back from the live
 * registers after a real operation is armed. A family that is not on the
 * list fails here - a new port has to say what it proves - and a row that
 * no longer matches the hardware fails with it. */
void test_dma_guarantee_ledger(void) {
    const dma_ledger_row_t* row = ledger_row();
    if (row == NULL) {
        printf("FAIL %s:%d  family '%s' is not on the DMA guarantee ledger - "
               "add it with the guarantees it holds\n",
               __FILE__, __LINE__, CURRENT_FAMILY);
        unity_failures++;
        return;
    }

    /* Arm a write and read the request the feed channel carries back. */
    {
        ow_pulse_t pulses[ONEWIRE_BITS_PER_BYTE + 1];
        hw_reset_all();
        for (uint16_t i = 0; i < ONEWIRE_BITS_PER_BYTE; i++) {
            pulses[i] = (ow_pulse_t)ONEWIRE_ONE_PULSE;
        }
        pulses[ONEWIRE_BITS_PER_BYTE] = 0u;
        hw_register_buf(&pulses[1]);
        onewire_write_slots(pulses, ONEWIRE_BITS_PER_BYTE);
        TEST_ASSERT_EQUAL_UINT32(row->feed_request, hw_feed_request());
    }

    /* Arm a capture and read the request the capture channel carries back. */
    {
        static uint16_t capture[OW_PORT_CAPTURE_BUF_SIZE];
        hw_reset_all();
        hw_register_buf(capture);
        onewire_reset(capture);
        TEST_ASSERT_EQUAL_UINT32(row->cap_request, hw_capture_request());
    }

    /* A family with a fixed request map programs nothing, so the live value
     * is 0 there and the ledger says 0 - the equality above is the whole
     * check. A family with a mux must program its request, or the routing
     * guarantee would be vacuous while claiming to be live: the equality
     * catches that too, because the live value would stay 0. */
}

void run_test_dma_contract(void) {
    TEST_RUN(test_dma_cr_value_macros);
    TEST_RUN(test_dma_guarantee_ledger);
    TEST_RUN(test_dma_contract_table);
}
