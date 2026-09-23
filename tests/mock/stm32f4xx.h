#ifndef STM32F4XX_MOCK_H
#define STM32F4XX_MOCK_H
/* Host-build stand-in for the STM32F4 CMSIS device header.
 * Provides just enough register types, instance symbols and bit-field
 * constants for the DS18B20 driver, plus the compiler helpers it expects.
 * Register/bit names follow the real stm32f407xx.h spelling (DMA_Stream_TypeDef
 * with CR/NDTR/PAR/M0AR, DMA_SxCR_* positions, MODER/OTYPER/AFR, RCC_AHB1ENR,
 * RCC_APB2ENR) so tests catch wrong-symbol bugs.
 *
 * DMA streams: DMA2_Stream2 (feed, TIM1_CH2) and DMA2_Stream4 (capture,
 * TIM1_CH4). The stream struct keeps the legacy F1-style field aliases
 * (CCR/CNDTR/CPAR/CMAR) in anonymous unions so the target-agnostic hw_model
 * and the register-level tests read one storage through either spelling.
 * Defines the family macro like the real stm32f4xx.h so the ow_port.h
 * PlatformIO/CubeMX fallback path is exercised on the host too. */
#define STM32F4 1
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

/* DMA stream registers: real F4 spelling CR/NDTR/PAR/M0AR, with the legacy
 * F1-style names exposed as aliases to the same storage. */
typedef struct {
    union {
        volatile uint32_t CR;
        volatile uint32_t CCR;
    };
    union {
        volatile uint32_t NDTR;
        volatile uint32_t CNDTR;
    };
    union {
        volatile uint32_t PAR;
        volatile uint32_t CPAR;
    };
    union {
        volatile uint32_t M0AR;
        volatile uint32_t CMAR;
    };
} DMA_Stream_TypeDef;

/* hw_model.c declares its storage objects with the F1-era type name; alias it
 * so the model stays family-agnostic. */
typedef DMA_Stream_TypeDef DMA1_Channel_TypeDef;

/* DMA2 controller: interrupt/flag registers. The F4 port writes LIFCR/HIFCR
 * only to clear stream TCIF (ow_port_dma_rearm). */
typedef struct {
    volatile uint32_t LISR;
    volatile uint32_t HISR;
    volatile uint32_t LIFCR;
    volatile uint32_t HIFCR;
} DMA_TypeDef;

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
    volatile uint32_t PLLCFGR;
    volatile uint32_t CFGR;
    volatile uint32_t CIR;
    volatile uint32_t AHB1ENR;
    volatile uint32_t APB2ENR;
} RCC_TypeDef;

/* Flash interface: only ACR is touched by configure_system_clock() latency
 * programming; the remaining registers keep the real layout for fidelity. */
typedef struct {
    volatile uint32_t ACR;
    volatile uint32_t KEYR;
    volatile uint32_t OPTKEYR;
    volatile uint32_t SR;
    volatile uint32_t CR;
    volatile uint32_t OPTCR;
} FLASH_TypeDef;

typedef struct {
    volatile uint32_t SR;
    volatile uint32_t DR;
    volatile uint32_t BRR;
    volatile uint32_t CR1;
    volatile uint32_t CR2;
    volatile uint32_t CR3;
    volatile uint32_t GTPR;
} USART_TypeDef;

/* Instances: pointers so ow_bits.h's (*TIM1), (*DMA2_Stream2), etc. work.
 * F4 fixed request map (RM0368 §9.3.3): feed rides DMA2_Stream2 (TIM1_CH2),
 * capture rides DMA2_Stream4 (TIM1_CH4), each with its own CHSEL=6. The
 * storage objects keep their legacy names so tests and hw_model stay
 * target-agnostic. */
extern TIM1_TypeDef mock_tim1;
extern DMA_Stream_TypeDef mock_dma1_ch4; /* DMA2_Stream4: CC4 capture */
extern DMA_Stream_TypeDef mock_feed_ch; /* DMA2_Stream2: CC2 slot-end feed */
extern DMA_TypeDef mock_dma2;
extern GPIO_TypeDef mock_gpioa;
extern RCC_TypeDef mock_rcc;
extern USART_TypeDef mock_usart1;
extern FLASH_TypeDef mock_flash;
#define TIM1 (&mock_tim1)
#define DMA2_Stream2 (&mock_feed_ch) /* CC2 slot-end marker -> feeds CCR3 */
#define DMA2_Stream4 (&mock_dma1_ch4) /* CC4 capture -> drains CCR4 */
#define DMA2 (&mock_dma2)
#define GPIOA (&mock_gpioa)
#define RCC (&mock_rcc)
#define USART1 (&mock_usart1)
#define FLASH (&mock_flash)

/* --- Bit-field constants used by the driver (F4 spellings) --- */
#define RCC_AHB1ENR_GPIOAEN 0x00000001u
#define RCC_AHB1ENR_DMA2EN 0x00400000u
#define RCC_APB2ENR_TIM1EN 0x00000001u
/* Clock-path constants used by configure_system_clock() under the harness
 * (RM0090 / stm32f407xx.h spellings and values). */
