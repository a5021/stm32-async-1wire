/* ============================================================
 *  test_port_init_contract.c - clock / bus-pin setup contract
 *
 *  test_dma_contract.c pins what the driver programs into TIM1 and the DMA
 *  channels per operation. Nothing pinned what ow_port_init() and
 *  ow_port_set_pin_mode() program once, at setup: the RCC clock gates, the
 *  bus-pin configuration and (on G0) the SYSCFG pad remap. Those bodies are
 *  the ones being consolidated into a shared port core, and they are exactly
 *  what a host mock cannot be trusted to police on its own - see the note on
 *  the pin numbers below.
 *
 *  So this states the setup contract the way test_dma_contract.c states the
 *  per-operation one: exact expected register values, per family, *captured
 *  from the current backends* rather than re-derived by reading them. Capture
 *  them again with
 *
 *      make test TEST_EXTRA_FLAG=-DOW_PORT_INIT_CAPTURE
 *
 *  which prints the rows below instead of asserting, and update the table when
 *  a change to the setup is intended.
 *
 *  Reading the numbers:
 *    psc  - 72/48/64/168 MHz system clock to a 1 us tick, so SYSCLK_MHZ - 1.
 *           This is the row that makes the CHIP_SYSCLK_MHZ-vs-header-default
 *           mismatch (now guarded by test_sysclk_fallback.c) visible: a wrong
 *           clock shows up here as a wrong prescaler.
 *    mode - F1 uses the legacy GPIO_CRH_MODE10/CNF10 field, the others the
 *           modern MODER two-bit field. F4 additionally puts PA11 into output
 *           mode in the same call, because its LA marker rides PA11.
 *    af   - the alternate function number for TIM1_CH3 on the bus pin. It is
 *           genuinely 2 on F0/G0 and 1 on F4; that is silicon, not a typo.
 *    remap- G0 only: PA11_RMP | PA12_RMP, because the TSSOP20 does not bond
 *           out PA9/PA10 and the bus runs on the remapped pads.
 *  Pin numbers are PA10 everywhere, which the host mocks now agree on -
 *  before tests/check_mock_headers.sh existed, the MODER10 field in the f0, g0
 *  and f4 mocks held PA11's bits, so "the bus pin is in AF mode" was asserting
 *  about the wrong pin in three of the four backends.
 * ============================================================ */

#include "ow_port.h"
#include "mock_target.h"
#include "hw_model.h"
#include "unity.h"

#include <stdio.h>

/* One flat view of the setup state, so the table is written once against a
 * family-independent shape instead of four times against register names that
 * only exist on some of them. */
typedef struct {
    uint32_t clk;       /* GPIOA + TIM1 + DMA clock gates, packed */
    uint32_t psc;       /* TIM1 prescaler */
    uint32_t bdtr;      /* TIM1 BDTR */
    uint32_t pin_mode;  /* CRH on F1, MODER elsewhere */
    uint32_t pin_otype; /* OTYPER where the family has one, else 0 */
    uint32_t pin_af;    /* AFR[1] where the family has one, else 0 */
    uint32_t pin_speed; /* OSPEEDR where the family has one, else 0 */
    uint32_t remap;     /* SYSCFG pad remap where the family has one, else 0 */
} port_setup_t;

