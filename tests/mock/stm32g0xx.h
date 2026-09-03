#ifndef STM32G0XX_MOCK_H
#define STM32G0XX_MOCK_H
/* Host-build stand-in for the STM32G0 CMSIS device header.
 * Provides just enough register types, instance symbols and bit-field
 * constants for the DS18B20 driver, plus the compiler helpers it expects.
 * Register/bit names follow the real stm32g031xx.h spelling (MODE10,
 * OT10, BS10, IOPENR, APBENR2, SYSCFG_CFGR1_*_RMP) so tests catch
 * wrong-symbol bugs of the kind the F0 BSRR fix exposed. */
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
    volatile uint32_t CCR; /* DMAMUX channel request selector */
} DMAMUX_Channel_TypeDef;

typedef struct {
    volatile uint32_t MODER;
    volatile uint32_t OTYPER;
    volatile uint32_t OSPEEDR;
    volatile uint32_t PUPDR;
    volatile uint32_t IDR;
    volatile uint32_t ODR;
    volatile uint32_t BSRR;
    volatile uint32_t LCKR;
    volatile uint32_t AFR[2];
} GPIO_TypeDef;

typedef struct {
    volatile uint32_t CR;
    volatile uint32_t ICSCR;
    volatile uint32_t CFGR;
    volatile uint32_t PLLCFGR;
    volatile uint32_t RESERVED0[2];
    volatile uint32_t CIER;
    volatile uint32_t CIFR;
    volatile uint32_t CICR;
    volatile uint32_t IOPRSTR;
    volatile uint32_t AHBRSTR;
    volatile uint32_t APBRSTR1;
    volatile uint32_t APBRSTR2;
    volatile uint32_t IOPENR;
    volatile uint32_t AHBENR;
    volatile uint32_t APBENR1;
    volatile uint32_t APBENR2;
} RCC_TypeDef;

typedef struct {
    volatile uint32_t CFGR1;
} SYSCFG_TypeDef;

typedef struct {
    volatile uint32_t CR1;
    volatile uint32_t CR2;
    volatile uint32_t CR3;
    volatile uint32_t BRR;
    volatile uint32_t GTPR;
    volatile uint32_t RTOR;
    volatile uint32_t RQR;
    volatile uint32_t ISR;
    volatile uint32_t ICR;
    volatile uint32_t RDR;
    volatile uint32_t TDR;
} USART_TypeDef;

/* Instances: pointers so macro.h's (*TIM1), (*DMA1_Channel3), etc. work.
 * STM32G031 routes peripheral DMA requests through DMAMUX (no fixed map):
 * feed rides DMAMUX channel 2 paired with DMA1_Channel3 (TIM1_CC2), capture
 * rides DMAMUX channel 3 paired with DMA1_Channel4 (TIM1_CH4). The storage
 * objects keep their legacy names so tests and hw_model stay target-agnostic. */
extern TIM1_TypeDef mock_tim1;
extern DMA1_Channel_TypeDef mock_dma1_ch4;
extern DMA1_Channel_TypeDef mock_feed_ch;
extern GPIO_TypeDef mock_gpioa;
extern RCC_TypeDef mock_rcc;
extern SYSCFG_TypeDef mock_syscfg;
extern DMAMUX_Channel_TypeDef mock_dmamux_ch2;
extern DMAMUX_Channel_TypeDef mock_dmamux_ch3;
extern USART_TypeDef mock_usart1;
#define TIM1 (&mock_tim1)
#define DMA1_Channel3 (&mock_feed_ch) /* CC2 slot-end marker -> feeds CCR3 */
#define DMA1_Channel4 (&mock_dma1_ch4) /* CC4 capture -> drains CCR4 */
#define GPIOA (&mock_gpioa)
#define RCC (&mock_rcc)
#define SYSCFG (&mock_syscfg)
#define DMAMUX1_Channel2 (&mock_dmamux_ch2)
#define DMAMUX1_Channel3 (&mock_dmamux_ch3)
#define USART1 (&mock_usart1)

/* --- Bit-field constants used by the driver (G0 spellings) --- */
#define RCC_AHBENR_DMA1EN 0x00000001u
#define RCC_IOPENR_GPIOAEN 0x00000001u
#define RCC_APBENR2_SYSCFGEN 0x00000001u
#define RCC_APBENR2_TIM1EN 0x00000800u
#define RCC_CFGR_PPRE_Msk (0x7UL << 12) /* APB prescaler field [14:12] */
#define SYSCFG_CFGR1_PA11_RMP 0x00000008u
#define SYSCFG_CFGR1_PA12_RMP 0x00000010u
#define GPIO_MODER_MODE9 0x000C0000u
#define GPIO_MODER_MODE9_1 0x00080000u
#define GPIO_MODER_MODE4 0x00000C00u
#define GPIO_MODER_MODE4_0 0x00000400u
#define GPIO_MODER_MODE10 0x00C00000u
#define GPIO_MODER_MODE10_0 0x00400000u
#define GPIO_MODER_MODE10_1 0x00800000u
#define GPIO_OTYPER_OT10 0x00000400u
#define GPIO_BSRR_BS10 0x00000400u
#define GPIO_BSRR_BS4 0x00000010u
#define GPIO_BSRR_BR4 0x00100000u
#define GPIO_AFRH_AFSEL9 0x000000F0u
#define GPIO_AFRH_AFSEL9_Pos 4U
#define GPIO_AFRH_AFSEL10 0x00000F00u
#define GPIO_AFRH_AFSEL10_Pos 8U
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
#define TIM_DIER_CC4DE 0x00001000u
#define TIM_DIER_UIE 0x00000001u
#define DMA_CCR_EN 0x00000001u
#define DMA_CCR_DIR 0x00000010u
#define DMA_CCR_MINC 0x00000080u
#define DMA_CCR_PSIZE_0 0x00000100u
#define DMA_CCR_MSIZE_0 0x00000400u
#define DMA_CCR_MSIZE_1 0x00000800u
#define USART_ISR_TXE_TXFNF 0x00000080u

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

/* --- Core primitives for the opt-in low-power WFE path (OW_PORT_LOW_POWER).
 *     Host stubs mirroring the real CMSIS defines; compiled only into the
 *     low-power test build so the default busy-poll build stays byte-identical.
 *     G0 maps OW_PORT_TIM1_UPD_IRQn to TIM1_BRK_UP_TRG_COM_IRQn. --- */
#ifdef OW_PORT_LOW_POWER
#define TIM1_BRK_UP_TRG_COM_IRQn 0
#define SCB_SCR_SEVONPEND_Msk 0x00000010u
typedef struct {
    volatile uint32_t SCR;
} SCB_Type;
extern SCB_Type mock_scb;
#define SCB (&mock_scb)
#define __SEV() ((void)0)
#define __WFE() ((void)0)
#define NVIC_ClearPendingIRQ(__IRQn) ((void)(__IRQn))
#endif /* OW_PORT_LOW_POWER */

#endif /* STM32G0XX_MOCK_H */