#define RCC_CR_HSEON 0x00010000u
#define RCC_CR_HSERDY 0x00020000u
#define RCC_CR_PLLON 0x01000000u
#define RCC_CR_PLLRDY 0x02000000u
#define RCC_PLLCFGR_PLLSRC_HSE 0x00400000u
#define RCC_PLLCFGR_PLLM_Pos 0u
#define RCC_PLLCFGR_PLLN_Pos 6u
#define RCC_PLLCFGR_PLLP_Pos 16u
#define RCC_PLLCFGR_PLLQ_Pos 24u
#define RCC_CFGR_SW 0x00000003u
#define RCC_CFGR_SW_PLL 0x00000002u
#define RCC_CFGR_SWS 0x0000000Cu
#define RCC_CFGR_SWS_PLL 0x00000008u
#define RCC_CFGR_PPRE1_Msk (0x7UL << 10) /* APB1 prescaler field [12:10] */
#define RCC_CFGR_PPRE1 RCC_CFGR_PPRE1_Msk
#define RCC_CFGR_PPRE1_DIV4 0x00001400u /* HCLK/4 -> 42MHz at 168 */
#define RCC_CFGR_PPRE2_Msk (0x7UL << 13) /* APB2 prescaler field [15:13] */
#define RCC_CFGR_PPRE2 RCC_CFGR_PPRE2_Msk
#define RCC_CFGR_PPRE2_DIV2 0x00008000u /* HCLK/2 -> 84MHz at 168 */
#define FLASH_ACR_PRFTEN 0x00000100u
#define FLASH_ACR_ICEN 0x00000200u
#define FLASH_ACR_DCEN 0x00000400u
#define FLASH_ACR_LATENCY_5WS 0x00000005u
#define GPIO_MODER_MODER10 0x00C00000u
#define GPIO_MODER_MODER10_0 0x00400000u
#define GPIO_MODER_MODER10_1 0x00800000u
#define GPIO_MODER_MODER11 0x00000C00u
#define GPIO_MODER_MODER11_0 0x00000400u
#define GPIO_MODER_MODER11_1 0x00000800u
#define GPIO_OTYPER_OT_10 0x00000400u
#define GPIO_OTYPER_OT_11 0x00000800u
#define GPIO_OSPEEDR_OSPEED10 0x00300000u
#define GPIO_OSPEEDR_OSPEED10_0 0x00100000u
#define GPIO_OSPEEDR_OSPEED10_1 0x00200000u
#define GPIO_OSPEEDR_OSPEED10_Pos 20u
#define GPIO_ODR_OD11 0x00000800u
#define GPIO_BSRR_BS_10 0x00000400u
#define GPIO_AFRH_AFSEL10 0x00000F00u
#define GPIO_AFRH_AFSEL10_Pos 8U
#define TIM_BDTR_MOE 0x00008000u
#define TIM_EGR_UG 0x00000001u
#define TIM_SR_UIF 0x00000001u
#define TIM_CR1_CEN 0x00000001u
#define TIM_CR1_URS 0x00000004u
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
/* DMA_SxCR bit positions (RM0368 §9.3.4). */
#define DMA_SxCR_EN 0x00000001u
#define DMA_SxCR_DIR_0 0x00000040u
#define DMA_SxCR_MINC 0x00000400u
#define DMA_SxCR_PSIZE_0 0x00000800u
#define DMA_SxCR_MSIZE_0 0x00002000u
#define DMA_SxCR_MSIZE_1 0x00004000u
#define DMA_SxCR_PL_1 0x00020000u
#define DMA_SxCR_CHSEL_Pos 25U
/* Stream2 flag-clear bits (low group, streams 0-3, each 6 bits:
 *  stream0 @0, stream1 @6, stream2 @12, stream3 @18.
 *  Within 6 bits: [FE, X, DME, TE, HT, TC] = [bit0, bit1, bit2, bit3, bit4, bit5]. */
#define DMA_LIFCR_CFEIF2 0x00001000u /* Stream2 FIFO error      (bit 12) */
#define DMA_LIFCR_CDMEIF2 0x00004000u /* Stream2 direct-mode err (bit 14) */
#define DMA_LIFCR_CTEIF2 0x00008000u /* Stream2 transfer error  (bit 15) */
#define DMA_LIFCR_CHTIF2 0x00010000u /* Stream2 half-transfer   (bit 16) */
#define DMA_LIFCR_CTCIF2 0x00020000u /* Stream2 transfer compl  (bit 17) */
/* Stream4 flag-clear bits (high group, streams 4-7). Stream4 starts at
 *  HIFCR bit 0: [FE, X, DME, TE, HT, TC] = [0,1,2,3,4,5]. */
#define DMA_HIFCR_CFEIF4 0x00000001u /* Stream4 FIFO error      (bit  0) */
#define DMA_HIFCR_CDMEIF4 0x00000004u /* Stream4 direct-mode err (bit  2) */
#define DMA_HIFCR_CTEIF4 0x00000008u /* Stream4 transfer error  (bit  3) */
#define DMA_HIFCR_CHTIF4 0x00000010u /* Stream4 half-transfer   (bit  4) */
#define DMA_HIFCR_CTCIF4 0x00000020u /* Stream4 transfer compl  (bit  5) */
/* Legacy F1-style aliases: the target-agnostic hw_model and the register-level
 * tests name the same F4 bits through these spellings (DIR maps to the low bit
 * of the two-bit F4 direction field). */
#define DMA_CCR_EN DMA_SxCR_EN
#define DMA_CCR_DIR DMA_SxCR_DIR_0
#define DMA_CCR_MINC DMA_SxCR_MINC
#define DMA_CCR_PSIZE_0 DMA_SxCR_PSIZE_0
#define DMA_CCR_MSIZE_0 DMA_SxCR_MSIZE_0
#define DMA_CCR_MSIZE_1 DMA_SxCR_MSIZE_1
#define USART_SR_TXE 0x00000080u

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
 *     F4 maps OW_PORT_TIM1_UPD_IRQn to TIM1_UP_TIM10_IRQn (TIM1_UP_IRQn does
 *     not exist on this family, so accidentally reusing the F1 mapping must
 *     fail to compile here). --- */
#if OW_PORT_LOW_POWER
#define TIM1_UP_TIM10_IRQn 25
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

#endif /* STM32F4XX_MOCK_H */
