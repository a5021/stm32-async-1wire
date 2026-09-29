#ifndef STM32F3XX_MOCK_H
#define STM32F3XX_MOCK_H
/* Host-build stand-in for the STM32F3 CMSIS device header.
 * Provides just enough register types, instance symbols and bit-field
 * constants for the DS18B20 driver, plus the compiler helpers it expects.
 * Register/bit names follow the real stm32f303xc.h spelling (GPIO_OSPEEDER_,
 * GPIO_AFRH_AFRH2 for pin 10, RCC_AHBENR holding both the GPIO and DMA1 gates)
 * so tests catch wrong-symbol bugs of the kind the F0 BSRR fix exposed.
 * Defines the family macro like the real stm32f3xx.h so the ow_port.h
 * PlatformIO/CubeMX fallback path is exercised on the host too. */
#define STM32F3 1
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

/* The F3 RCC has no AHB2ENR: every GPIO port and DMA1 share AHBENR. */
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
 * STM32F303 has a fixed request map and no DMAMUX. Bench-verified on MB1035B:
 * TIM1's CC2 feed rides channel 3 and the CH4 capture channel 4 (D13/D14),
 * the same pair as F0/F1. The storage objects keep their legacy names so tests and
 * hw_model stay target-agnostic; only the CMSIS instance they hang off differs. */
extern TIM1_TypeDef mock_tim1;
extern DMA1_Channel_TypeDef mock_dma1_ch4;
extern DMA1_Channel_TypeDef mock_feed_ch;
extern GPIO_TypeDef mock_gpioa;
extern RCC_TypeDef mock_rcc;
extern USART_TypeDef mock_usart1;
#define TIM1 (&mock_tim1)
#define DMA1_Channel3 (&mock_feed_ch) /* CC2 slot-end marker -> feeds CCR3 */
#define DMA1_Channel4 (&mock_dma1_ch4) /* CH4 capture -> drains CCR4 */
#define GPIOA (&mock_gpioa)
#define RCC (&mock_rcc)
#define USART1 (&mock_usart1)

/* --- Bit-field constants used by the driver (F3 spellings) --- */
#define RCC_AHBENR_DMA1EN 0x00000001u
#define RCC_AHBENR_GPIOAEN 0x00020000u
#define RCC_AHBENR_GPIOEEN 0x00200000u
#define RCC_APB2ENR_TIM1EN 0x00000800u
#define RCC_CFGR_PPRE_Msk (0x7UL << 8) /* APB prescaler field [10:8] */
/* MODER bit fields, matching CMSIS stm32f303xc.h. Pin 10 occupies bits [21:20]
 * and pin 11 bits [23:22]. The F0/G0/F4 mocks once held pin 11's bits under
 * pin 10's names, which meant a test asserting "PA10 is in alternate-function
 * mode" was really asserting about PA11. Verified against the CMSIS header by
 * tests/check_mock_headers.sh. */
#define GPIO_MODER_MODER8 0x00030000u
#define GPIO_MODER_MODER8_0 0x00010000u
#define GPIO_MODER_MODER9 0x000C0000u
#define GPIO_MODER_MODER9_0 0x00040000u
#define GPIO_MODER_MODER9_1 0x00080000u
#define GPIO_MODER_MODER10 0x00300000u
#define GPIO_MODER_MODER10_0 0x00100000u
#define GPIO_MODER_MODER10_1 0x00200000u
#define GPIO_MODER_MODER11 0x00C00000u
#define GPIO_MODER_MODER11_0 0x00400000u
#define GPIO_MODER_MODER11_1 0x00800000u
#define GPIO_OTYPER_OT_8 0x00000100u
#define GPIO_OTYPER_OT_9 0x00000200u
#define GPIO_OTYPER_OT_10 0x00000400u
/* Drive strength: the F3 CMSIS spells this GPIO_OSPEEDER_, with an E. There is
 * no GPIO_OSPEEDR_* in this header at all - writing one would be a compile
 * error here and on the device, which is the point of matching the spelling. */
#define GPIO_OSPEEDER_OSPEEDR10 0x00300000u
#define GPIO_OSPEEDER_OSPEEDR10_0 0x00100000u
#define GPIO_OSPEEDER_OSPEEDR10_1 0x00200000u
#define GPIO_OSPEEDER_OSPEEDR10_Pos 20u
#define GPIO_BSRR_BS_8 0x00000100u
#define GPIO_BSRR_BR_8 0x01000000u
#define GPIO_BSRR_BS_10 0x00000400u
/* The alternate-function fields are named by their position in AFR, not by pin:
 * this header defines AFRH_AFRH0..7 and AFRL_AFRL0..7 only, so PA9 is AFRH1 and
 * PA10 is AFRH2. There is no GPIO_AFRH_AFSEL* at all. */
#define GPIO_AFRH_AFRH1 0x000000F0u
#define GPIO_AFRH_AFRH1_Pos 4U
#define GPIO_AFRH_AFRH2 0x00000F00u
#define GPIO_AFRH_AFRH2_Pos 8U
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
/* The F3 has an IC4PSC capture prescaler at CCMR2 bits [11:10] that F4 does not.
 * The shared core assigns the whole register and passes no IC4PSC bit, so this
 * must stay 0 (no prescaler) - asserted by test_port_init_contract.c, since a
 * non-zero value would silently divide the filter clock rather than the
 * capture. Declared here so the assert has a name to compare against. */
#define TIM_CCMR2_IC4PSC 0x00000C00u
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
#define USART_ISR_TXE 0x00000080u

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
 *     F3 maps OW_PORT_TIM1_UPD_IRQn to TIM1_UP_TIM16_IRQn. --- */
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

#endif /* STM32F3XX_MOCK_H */
