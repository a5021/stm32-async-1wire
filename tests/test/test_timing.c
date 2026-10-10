/* ============================================================
 *  test_timing.c - Timing Register Regression Tests
 *
 *  Verifies the actual register values each bus operation programs
 *  into TIM1 (ARR/RCR/CCR2/CCR3) against the DS18B20 timing spec.
 *  Unlike a constant re-check, this locks the real driver output.
 * ============================================================ */

#include "ds18b20.h"
#include "ds18b20_test_access.h"
#include "hw_model.h"
#include "mock_target.h"
#include "onewire.h"
#include "unity.h"
#if defined(OW_PORT_TARGET_F4) || defined(OW_PORT_TARGET_G4) || defined(OW_PORT_TARGET_H5)
#include "app.h" /* configure_system_clock() test surface */
#endif

void test_timing_reset_programs_timeout_and_pulse(void) {
    hw_reset_all();
    test_bus_reset();
    /* RESET_TIMEOUT = 2 * RESET_PULSE_MIN = 960µs, pulse = 480µs */
    TEST_ASSERT_EQUAL_UINT16(960, (uint16_t)mock_tim1.ARR);
    /* one slot: RCR = 0 */
    TEST_ASSERT_EQUAL_UINT32(0, mock_tim1.RCR);
    /* capture ops preload the output CCR with 0 via OCxPE (hardware bus release) */
    TEST_ASSERT_TRUE(MOCK_TIM_OUT_CCMR & MOCK_TIM_OUT_PE);
    TEST_ASSERT_EQUAL_UINT16(0, (uint16_t)MOCK_TIM_OUT_CCR);
}

void test_timing_command_programs_slot_period(void) {
    hw_reset_all();
    ow_pulse_t cmd[17];
    for (int i = 0; i < 16; i++) {
        cmd[i] = (i & 1) ? ONEWIRE_ONE_PULSE : ONEWIRE_ZERO_PULSE;
    }
    cmd[16] = 0;
    test_bus_send_command_n(cmd, 16);
    /* ARR = one_pulse + zero_pulse + guard_band */
    TEST_ASSERT_EQUAL_UINT16((uint16_t)(ONEWIRE_ONE_PULSE + ONEWIRE_ZERO_PULSE + ONEWIRE_GUARD_BAND),
                             (uint16_t)mock_tim1.ARR);
    /* RCR = slots - 1 */
    TEST_ASSERT_EQUAL_UINT32(15, mock_tim1.RCR);
    /* the slot-end marker compare triggers the DMA reload at one_pulse + zero_pulse */
    TEST_ASSERT_EQUAL_UINT32((uint32_t)(ONEWIRE_ONE_PULSE + ONEWIRE_ZERO_PULSE), MOCK_TIM_MARKER_CCR);
}

void test_timing_read_programs_72_slots(void) {
    hw_reset_all();
    test_bus_read_data();
    /* 72 data slots: RCR = 71 */
    TEST_ASSERT_EQUAL_UINT32(71, mock_tim1.RCR);
    TEST_ASSERT_EQUAL_UINT16((uint16_t)(ONEWIRE_ONE_PULSE + ONEWIRE_ZERO_PULSE + ONEWIRE_GUARD_BAND),
                             (uint16_t)mock_tim1.ARR);
    /* capture ops preload the output CCR with 0 via OCxPE (hardware bus release) */
    TEST_ASSERT_TRUE(MOCK_TIM_OUT_CCMR & MOCK_TIM_OUT_PE);
    TEST_ASSERT_EQUAL_UINT32(0, MOCK_TIM_OUT_CCR);
}

void test_timing_wait_conversion_750ms(void) {
    hw_reset_all();
    test_bus_wait_conversion();
    /* PAUSE_750MS = 62500 ticks * 12 periods * 1µs = 750ms */
    TEST_ASSERT_EQUAL_UINT32(62500, mock_tim1.ARR);
    TEST_ASSERT_EQUAL_UINT32(11, mock_tim1.RCR);
}

