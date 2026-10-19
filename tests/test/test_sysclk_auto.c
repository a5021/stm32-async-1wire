/* ============================================================
 *  test_sysclk_auto.c - SystemCoreClock auto-path host tests
 *
 *  The only TU built WITHOUT -DOW_PORT_SYSCLK_MHZ (a miswired command
 *  line fails loudly on the #error below): exercises the port core's
 *  runtime clock path. Defines its own SystemCoreClock, drives
 *  ow_port_init() through every supported frequency and checks the PSC,
 *  checks the runtime IC4F helper against the documented tiers, and
 *  checks one full CCMR2 composition through ow_port_capture().
 *  Built per OW_TARGET by the Makefile (TEST_AUTO_EXE), run by `make test`.
 * ============================================================ */

#ifdef OW_PORT_SYSCLK_MHZ
#error "test_sysclk_auto: must be built WITHOUT -DOW_PORT_SYSCLK_MHZ (it exercises the SystemCoreClock auto-path)"
#endif

#include "hw_model.h"
#include "onewire.h"
#include "ow_port.h"
#include "unity.h"

uint32_t SystemCoreClock = 72000000u;

int unity_failures = 0;

void setUp(void) {
    hw_reset_all();
}

void tearDown(void) {
    /* nothing to clean up */
}

typedef struct {
    uint32_t hz;
    uint16_t psc;
    uint32_t ic4f;
} sysclk_auto_case_t;

/* Every supported clock: prescaler plus the documented IC4F tier
 * (test_timing.c::test_ic4f_matches_the_documented_tier pins the same
 * tiers for the compile-time ladder). */
static const sysclk_auto_case_t sysclk_auto_cases[] = {
    {8000000u, 7u, TIM_CCMR2_IC4F_1},
    {16000000u, 15u, (uint32_t)(TIM_CCMR2_IC4F_0 | TIM_CCMR2_IC4F_1)},
    {48000000u, 47u, (uint32_t)(TIM_CCMR2_IC4F_0 | TIM_CCMR2_IC4F_1 | TIM_CCMR2_IC4F_2)},
    {64000000u, 63u, (uint32_t)(TIM_CCMR2_IC4F_0 | TIM_CCMR2_IC4F_1 | TIM_CCMR2_IC4F_2)},
    {72000000u, 71u, (uint32_t)(TIM_CCMR2_IC4F_0 | TIM_CCMR2_IC4F_1 | TIM_CCMR2_IC4F_2)},
    {84000000u, 83u, TIM_CCMR2_IC4F_3},
    {168000000u, 167u, TIM_CCMR2_IC4F_3},
    {170000000u, 169u, (uint32_t)(TIM_CCMR2_IC4F_0 | TIM_CCMR2_IC4F_1 | TIM_CCMR2_IC4F_3)},
    {180000000u, 179u, (uint32_t)(TIM_CCMR2_IC4F_0 | TIM_CCMR2_IC4F_1 | TIM_CCMR2_IC4F_3)},
    {250000000u, 249u, (uint32_t)(TIM_CCMR2_IC4F_0 | TIM_CCMR2_IC4F_1 | TIM_CCMR2_IC4F_3)},
};

static void test_sysclk_auto_table(void) {
    unsigned i;
    for (i = 0; i < sizeof(sysclk_auto_cases) / sizeof(sysclk_auto_cases[0]); i++) {
        SystemCoreClock = sysclk_auto_cases[i].hz;
        hw_reset_all();
        ow_port_init();
        TEST_ASSERT_EQUAL_UINT16(sysclk_auto_cases[i].psc, (uint16_t)mock_tim1.PSC);
        TEST_ASSERT_EQUAL_UINT32(sysclk_auto_cases[i].ic4f, ow_port_ic4f_mask_from_hz(sysclk_auto_cases[i].hz));
    }
}

static void test_sysclk_auto_ccmr2_composition(void) {
    static volatile uint16_t cap_buf[2];
    /* 168MHz tier through the real capture arm: OC3 PWM mode 2, CC4 on TI3,
     * IC4F_3, preload bit on. */
    SystemCoreClock = 168000000u;
    hw_reset_all();
    ow_port_init();
    ow_port_capture((volatile void*)cap_buf, 2, 16);
    TEST_ASSERT_EQUAL_UINT32((uint32_t)(TIM_CCMR2_OC3M_0 | TIM_CCMR2_OC3M_1 | TIM_CCMR2_OC3M_2 | TIM_CCMR2_CC4S_1 | TIM_CCMR2_IC4F_3 | TIM_CCMR2_OC3PE),
                             (uint32_t)mock_tim1.CCMR2);
    /* 72MHz tier: three low filter bits, preload bit still on. */
    SystemCoreClock = 72000000u;
    hw_reset_all();
    ow_port_init();
    ow_port_capture((volatile void*)cap_buf, 2, 16);
    TEST_ASSERT_EQUAL_UINT32((uint32_t)(TIM_CCMR2_OC3M_0 | TIM_CCMR2_OC3M_1 | TIM_CCMR2_OC3M_2 | TIM_CCMR2_CC4S_1 | TIM_CCMR2_IC4F_0 | TIM_CCMR2_IC4F_1 | TIM_CCMR2_IC4F_2 | TIM_CCMR2_OC3PE),
                             (uint32_t)mock_tim1.CCMR2);
}

int main(void) {
    test_sysclk_auto_table();
    test_sysclk_auto_ccmr2_composition();
    printf("%s: %d failure(s)\n", unity_failures ? "FAIL" : "PASS", unity_failures);
    return unity_failures ? 1 : 0;
}