static port_setup_t snapshot(void) {
    port_setup_t s;
    s.clk = 0u;
    s.psc = (uint32_t)mock_tim1.PSC;
    s.bdtr = (uint32_t)mock_tim1.BDTR;
    s.pin_mode = 0u;
    s.pin_otype = 0u;
    s.pin_af = 0u;
    s.pin_speed = 0u;
    s.remap = 0u;

#if defined(OW_PORT_FAMILY_F1)
    s.clk = (uint32_t)mock_rcc.APB2ENR | ((uint32_t)mock_rcc.AHBENR << 16);
    s.pin_mode = (uint32_t)mock_gpioa.CRH;
#elif defined(OW_PORT_FAMILY_F0)
    s.clk = (uint32_t)mock_rcc.APB2ENR | ((uint32_t)mock_rcc.AHBENR << 16);
    s.pin_mode = (uint32_t)mock_gpioa.MODER;
    s.pin_otype = (uint32_t)mock_gpioa.OTYPER;
    s.pin_af = (uint32_t)mock_gpioa.AFR[1];
    s.pin_speed = (uint32_t)mock_gpioa.OSPEEDR;
#elif defined(OW_PORT_FAMILY_G0)
    s.clk = (uint32_t)mock_rcc.APBENR2 |
            ((uint32_t)mock_rcc.IOPENR << 16) |
            ((uint32_t)mock_rcc.AHBENR << 32);
    s.pin_mode = (uint32_t)mock_gpioa.MODER;
    s.pin_otype = (uint32_t)mock_gpioa.OTYPER;
    s.pin_af = (uint32_t)mock_gpioa.AFR[1];
    s.pin_speed = (uint32_t)mock_gpioa.OSPEEDR;
    s.remap = (uint32_t)mock_syscfg.CFGR1;
#else /* OW_PORT_FAMILY_F4 */
    s.clk = (uint32_t)mock_rcc.APB2ENR | ((uint32_t)mock_rcc.AHB1ENR << 16);
    s.pin_mode = (uint32_t)mock_gpioa.MODER;
    s.pin_otype = (uint32_t)mock_gpioa.OTYPER;
    s.pin_af = (uint32_t)mock_gpioa.AFR[1];
    s.pin_speed = (uint32_t)mock_gpioa.OSPEEDR;
#endif
    return s;
}

typedef enum { SETUP_INIT, SETUP_PUSH_PULL, SETUP_OPEN_DRAIN } setup_variant_t;

typedef struct {
    const char* name;
    uint32_t clk;
    uint32_t psc;
    uint32_t bdtr;
    uint32_t pin_mode;
    uint32_t pin_otype;
    uint32_t pin_af;
    uint32_t pin_speed;
    uint32_t remap;
} setup_row_t;

/* Captured from the backends, not derived from them. See the header. */
#if defined(OW_PORT_FAMILY_F1)
/* clk packs APB2ENR in the low half (IOPAEN | TIM1EN) and AHBENR in the high
 * half (DMA1EN). mode is CRH: MODE10 = 0b10 (2 MHz) with CNF10 = 0b11
 * (alternate-function open-drain) after init, and CNF10 = 0b10 (AF push-pull)
 * in push-pull mode. */
static const setup_row_t k_setup[] = {
    { "init", 0x00010804u, 0x0047u, 0x8000u, 0x00000E00u, 0u, 0u, 0u, 0u },
    { "push_pull", 0x00010804u, 0x0047u, 0x8000u, 0x00000A00u, 0u, 0u, 0u, 0u },
    { "open_drain", 0x00010804u, 0x0047u, 0x8000u, 0x00000E00u, 0u, 0u, 0u, 0u },
};
#elif defined(OW_PORT_FAMILY_F0)
/* mode is PA10 = 0b10 in MODER (alternate function); otype 0x400 = PA10
 * open-drain, cleared for push-pull; af 0x200 = AF2 for TIM1_CH3;
 * speed 0x300000 = PA10 at the fastest setting. */
static const setup_row_t k_setup[] = {
    { "init", 0x00010800u, 0x002Fu, 0x8000u, 0x00200000u, 0x00000400u, 0x00000200u, 0x00300000u, 0u },
    { "push_pull", 0x00010800u, 0x002Fu, 0x8000u, 0x00200000u, 0u, 0x00000200u, 0x00300000u, 0u },
    { "open_drain", 0x00010800u, 0x002Fu, 0x8000u, 0x00200000u, 0x00000400u, 0x00000200u, 0x00300000u, 0u },
};
#elif defined(OW_PORT_FAMILY_G0)
/* As F0 for the bus pin, plus remap 0x18 = PA11_RMP | PA12_RMP: the G031 TSSOP20
 * does not bond out PA9/PA10, so the bus runs on the remapped pads. clk packs
 * APBENR2 low, IOPENR middle, AHBENR high. */