void test_timing_long_wait_arr_rcr(void) {
    hw_reset_all();
    /* Generic long idle-HIGH wait: 62500 ticks * 80 periods * 1us = 5s. The
     * driver no longer has a cycle-pause knob, so the ARR/RCR math of a long
     * timer pass is covered through the raw onewire_start_timer() contract. */
    test_bus_start_timer(62500, 79);
    TEST_ASSERT_EQUAL_UINT32(62500, mock_tim1.ARR);
    TEST_ASSERT_EQUAL_UINT32(79, mock_tim1.RCR);
}

void test_timing_temperature_formula(void) {
    /* raw = 0x0164 = 356 -> 22.25°C -> 223 tenths (round-half-away-from-zero) */
    TEST_ASSERT_EQUAL_INT(223, (int)(((int32_t)0x0164 * 10 + 8) / 16));
    /* raw = 0x0000 -> 0 */
    TEST_ASSERT_EQUAL_INT(0, (int)(((int32_t)0x0000 * 10 + 8) / 16));
}

/*-------------------------------------------------------------
 *  Test: APB prescaler for TIM1 bus must be /1.
 *
 *  STM32 rule: if APB prescaler != 1, TIM clock = 2 × PCLK.
 *  This doubles the tick rate and breaks every µs-based timing
 *  constant (slots, reset pulse, conversion wait).
 *
 *  configure_system_clock() must NOT divide the APB bus feeding
 *  TIM1.  This test catches the mistake at the register level.
 *
 *  F0: TIM1 on APB2, PPRE defaults /1.  ✓
 *  F1: TIM1 on APB2, PPRE2 stays /1 (PPRE1=/2 is OK — different bus).  ✓
 *  G0: single APB bus, PPRE defaults /1.  ✓
 * -----------------------------------------------------------*/
