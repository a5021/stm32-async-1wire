#ifndef MOCK_TARGET_H
#define MOCK_TARGET_H
/* Target dispatcher for the host-test mocks: pulls in the device stand-in
 * matching the backend under test (OW_PORT_TARGET_F0 / OW_PORT_TARGET_F1). */
#if defined(OW_PORT_TARGET_F0)
#include "stm32f0xx.h"
#else
#include "stm32f1xx.h"
#endif

/* Semantic aliases over the backend register-layout differences: the PWM
 * output lives on CH1/CCR1 (preload OC1PE in CCMR1) on F1 but moved to
 * CH3/CCR3 (OC3PE in CCMR2) on F0; the capture register is CCR2 vs CCR4,
 * and the slot-end marker compare sits on CC3 (CCR3) vs CC2 (CCR2). The
 * DMA request bits follow their channels. */
#if defined(OW_PORT_TARGET_F0)
#define MOCK_TIM_OUT_CCMR (mock_tim1.CCMR2)
#define MOCK_TIM_OUT_PE TIM_CCMR2_OC3PE
#define MOCK_TIM_OUT_CCR (mock_tim1.CCR3)
#define MOCK_TIM_MARKER_CCR (mock_tim1.CCR2)
#define MOCK_TIM_CAP_CCR (mock_tim1.CCR4)
#define MOCK_TIM_FEED_DE TIM_DIER_CC2DE
#define MOCK_TIM_CAP_DE TIM_DIER_CC4DE
#define MOCK_TIM_OUT_CCE TIM_CCER_CC3E
#define MOCK_TIM_CAP_CCE TIM_CCER_CC4E
/* 1-Wire bus pin: PA8 on F1, PA10 on F0. */
#define MOCK_BUS_MODER_1 GPIO_MODER_MODER10_1
#define MOCK_BUS_MODER_0 GPIO_MODER_MODER10_0
#define MOCK_BUS_OT GPIO_OTYPER_OT_10
#define MOCK_BUS_AFSEL GPIO_AFRH_AFSEL10
#define MOCK_BUS_AFSEL_POS GPIO_AFRH_AFSEL10_Pos
#else
#define MOCK_TIM_OUT_CCMR (mock_tim1.CCMR1)
#define MOCK_TIM_OUT_PE TIM_CCMR1_OC1PE
#define MOCK_TIM_OUT_CCR (mock_tim1.CCR1)
#define MOCK_TIM_MARKER_CCR (mock_tim1.CCR3)
#define MOCK_TIM_CAP_CCR (mock_tim1.CCR2)
#define MOCK_TIM_FEED_DE TIM_DIER_CC3DE
#define MOCK_TIM_CAP_DE TIM_DIER_CC2DE
#define MOCK_TIM_OUT_CCE TIM_CCER_CC1E
#define MOCK_TIM_CAP_CCE TIM_CCER_CC2E
/* 1-Wire bus pin: PA8 on F1, PA10 on F0. */
#define MOCK_BUS_MODER_1 GPIO_MODER_MODER8_1
#define MOCK_BUS_MODER_0 GPIO_MODER_MODER8_0
#define MOCK_BUS_OT GPIO_OTYPER_OT_8
#define MOCK_BUS_AFSEL GPIO_AFRH_AFSEL8
#define MOCK_BUS_AFSEL_POS GPIO_AFRH_AFSEL8_Pos
#endif
#endif /* MOCK_TARGET_H */