static const setup_row_t k_setup[] = {
    { "init", 0x00010801u, 0x003Fu, 0x8000u, 0x00200000u, 0x00000400u, 0x00000200u, 0x00300000u, 0x00000018u },
    { "push_pull", 0x00010801u, 0x003Fu, 0x8000u, 0x00200000u, 0u, 0x00000200u, 0x00300000u, 0x00000018u },
    { "open_drain", 0x00010801u, 0x003Fu, 0x8000u, 0x00200000u, 0x00000400u, 0x00000200u, 0x00300000u, 0x00000018u },
};
#else /* OW_PORT_FAMILY_F4 */
/* mode additionally sets PA11 to 0b01 (output), because this backend rides the
 * LA marker on PA11. af is 0x100 = AF1 here, against AF2 on F0/G0 - that is the
 * silicon, not an inconsistency. */
static const setup_row_t k_setup[] = {
    { "init", 0x00010001u, 0x00A7u, 0x8000u, 0x00600000u, 0x00000400u, 0x00000100u, 0x00300000u, 0u },
    { "push_pull", 0x00010001u, 0x00A7u, 0x8000u, 0x00600000u, 0u, 0x00000100u, 0x00300000u, 0u },
    { "open_drain", 0x00010001u, 0x00A7u, 0x8000u, 0x00600000u, 0x00000400u, 0x00000100u, 0x00300000u, 0u },
};
#endif

static void apply(setup_variant_t v) {
    hw_reset_all();
    ow_port_init();
    if (v == SETUP_PUSH_PULL) {
        ow_port_set_pin_mode(1u);
    } else if (v == SETUP_OPEN_DRAIN) {
        ow_port_set_pin_mode(0u);
    }
}

static void check(setup_variant_t v) {
    const setup_row_t* row = &k_setup[v];
    port_setup_t s;

    apply(v);
    s = snapshot();

    TEST_ASSERT_EQUAL_HEX32_MESSAGE(row->clk, s.clk, "clock gates");
    TEST_ASSERT_EQUAL_HEX32_MESSAGE(row->psc, s.psc, "TIM1 prescaler");
    TEST_ASSERT_EQUAL_HEX32_MESSAGE(row->bdtr, s.bdtr, "TIM1 BDTR");
    TEST_ASSERT_EQUAL_HEX32_MESSAGE(row->pin_mode, s.pin_mode, "bus pin mode");
    TEST_ASSERT_EQUAL_HEX32_MESSAGE(row->pin_otype, s.pin_otype, "bus pin output type");
    TEST_ASSERT_EQUAL_HEX32_MESSAGE(row->pin_af, s.pin_af, "bus pin alternate function");
    TEST_ASSERT_EQUAL_HEX32_MESSAGE(row->pin_speed, s.pin_speed, "bus pin drive strength");
    TEST_ASSERT_EQUAL_HEX32_MESSAGE(row->remap, s.remap, "SYSCFG pad remap");
}

static void test_port_setup_after_init(void) {
    check(SETUP_INIT);
}

static void test_port_setup_push_pull(void) {
    check(SETUP_PUSH_PULL);
}

static void test_port_setup_open_drain(void) {
    check(SETUP_OPEN_DRAIN);
}

#if defined(OW_PORT_INIT_CAPTURE)
/* Capture mode: print the rows instead of asserting, so the table can be
 * regenerated from a backend. Never compiled in CI. */
static void capture(void) {
    static const char* tags[3] = { "init", "push_pull", "open_drain" };
    for (uint32_t i = 0u; i < 3u; i++) {
        port_setup_t s;
        apply((setup_variant_t)i);
        s = snapshot();
        printf("CAPTURE %-11s clk=0x%08lx psc=0x%04lx bdtr=0x%04lx mode=0x%08lx "
               "otype=0x%08lx af=0x%08lx speed=0x%08lx remap=0x%08lx\n",
               tags[i], (unsigned long)s.clk, (unsigned long)s.psc, (unsigned long)s.bdtr,
               (unsigned long)s.pin_mode, (unsigned long)s.pin_otype, (unsigned long)s.pin_af,
               (unsigned long)s.pin_speed, (unsigned long)s.remap);
    }
}
#endif

void run_test_port_init_contract(void) {
#if defined(OW_PORT_INIT_CAPTURE)
    capture();
    return;
#endif
    TEST_RUN(test_port_setup_after_init);
    TEST_RUN(test_port_setup_push_pull);
    TEST_RUN(test_port_setup_open_drain);
}