void test_apb_prescaler_div1_for_tim1(void) {
    hw_reset_all();
    ds18b20_init();
    /* PSC must equal SYSCLK_MHZ - 1 (1µs tick at full SYSCLK) */
    TEST_ASSERT_EQUAL_UINT16(OW_PORT_SYSCLK_MHZ - 1, (uint16_t)mock_tim1.PSC);

#if defined(OW_PORT_TARGET_F1)
    /* F1: TIM1 on APB2 → PPRE2 must be /1 (field = 0) */
    TEST_ASSERT_EQUAL_UINT32(0, mock_rcc.CFGR & RCC_CFGR_PPRE2_Msk);
#elif defined(OW_PORT_TARGET_F4)
    /* Preset the ready/status flags configure_system_clock() waits on: the mock
     * does not model hardware self-setting of HSERDY/PLLRDY/SWS. */
    mock_rcc.CR = RCC_CR_HSERDY | RCC_CR_PLLRDY;
    mock_rcc.CFGR = RCC_CFGR_SWS_PLL;
#if (OW_PORT_SYSCLK_MHZ) == 180
    /* The F446's 180MHz waits on two more flags, on the regulator instead of the
     * clock tree. The mock does not model the ramp-up either, so they are
     * preset here and the asserts below then check that the code actually
     * *enabled* over-drive - which is the part a rewrite could silently drop,
     * and unlike a missing bit on a GPIO that failure is out-of-spec silicon
     * rather than a wrong pin. */
    mock_pwr.CSR = PWR_CSR_ODRDY | PWR_CSR_ODSWRDY;
#endif
    configure_system_clock();
    TEST_ASSERT_EQUAL_UINT8(1, app_clock_ok());
#if (OW_PORT_SYSCLK_MHZ) == 180
    /* PWR sits on APB1, so its clock has to be on before PWR->CR is writable.
     * Without this the two CR writes are dropped and the part runs 180MHz
     * without over-drive - still boots, just outside its rating. */
    TEST_ASSERT_EQUAL_UINT32(RCC_APB1ENR_PWREN, mock_rcc.APB1ENR & RCC_APB1ENR_PWREN);
    /* Both halves of the RM0390 §5.4.6 sequence, in order: ODEN -> ODRDY, then
     * ODSWEN -> ODSWRDY. Checking only ODEN would pass a build that waited on
     * ODRDY twice and never enabled over-drive switching. */
    TEST_ASSERT_EQUAL_UINT32(PWR_CR_ODEN, mock_pwr.CR & PWR_CR_ODEN);
    TEST_ASSERT_EQUAL_UINT32(PWR_CR_ODSWEN, mock_pwr.CR & PWR_CR_ODSWEN);
    /* 180MHz shares the 168MHz wait-state row (5 WS) but not the APB numbers:
     * the F446 allows APB1 = 45MHz and APB2 = 90MHz, so /4 and /2 are the same
     * divisors the F407 uses and reach 45/90 rather than 42/84. The whole ACR is
     * compared rather than the wait-state field alone: app.c assigns it in one
     * write, so this also pins the prefetch and cache enables, which a masked
     * read would have let drift. (The field mask is 3 bits on the F407 and 4 on
     * the F446, so there is no single mask to read it with anyway.) */
    TEST_ASSERT_EQUAL_UINT32(FLASH_ACR_PRFTEN | FLASH_ACR_ICEN | FLASH_ACR_DCEN |
                                 FLASH_ACR_LATENCY_5WS,
                             mock_flash.ACR);
#endif
    /* APB1 is /4 at 168MHz and at the F446's 180MHz, and /2 at 84MHz - /4 and /2
     * both land PCLK1 on 42MHz while 180MHz's /4 lands it on 45MHz, the F446's
     * own limit - and APB2 is /2 in all three, so TIM1 = 2 x PCLK2 = SYSCLK
     * either way, which is the 1us tick invariant. Derived from the configured
     * clock rather than hardcoded, so the 84MHz F401 and 180MHz F446 host builds
     * check their own values. */
#if (OW_PORT_SYSCLK_MHZ) == 168 || (OW_PORT_SYSCLK_MHZ) == 180
    TEST_ASSERT_EQUAL_UINT32(RCC_CFGR_PPRE1_DIV4, mock_rcc.CFGR & RCC_CFGR_PPRE1_Msk);
#else
    TEST_ASSERT_EQUAL_UINT32(RCC_CFGR_PPRE1_DIV2, mock_rcc.CFGR & RCC_CFGR_PPRE1_Msk);
#endif
    TEST_ASSERT_EQUAL_UINT32(RCC_CFGR_PPRE2_DIV2, mock_rcc.CFGR & RCC_CFGR_PPRE2_Msk);
#if (OW_PORT_SYSCLK_MHZ) == 168 || (OW_PORT_SYSCLK_MHZ) == 84 || (OW_PORT_SYSCLK_MHZ) == 180
    /* The M divider is the one field a wrong crystal breaks silently: PLLM =
     * OW_HSE_MHZ puts the PLL input at 1MHz and PLLN = 2 x SYSCLK sets the VCO.
     * Nothing else in the suite looks at PLLCFGR, which is how an F401 on a
     * 25MHz crystal lost its M=25 and reverted to M=8 without a single test
     * objecting. An 8MHz crystal still has to give M=8/N=336 at 168MHz,
     * M=8/N=168 at 84MHz and M=8/N=360 at 180MHz; a 25MHz board gives
     * M=25/N=168. Field widths: PLLM is 5 bits, PLLN is 9. */
    TEST_ASSERT_EQUAL_UINT32(OW_HSE_MHZ,
                             (mock_rcc.PLLCFGR >> RCC_PLLCFGR_PLLM_Pos) & 0x1Fu);
    TEST_ASSERT_EQUAL_UINT32(
        (OW_PORT_SYSCLK_MHZ) * 2u,
        (mock_rcc.PLLCFGR >> RCC_PLLCFGR_PLLN_Pos) & 0x1FFu);
#endif
#elif defined(OW_PORT_TARGET_F0)
    /* F0: single APB bus — PPRE must be /1 (field = 0) */
    TEST_ASSERT_EQUAL_UINT32(0, mock_rcc.CFGR & RCC_CFGR_PPRE_Msk);
#elif defined(OW_PORT_TARGET_G0)
    /* G0: single APB bus — PPRE must be /1 (field = 0) */
    TEST_ASSERT_EQUAL_UINT32(0, mock_rcc.CFGR & RCC_CFGR_PPRE_Msk);
#elif defined(OW_PORT_TARGET_G4)
    /* Preset the ready/status flags configure_system_clock() waits on: the mock
     * does not model hardware self-setting of HSERDY/PLLRDY. SWS is preset
     * only on the PLL path - the raw-HSI16 path must leave CFGR untouched. */
    mock_rcc.CR = RCC_CR_HSERDY | RCC_CR_PLLRDY;
#if (OW_PORT_SYSCLK_MHZ) == 170
    mock_rcc.CFGR = RCC_CFGR_SWS_PLL;
#endif
    configure_system_clock();
    TEST_ASSERT_EQUAL_UINT8(1, app_clock_ok());
    /* APB1 and APB2 stay /1 at every G4 clock, so TIM1 = PCLK2 = SYSCLK
     * directly - no x2 doubling (the ow_port 1us-tick invariant). */
    TEST_ASSERT_EQUAL_UINT32(0, mock_rcc.CFGR & RCC_CFGR_PPRE1_Msk);
    TEST_ASSERT_EQUAL_UINT32(0, mock_rcc.CFGR & RCC_CFGR_PPRE2_Msk);
#if (OW_PORT_SYSCLK_MHZ) == 170
    /* Range 1 Boost (PWR_CR1.VOS = 00) before the PLL starts: 170MHz is above
     * the 150MHz Range-1 ceiling, and running it there is out of spec. PWR
     * sits on APB1, so its clock has to be on before CR1 is writable -
     * without it the VOS write is dropped and the part runs 170MHz in the
     * reset scale. The mock's VOSF starts clear, so the transition wait
     * passes; asserting the write is what would catch a dropped one. */
    TEST_ASSERT_EQUAL_UINT32(RCC_APB1ENR1_PWREN, mock_rcc.APB1ENR1 & RCC_APB1ENR1_PWREN);
    TEST_ASSERT_EQUAL_UINT32(0u, mock_pwr.CR1 & PWR_CR1_VOS);
    /* The switch write happened (SW was 0 in the preset, now PLL). */
    TEST_ASSERT_EQUAL_UINT32(RCC_CFGR_SW_PLL, mock_rcc.CFGR & RCC_CFGR_SW);
    /* Whole ACR compared: app.c assigns it in one write (prefetch + caches +
     * latency), so this pins all three. */
    TEST_ASSERT_EQUAL_UINT32(FLASH_ACR_PRFTEN | FLASH_ACR_ICEN | FLASH_ACR_DCEN |
                                 FLASH_ACR_LATENCY_8WS,
                             mock_flash.ACR);
    /* M = HSE/2 aims the PLL input at 4MHz (the PLLM field holds divider-1,
     * so an 8MHz crystal needs field=1 to get divider=2 -> 4MHz PLL input).
     * N = SYSCLK/2, R stays /2 (field 0) with PLLREN selecting it. Nothing
     * else in the suite looks at these fields: a wrong crystal breaks M
     * silently (the PLL never locks, or locks marginally below the 2.66MHz
     * input window), which is the F4 M-field lesson applied here. Field
     * widths: PLLM 4 bits, PLLN 7, PLLR 2. The M expectation is a literal,
     * not the code's own formula: asserting the formula proved nothing when
     * the formula itself forgot the minus-1. */
#if (OW_HSE_MHZ) == 8
    TEST_ASSERT_EQUAL_UINT32(1u, (mock_rcc.PLLCFGR >> RCC_PLLCFGR_PLLM_Pos) & 0xFu);
#endif
    TEST_ASSERT_EQUAL_UINT32((OW_PORT_SYSCLK_MHZ) / 2u,
                             (mock_rcc.PLLCFGR >> RCC_PLLCFGR_PLLN_Pos) & 0x7Fu);
    TEST_ASSERT_EQUAL_UINT32(0u, (mock_rcc.PLLCFGR >> RCC_PLLCFGR_PLLR_Pos) & 0x3u);
    TEST_ASSERT_EQUAL_UINT32(RCC_PLLCFGR_PLLREN, mock_rcc.PLLCFGR & RCC_PLLCFGR_PLLREN);
    /* PLLSRC uses the ST selection value (HSE = both bits, HSI16 = bit 1)
     * that stm32g474xx.h and the Cube HAL agree on - not the RM table order,
     * which is why this asserts the value rather than trusting the name. */
    TEST_ASSERT_EQUAL_UINT32(RCC_PLLCFGR_PLLSRC_HSE, mock_rcc.PLLCFGR & RCC_PLLCFGR_PLLSRC);
#else
    /* Raw HSI16: nothing configured - CFGR, PLL and flash latency stay reset. */
    TEST_ASSERT_EQUAL_UINT32(0u, mock_rcc.CFGR);
    TEST_ASSERT_EQUAL_UINT32(0u, mock_rcc.PLLCFGR);
    TEST_ASSERT_EQUAL_UINT32(0u, mock_flash.ACR);
#endif
#elif defined(OW_PORT_TARGET_H5)
    /* HSI-64 bring-up: flash latency + HSI divider only, no waits (HSI is
     * already running), so no failure flag and no ready-flag presetting. */
    configure_system_clock();
    /* 3 WS + prefetch in one write (covers 64MHz in every voltage range). */
    TEST_ASSERT_EQUAL_UINT32(FLASH_ACR_PRFTEN | FLASH_ACR_LATENCY_3WS,
                             mock_flash.ACR);
    /* Divider to /1: reset is /2 (32MHz SYSCLK). */
    TEST_ASSERT_EQUAL_UINT32(0u, mock_rcc.CR & RCC_CR_HSIDIV);
    /* APB prescalers stay at reset /1: TIM1 = PCLK2 = SYSCLK directly. */
    TEST_ASSERT_EQUAL_UINT32(0u, mock_rcc.CFGR2 &
                                     (RCC_CFGR2_HPRE | RCC_CFGR2_PPRE1 | RCC_CFGR2_PPRE2));
#endif
}

