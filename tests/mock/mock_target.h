#ifndef MOCK_TARGET_H
#define MOCK_TARGET_H
/* Target dispatcher for the host-test mocks: pulls in the device stand-in
 * matching the backend under test (OW_PORT_TARGET_F0 / OW_PORT_TARGET_F1 /
 * OW_PORT_TARGET_G0). */
#if defined(OW_PORT_TARGET_F4)
#include "stm32f4xx.h"
#elif defined(OW_PORT_TARGET_F0)
#include "stm32f0xx.h"
#elif defined(OW_PORT_TARGET_G0)
#include "stm32g0xx.h"
#else
#include "stm32f1xx.h"
#endif

/* Both backends share the same TIM1 channel roles: PWM output on CH3/CCR3
 * (preload OC3PE in CCMR2), indirect capture on CH4/CCR4 (IC4 <- TI3), and
 * the slot-end marker compare on CC2/CCR2. Only the DMA channel numbers and
 * the GPIO pin configuration differ; those live in the per-target mock
 * headers via the CMSIS instance names. */
#define MOCK_TIM_OUT_CCMR (mock_tim1.CCMR2)
#define MOCK_TIM_OUT_PE TIM_CCMR2_OC3PE
#define MOCK_TIM_OUT_CCR (mock_tim1.CCR3)
#define MOCK_TIM_MARKER_CCR (mock_tim1.CCR2)
#define MOCK_TIM_CAP_CCR (mock_tim1.CCR4)
#define MOCK_TIM_FEED_DE TIM_DIER_CC2DE
#define MOCK_TIM_CAP_DE TIM_DIER_CC4DE
#define MOCK_TIM_OUT_CCE TIM_CCER_CC3E
#define MOCK_TIM_CAP_CCE TIM_CCER_CC4E

/* --- DMA model abstraction -------------------------------------------------
 * The hw_model works on one feed and one capture channel per family. F1/F0/G0
 * use DMA1 channels with the CCR/CNDTR/CPAR/CMAR register names and 8-bit
 * memory cells; F4 uses DMA2 streams with CR/NDTR/PAR/M0AR. The F4 mock unions
 * keep the legacy field spellings valid, so the model can name a field
 * uniformly; only the ENABLE/MSIZE bit spellings and the memory-cell width
 * genuinely differ and are abstracted here. */
#define MOCK_DMA_FEED mock_feed_ch /* feed channel storage */
#define MOCK_DMA_CAP mock_dma1_ch4 /* capture channel storage */
#if defined(OW_PORT_TARGET_F4)
#define MOCK_DMA_FEED_EN DMA_SxCR_EN
#define MOCK_DMA_CAP_EN DMA_SxCR_EN
#define MOCK_DMA_CAP_MSIZE_0 DMA_SxCR_MSIZE_0
#else
#define MOCK_DMA_FEED_EN DMA_CCR_EN
#define MOCK_DMA_CAP_EN DMA_CCR_EN
#define MOCK_DMA_CAP_MSIZE_0 DMA_CCR_MSIZE_0
#endif

/* --- bus pin state, family-neutral ------------------------------------------
 * The bus pin is PA10 on every family, but the CMSIS fields that say "alternate
 * function" and "open-drain" are spelled three different ways: F1 configures
 * the pin through the legacy CRH CNF10 field, G0's CMSIS drops the R (MODE10,
 * not MODER10), and F0/F4 use MODER10 with OTYPER for the output stage.
 *
 * A test that only wants to know which state the pin is in should ask through
 * these three. test_parasite.c, test_active_drive.c and test_state_machine.c
 * each carried its own copy of the spelling, and a fourth copy is what made the
 * mocks' wrong MODER10 bit field invisible - the same word in a test file reads
 * as deliberate, where a wrong value in a mock does not.
 *
 * MOCK_PIN_AT_MAX_SPEED is the F1 CRH MODE10 field; the other families have an
 * equivalent two-bit speed field in OSPEEDR, so a test asking about drive
 * strength should read that field itself - the value the driver programs is
 * pinned per family by test_port_init_contract.c. */
#if defined(OW_PORT_TARGET_F1)
#define MOCK_PIN_IS_AF() (((mock_gpioa.CRH & GPIO_CRH_CNF10_1) != 0u))
#define MOCK_PIN_IS_OD() (((mock_gpioa.CRH & GPIO_CRH_CNF10_0) != 0u))
#define MOCK_PIN_IS_PP() (((mock_gpioa.CRH & GPIO_CRH_CNF10_0) == 0u))
#define MOCK_PIN_AT_MAX_SPEED() (((mock_gpioa.CRH & GPIO_CRH_MODE10) == GPIO_CRH_MODE10_1))
#else
#if defined(OW_PORT_TARGET_G0)
#define MOCK_PIN_MODER GPIO_MODER_MODE10
#define MOCK_PIN_MODE_AF GPIO_MODER_MODE10_1
#define MOCK_PIN_MODE_AF_0 GPIO_MODER_MODE10_0
#define MOCK_PIN_OTYPE GPIO_OTYPER_OT10
#else
#define MOCK_PIN_MODER GPIO_MODER_MODER10
#define MOCK_PIN_MODE_AF GPIO_MODER_MODER10_1
#define MOCK_PIN_MODE_AF_0 GPIO_MODER_MODER10_0
#define MOCK_PIN_OTYPE GPIO_OTYPER_OT_10
#endif
/* mode 0b10 is alternate function, which is what the timer needs on the pin */
#define MOCK_PIN_IS_AF() (((mock_gpioa.MODER & MOCK_PIN_MODE_AF) != 0u) && \
                          ((mock_gpioa.MODER & MOCK_PIN_MODE_AF_0) == 0u))
#define MOCK_PIN_IS_OD() (((mock_gpioa.OTYPER & MOCK_PIN_OTYPE) != 0u))
#define MOCK_PIN_IS_PP() (((mock_gpioa.OTYPER & MOCK_PIN_OTYPE) == 0u))
#endif
#endif /* MOCK_TARGET_H */
