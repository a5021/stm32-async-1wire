#ifndef MOCK_TARGET_H
#define MOCK_TARGET_H
/* Target dispatcher for the host-test mocks: pulls in the device stand-in
 * matching the backend under test (OW_PORT_TARGET_F0 / OW_PORT_TARGET_F1). */
#if defined(OW_PORT_TARGET_F0)
#include "stm32f0xx.h"
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
#endif /* MOCK_TARGET_H */