/*-------------------------------------------------------------
 *  onewire_search_start() (onewire.c early-return guard).
 *----------------------------------------------------------*/
void test_search_start_ignored_while_running(void) {
    onewire_search_start(NULL, 1, DS18B20_SEARCH_ROM, 0);
    TEST_ASSERT_TRUE(onewire_search_active());
    /* Second start while the search owns the timer must be ignored. */
    onewire_search_start(NULL, 1, DS18B20_SEARCH_ROM, 0);
    TEST_ASSERT_TRUE(onewire_search_active());
    ds18b20_test_reset_search();
}

/*-------------------------------------------------------------
 *  The IC4F bits must be the ones the tier table in ow_port.h
 *  documents, not merely *some* filter.
 *
 *  ICxF is a 4-bit ladder, so almost any wrong value still
 *  configures a plausible-looking filter - it is just a
 *  different time, with no error anywhere. The F446 180MHz
 *  tier is the live example: adding IC4F_2 as well would read
 *  as "bigger filter" but is 0b1111 = fDTS/32 N=8, i.e. 1422ns
 *  rather than the 533ns of IC4F_0|IC4F_1|IC4F_3. So pin the
 *  expected value per clock instead of trusting the comment.
 *-------------------------------------------------------------*/
void test_ic4f_matches_the_documented_tier(void) {
    uint32_t expected;
    hw_reset_all();
    /* A reset goes through ow_port_capture(), which is what programs CCMR2 with
     * the filter - so run a real operation rather than reading a register that
     * nothing has touched. */
    test_bus_reset();

#if (OW_PORT_SYSCLK_MHZ) <= 8
    expected = TIM_CCMR2_IC4F_1; /* fCK_INT, N=4 */
#elif (OW_PORT_SYSCLK_MHZ) <= 16
    expected = TIM_CCMR2_IC4F_0 | TIM_CCMR2_IC4F_1; /* fCK_INT, N=8 */
#elif (OW_PORT_SYSCLK_MHZ) <= 72
    expected = TIM_CCMR2_IC4F_0 | TIM_CCMR2_IC4F_1 | TIM_CCMR2_IC4F_2; /* fDTS/4, N=8 */
#elif (OW_PORT_SYSCLK_MHZ) <= 168
    expected = TIM_CCMR2_IC4F_3; /* fDTS/8, N=6 */
#else
    expected = TIM_CCMR2_IC4F_0 | TIM_CCMR2_IC4F_1 | TIM_CCMR2_IC4F_3; /* fDTS/16, N=6 */
#endif
    TEST_ASSERT_EQUAL_UINT32(expected, mock_tim1.CCMR2 & 0xF000u);
}

