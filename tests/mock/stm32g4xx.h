#ifndef STM32G4XX_MOCK_H
#define STM32G4XX_MOCK_H
/* Host-build stand-in for the STM32G4 CMSIS device header.
 * Provides just enough register types, instance symbols and bit-field
 * constants for the DS18B20 driver, plus the compiler helpers it expects.
 * Register/bit names follow the real stm32g474xx.h spelling (MODER10,
 * OT_10, AHB1ENR/AHB2ENR/APB2ENR, DMAMUX channel type) so tests catch
 * wrong-symbol bugs of the kind the F0 BSRR fix exposed.
 * Defines the family macro like the real stm32g4xx.h so the ow_port.h
 * PlatformIO/CubeMX fallback path is exercised on the host too. */
#define STM32G4 1
#include "ow_config.h"
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
    /* RCR is 8-bit on real TIM1 hardware; keep the mock 8-bit so host tests
     * reproduce the truncation instead of silently accepting wide values. */
    volatile uint8_t RCR;
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
    volatile uint32_t RESERVED0;
    volatile uint32_t CIER;
    volatile uint32_t CIFR;
    volatile uint32_t CICR;
    volatile uint32_t AHB1RSTR;
    volatile uint32_t AHB2RSTR;
    volatile uint32_t AHB3RSTR;
    volatile uint32_t APB1RSTR1;
    volatile uint32_t APB1RSTR2;
    volatile uint32_t APB2RSTR;
    volatile uint32_t AHB1ENR;
    volatile uint32_t AHB2ENR;
    volatile uint32_t AHB3ENR;
    volatile uint32_t APB1ENR1;
    volatile uint32_t APB1ENR2;
    volatile uint32_t APB2ENR;
} RCC_TypeDef;

typedef struct {
    volatile uint32_t ACR;
} FLASH_TypeDef;

typedef struct {
    volatile uint32_t CR1;
    volatile uint32_t SR2;
} PWR_TypeDef;

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

/* Instances: pointers so ow_bits.h's (*TIM1), (*DMA1_Channel3), etc. work.
 * STM32G474 routes peripheral DMA requests through DMAMUX (no fixed map):
 * feed rides DMAMUX channel 2 paired with DMA1_Channel3 (TIM1_CC2), capture
 * rides DMAMUX channel 3 paired with DMA1_Channel4 (TIM1_CH4). The storage
 * objects keep their legacy names so tests and hw_model stay target-agnostic. */
extern TIM1_TypeDef mock_tim1;
extern DMA1_Channel_TypeDef mock_dma1_ch4;
extern DMA1_Channel_TypeDef mock_feed_ch;
extern GPIO_TypeDef mock_gpioa;
extern RCC_TypeDef mock_rcc;
extern FLASH_TypeDef mock_flash;
extern PWR_TypeDef mock_pwr;
extern DMAMUX_Channel_TypeDef mock_dmamux_ch2;
extern DMAMUX_Channel_TypeDef mock_dmamux_ch3;
extern USART_TypeDef mock_usart1;
#define TIM1 (&mock_tim1)
#define DMA1_Channel3 (&mock_feed_ch) /* CC2 slot-end marker -> feeds CCR3 */
#define DMA1_Channel4 (&mock_dma1_ch4) /* CC4 capture -> drains CCR4 */
#define GPIOA (&mock_gpioa)
#define RCC (&mock_rcc)
#define FLASH (&mock_flash)
#define PWR (&mock_pwr)
#define DMAMUX1_Channel2 (&mock_dmamux_ch2)
#define DMAMUX1_Channel3 (&mock_dmamux_ch3)
#define USART1 (&mock_usart1)

/* --- Bit-field constants used by the driver (G4 spellings) --- */
#define RCC_AHB1ENR_DMA1EN 0x00000001u
#define RCC_AHB1ENR_DMAMUX1EN 0x00000004u
#define RCC_AHB2ENR_GPIOAEN 0x00000001u
#define RCC_AHB2ENR_GPIOCEN 0x00000004u
#define RCC_APB1ENR1_PWREN 0x10000000u
#define RCC_APB2ENR_SYSCFGEN 0x00000001u
#define RCC_APB2ENR_TIM1EN 0x00000800u
#define RCC_APB2ENR_USART1EN 0x00004000u
#define RCC_CFGR_PPRE1_Msk (0x7UL << 8) /* APB1 prescaler field [10:8] */
#define RCC_CFGR_PPRE2_Msk (0x7UL << 11) /* APB2 prescaler field [13:11] */
#define RCC_CR_HSEON 0x00010000u
#define RCC_CR_HSERDY 0x00020000u
#define RCC_CR_PLLON 0x01000000u
#define RCC_CR_PLLRDY 0x02000000u
#define RCC_CFGR_SW 0x00000003u
#define RCC_CFGR_SW_PLL 0x00000003u
#define RCC_CFGR_SWS 0x0000000Cu
#define RCC_CFGR_SWS_PLL 0x0000000Cu
#define RCC_PLLCFGR_PLLSRC_HSE 0x00000003u
#define RCC_PLLCFGR_PLLSRC 0x00000003u
#define RCC_PLLCFGR_PLLSRC_HSE 0x00000003u
#define RCC_PLLCFGR_PLLM_Pos 4u
#define RCC_PLLCFGR_PLLN_Pos 8u
#define RCC_PLLCFGR_PLLR_Pos 25u
#define RCC_PLLCFGR_PLLREN 0x01000000u
#define FLASH_ACR_PRFTEN 0x00000100u
#define FLASH_ACR_ICEN 0x00000200u
#define FLASH_ACR_DCEN 0x00000400u
#define FLASH_ACR_LATENCY_5WS 0x00000005u
#define FLASH_ACR_LATENCY_8WS 0x00000008u
#define PWR_CR1_VOS 0x00000600u
#define PWR_SR2_VOSF 0x00000400u
#define GPIO_MODER_MODER8 0x00030000u
#define GPIO_MODER_MODER8_0 0x00010000u
#define GPIO_MODER_MODER9 0x000C0000u
#define GPIO_MODER_MODER9_1 0x00080000u
#define GPIO_MODER_MODER10 0x00300000u
#define GPIO_MODER_MODER10_0 0x00100000u
#define GPIO_MODER_MODER10_1 0x00200000u
#define GPIO_OTYPER_OT_10 0x00000400u
#define GPIO_OSPEEDR_OSPEED10 0x00300000u
#define GPIO_OSPEEDR_OSPEED10_0 0x00100000u
#define GPIO_OSPEEDR_OSPEED10_1 0x00200000u
#define GPIO_OSPEEDR_OSPEED10_Pos 20u
#define GPIO_BSRR_BS8 0x00000100u
#define GPIO_BSRR_BR8 0x01000000u
#define GPIO_BSRR_BS10 0x00000400u
#define GPIO_BSRR_BS13 0x00002000u
#define GPIO_BSRR_BR13 0x02000000u
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
#define TIM_CCMR2_IC4F_3 0x00008000u
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
 *     G4 maps OW_PORT_TIM1_UPD_IRQn to TIM1_UP_TIM16_IRQn. --- */
#if OW_PORT_LOW_POWER
#define TIM1_UP_TIM16_IRQn 0
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

#endif /* STM32G4XX_MOCK_H */
