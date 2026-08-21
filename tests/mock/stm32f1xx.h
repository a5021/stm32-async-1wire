#ifndef STM32F1XX_MOCK_H
#define STM32F1XX_MOCK_H
/* Host-build stand-in for the STM32F1 CMSIS device header.
 * Provides just enough register types, instance symbols and bit-field
 * constants for the DS18B20 driver, plus the compiler helpers it expects. */
#include <stdint.h>

/* --- Register types (host mocks, one instance each) --- */
typedef struct {
    volatile uint32_t CR1;
    volatile uint32_t CR2;
    volatile uint32_t SMCR;
    volatile uint32_t DIER;
    volatile uint32_t SR;
    volatile uint32_t EGR;
    volatile uint32_t CCMR1;
    volatile uint32_t CCMR2;
    volatile uint32_t CCER;
    volatile uint32_t CNT;
    volatile uint32_t PSC;
    volatile uint32_t ARR;
    volatile uint32_t RCR;
    volatile uint32_t CCR1;
    volatile uint32_t CCR2;
    volatile uint32_t CCR3;
    volatile uint32_t CCR4;
    volatile uint32_t BDTR;
    volatile uint32_t DCR;
    volatile uint32_t DMAR;
} TIM1_TypeDef;

typedef struct {
    volatile uint32_t CCR;
    volatile uint32_t CNDTR;
    volatile uint32_t CPAR;
    volatile uint32_t CMAR;
} DMA1_Channel_TypeDef;

typedef struct {
    volatile uint32_t CRL;
    volatile uint32_t CRH;
    volatile uint32_t IDR;
    volatile uint32_t ODR;
    volatile uint32_t BSRR;
    volatile uint32_t BRR;
    volatile uint32_t LCKR;
} GPIO_TypeDef;

typedef struct {
    volatile uint32_t CR;
    volatile uint32_t CFGR;
    volatile uint32_t CIR;
    volatile uint32_t APB2RSTR;
    volatile uint32_t APB1RSTR;
    volatile uint32_t AHBENR;
    volatile uint32_t APB2ENR;
    volatile uint32_t APB1ENR;
    volatile uint32_t BDCR;
    volatile uint32_t CSR;
} RCC_TypeDef;

/* Instances: pointers so macro.h's (*TIM1), (*DMA1_Channel2), etc. work.
 * STM32F103 fixed request map (RM0008 table 78): feed rides TIM1_CC2 ->
 * channel 2, capture rides TIM1_CH4 -> channel 4. The storage objects keep
 * their legacy names so tests and hw_model stay target-agnostic. */
extern TIM1_TypeDef mock_tim1;
extern DMA1_Channel_TypeDef mock_dma1_ch3;
extern DMA1_Channel_TypeDef mock_feed_ch;
extern GPIO_TypeDef mock_gpioa;
extern RCC_TypeDef mock_rcc;
#define TIM1 (&mock_tim1)
#define DMA1_Channel2 (&mock_feed_ch) /* CC2 slot-end marker -> feeds CCR3 */
#define DMA1_Channel4 (&mock_dma1_ch3) /* CC4 capture -> drains CCR4 */
#define GPIOA (&mock_gpioa)
#define RCC (&mock_rcc)

/* --- Bit-field constants used by the driver --- */
#define RCC_APB2ENR_IOPAEN 0x00000004u
#define RCC_APB2ENR_TIM1EN 0x00000800u
#define RCC_AHBENR_DMA1EN 0x00000001u
#define GPIO_CRH_CNF10_0 0x00000400u
#define GPIO_CRH_CNF10_1 0x00000800u
#define GPIO_CRH_MODE10_1 0x00000200u
#define TIM_BDTR_MOE 0x00008000u
#define TIM_EGR_UG 0x00000001u
#define TIM_SR_UIF 0x00000001u
#define TIM_CR1_CEN 0x00000001u
#define TIM_CR1_OPM 0x00000008u
#define TIM_CCMR2_OC3M_0 0x00000010u
#define TIM_CCMR2_OC3M_1 0x00000020u
#define TIM_CCMR2_OC3M_2 0x00000040u
#define TIM_CCMR2_OC3PE 0x00000008u
#define TIM_CCMR2_CC4S_1 0x00000200u
#define TIM_CCMR2_IC4F_0 0x00001000u
#define TIM_CCMR2_IC4F_1 0x00002000u
#define TIM_CCMR2_IC4F_2 0x00004000u
#define TIM_CCER_CC3E 0x00000100u
#define TIM_CCER_CC4E 0x00001000u
#define TIM_DIER_CC2DE 0x00000400u
#define TIM_DIER_CC4DE 0x00004000u
#define DMA_CCR_EN 0x00000001u
#define DMA_CCR_DIR 0x00000010u
#define DMA_CCR_MINC 0x00000080u
#define DMA_CCR_PSIZE_0 0x00000100u
#define DMA_CCR_MSIZE_0 0x00000400u
#define DMA_CCR_MSIZE_1 0x00000800u

/* --- Compiler helpers the driver expects from CMSIS --- */
#ifndef __STATIC_FORCEINLINE
#define __STATIC_FORCEINLINE static __attribute__((always_inline)) inline
#endif
#ifndef __STATIC_INLINE
#define __STATIC_INLINE static inline
#endif
#ifndef __WEAK
#define __WEAK __attribute__((weak))
#endif
#define __DSB() ((void)0)

#endif /* STM32F1XX_MOCK_H */