#if defined(OW_PORT_TARGET_F4)
/*-------------------------------------------------------------
 *  The console UART divisor must follow the APB prescalers.
 *
 *  This is the check that was missing when 180MHz was added. The
 *  divisor was a per-clock ternary chain with no 180MHz case, so
 *  it fell through to the raw-HSI default and sized BRR for
 *  PCLK2 = 180MHz when the part runs 45/90: BRR came out exactly
 *  2x too large and the console ran at ~57600 baud. A clean
 *  compile, a working UART, and a symptom ("garbage in the
 *  terminal") that reads like a bad cable rather than a wrong
 *  clock.
 *
 *  Asserted on the value rather than on the register, because the
 *  register write lives in hardware_init() - inside
 *  #if !defined(DS18B20_TEST_HARNESS), so no host build has ever
 *  compiled it on any family. That is why the arithmetic moved to
 *  app.h: this is the only reach the host suite has over it, so the
 *  expected numbers are spelled out here instead of being
 *  recomputed the same way the code computes them.
 *-------------------------------------------------------------*/
void test_console_baud_divisor(void) {
    uint32_t pclk2_expected;
    uint32_t brr_expected;
    switch ((OW_PORT_SYSCLK_MHZ)) {
    case 180:
        pclk2_expected = 90; /* APB2 /2 at the F446's 180MHz */
        break;
    case 168:
        pclk2_expected = 84; /* APB2 /2 at the F407's 168MHz */
        break;
    case 84:
        pclk2_expected = 42; /* APB2 /2 at the F401's 84MHz */
        break;
    default:
        pclk2_expected = (OW_PORT_SYSCLK_MHZ); /* raw HSI/HSE: APB2 stays /1 */
        break;
    }
    TEST_ASSERT_EQUAL_UINT32(pclk2_expected, (uint32_t)OW_F4_PCLK2_MHZ);

    /* 115200 baud: BRR = round(PCLK / 115200) with the register carrying
     * USARTDIV = mantissa + fraction/16. */
    brr_expected = (pclk2_expected * 1000000u + 57600u) / 115200u;
    TEST_ASSERT_EQUAL_UINT32(brr_expected, (uint32_t)OW_F4_CONSOLE_BRR);
    /* Sanity on the arithmetic itself: at every supported clock the divisor
     * must land in a sane BRR range. A SYSCLK-sized mistake gives ~1563 at
     * 180MHz, i.e. double this. */
    TEST_ASSERT_TRUE(brr_expected > 50u && brr_expected < 2000u);
}
#endif /* OW_PORT_TARGET_F4 */

#if defined(OW_PORT_TARGET_G4)
/*-------------------------------------------------------------
 *  The console UART divisor must follow the APB prescalers.
 *
 *  Same gap the F4 divisor check closes: the BRR write lives in
 *  hardware_init() - inside #if !defined(DS18B20_TEST_HARNESS), so no host
 *  build has ever compiled it on any family - and a per-clock table without
 *  the new clock falls through silently. The G4 backend leaves APB2 at /1
 *  at both clocks, so PCLK2 = SYSCLK; asserting the app.h value pins that
 *  against both executables (170 default, 16 second clock).
 *-------------------------------------------------------------*/
void test_g4_console_baud_divisor(void) {
    uint32_t brr_expected;

    TEST_ASSERT_EQUAL_UINT32((OW_PORT_SYSCLK_MHZ), (uint32_t)OW_G4_PCLK2_MHZ);

    /* 115200 baud: BRR = round(PCLK / 115200). */
    brr_expected = ((OW_PORT_SYSCLK_MHZ) * 1000000u + 57600u) / 115200u;
    TEST_ASSERT_EQUAL_UINT32(brr_expected, (uint32_t)OW_G4_CONSOLE_BRR);
    /* Sanity on the arithmetic itself: at every supported clock the divisor
     * must land in a sane BRR range. */
    TEST_ASSERT_TRUE(brr_expected > 50u && brr_expected < 2000u);
}
#endif /* OW_PORT_TARGET_G4 */

#if defined(OW_PORT_TARGET_H5)
/*-------------------------------------------------------------
 *  The console UART divisor must follow the APB prescalers.
 *
 *  Same gap the G4 divisor check closes: the BRR write lives in
 *  hardware_init() - inside #if !defined(DS18B20_TEST_HARNESS), so no host
 *  build has ever compiled it on any family. The H5 backend leaves APB2 at
 *  /1, so PCLK2 = SYSCLK; asserting the app.h value pins that.
 *-------------------------------------------------------------*/
void test_h5_console_baud_divisor(void) {
    uint32_t brr_expected;

    TEST_ASSERT_EQUAL_UINT32((OW_PORT_SYSCLK_MHZ), (uint32_t)OW_H5_PCLK2_MHZ);

    /* 115200 baud: BRR = round(PCLK / 115200). */
    brr_expected = ((OW_PORT_SYSCLK_MHZ) * 1000000u + 57600u) / 115200u;
    TEST_ASSERT_EQUAL_UINT32(brr_expected, (uint32_t)OW_H5_CONSOLE_BRR);
    /* Sanity on the arithmetic itself: at every supported clock the divisor
     * must land in a sane BRR range. */
    TEST_ASSERT_TRUE(brr_expected > 50u && brr_expected < 2000u);
}
#endif /* OW_PORT_TARGET_H5 */

void run_test_timing(void) {
    TEST_RUN(test_timing_reset_programs_timeout_and_pulse);
    TEST_RUN(test_timing_command_programs_slot_period);
    TEST_RUN(test_timing_read_programs_72_slots);
    TEST_RUN(test_timing_wait_conversion_750ms);
    TEST_RUN(test_timing_long_wait_arr_rcr);
    TEST_RUN(test_timing_temperature_formula);
    TEST_RUN(test_apb_prescaler_div1_for_tim1);
    TEST_RUN(test_ic4f_matches_the_documented_tier);
#if defined(OW_PORT_TARGET_F4)
    TEST_RUN(test_console_baud_divisor);
#endif
#if defined(OW_PORT_TARGET_G4)
    TEST_RUN(test_g4_console_baud_divisor);
#endif
#if defined(OW_PORT_TARGET_H5)
    TEST_RUN(test_h5_console_baud_divisor);
#endif
    TEST_RUN(test_search_start_ignored_while_running);
}
