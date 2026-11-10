#ifndef STM32C5XX_MOCK_H
#define STM32C5XX_MOCK_H
/* Host-build stand-in for the STM32C5 CMSIS device header.
 * Provides just enough register types, instance symbols and bit-field
 * constants for the 1-Wire port, plus the compiler helpers it expects.
 * Register/bit names and values follow the real stm32c552xx.h spelling
 * (LPDMA channel CTR1/CTR2/CBR1/CSAR/CDAR registers, REQSEL in CTR2,
 * RCC CR1/CR2/CFGR1 with the PSI clock, G0-style GPIO MODE10/OT10
 * spellings, TIM1_UPD split vector) so tests catch wrong-symbol bugs and
 * test-mocks can compare every literal macro.
 *
 * The C5 has no DMA1 and no GPDMA1: only LPDMA1 (8 channels) and LPDMA2
 * (4 channels). The backend uses LPDMA1 channels 2 and 3, so the storage
 * objects keep their legacy names (mock_feed_ch / mock_dma1_ch4) and the
 * LPDMA1_CH2 / LPDMA1_CH3 macros point at them, the way the H5 mock does.
 * Defines the family macro like the real stm32c5xx.h so the ow_board.h
 * PlatformIO/CubeMX fallback path is exercised on the host too. */
#define STM32C5 1
#include "mock_port_hooks.h"
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
    /* The C5 TIM1 repetition counter is a full 32-bit register in the
     * device header (unlike the 8-bit H5 one); the driver still writes an
     * 8-bit value, so the boundary tests see the same 0..255 range. */
    volatile uint32_t RCR;
    volatile uint32_t CCR1;
    volatile uint32_t CCR2;
    volatile uint32_t CCR3;
    volatile uint32_t CCR4;
    volatile uint32_t BDTR;
    volatile uint32_t TISEL;
    volatile uint32_t DCR;
    volatile uint32_t DMAR;
} TIM1_TypeDef;

/* LPDMA channel registers: real C5 spelling CLBAR/CFCR/CSR/CCR/CTR1/CTR2/
 * CBR1/CSAR/CDAR/CLLR (stm32c552xx.h DMA_Channel_TypeDef). SAR/DAR are
 * direction-relative (source/destination), unlike the classic CPAR (always
 * peripheral) / CMAR (always memory): feed is memory-to-peripheral (mem in
 * CSAR, CCR3 in CDAR) while capture is peripheral-to-memory (CCR4 in CSAR,
 * mem in CDAR), so no legacy union can name both directions at once. The
 * model and the contract tests therefore read the direction-correct
 * register per operation. CNDTR here aliases CBR1, whose BNDT field is a
 * *source-byte* count, NOT a transfer count: because the capture source is
 * the halfword CCR4, the driver programs BNDT = 2 x transfer count and each
 * transfer consumes 2 BNDT bytes (MOCK_CAP_CNDTR / MOCK_CAP_TRANSFERS). */
typedef struct {
    volatile uint32_t CLBAR;
    uint32_t RESERVED1[2];
    volatile uint32_t CFCR;
    volatile uint32_t CSR;
    volatile uint32_t CCR;
    uint32_t RESERVED2[10];
    volatile uint32_t CTR1;
    volatile uint32_t CTR2;
    union {
        volatile uint32_t CBR1;
        volatile uint32_t CNDTR;
    };
    volatile uint32_t CSAR;
    volatile uint32_t CDAR;
    uint32_t RESERVED3[10];
    volatile uint32_t CLLR;
} DMA_Channel_TypeDef;

/* hw_model.c declares its storage objects with the F1-era type names; alias
 * them so the model stays family-agnostic. */
typedef DMA_Channel_TypeDef DMA1_Channel_TypeDef;
typedef DMA_Channel_TypeDef GPDMA_Channel_TypeDef;

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

/* Only the RCC registers the C5 clock path touches: CR1/CR2 carry HSE and
 * PSI, CFGR1 the SYSCLK mux, and the three enable registers gate LPDMA1,
 * GPIOA/GPIOE and TIM1/USART1. */
typedef struct {
    volatile uint32_t CR1;
    volatile uint32_t CR2;
    volatile uint32_t CFGR1;
    volatile uint32_t CFGR2;
    volatile uint32_t AHB1ENR;
    volatile uint32_t AHB2ENR;
    volatile uint32_t APB1LENR;
    volatile uint32_t APB1HENR;
    volatile uint32_t APB2ENR;
} RCC_TypeDef;

typedef struct {
    volatile uint32_t ACR;
} FLASH_TypeDef;

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

/* Instances: pointers so the port core's (*LPDMA1_CH2), etc. work.
 * C5 routes peripheral DMA requests per channel through REQSEL in CTR2 (no
 * DMAMUX): feed rides LPDMA1_Channel2 (TIM1_CC2, REQSEL 25), capture rides
 * LPDMA1_Channel3 (TIM1_CC4, REQSEL 27). The storage objects keep their
 * legacy names so tests and hw_model stay target-agnostic. */
extern TIM1_TypeDef mock_tim1;
extern DMA1_Channel_TypeDef mock_dma1_ch4;
extern DMA1_Channel_TypeDef mock_feed_ch;
extern GPIO_TypeDef mock_gpioa;
extern GPIO_TypeDef mock_gpioe;
extern RCC_TypeDef mock_rcc;
extern FLASH_TypeDef mock_flash;
extern USART_TypeDef mock_usart1;
#define TIM1 (&mock_tim1)
#define LPDMA1_CH2 (&mock_feed_ch) /* CC2 slot-end marker -> feeds CCR3 */
#define LPDMA1_CH3 (&mock_dma1_ch4) /* CC4 capture -> drains CCR4 */
#define GPIOA (&mock_gpioa)
#define GPIOE (&mock_gpioe)
#define RCC (&mock_rcc)
#define FLASH (&mock_flash)
#define USART1 (&mock_usart1)

/* --- Bit-field constants used by the driver (C5 spellings, real values) --- */
#define RCC_AHB1ENR_LPDMA1EN 0x00000001u
#define RCC_AHB2ENR_GPIOAEN 0x00000001u
#define RCC_AHB2ENR_GPIOEEN 0x00000010u
#define RCC_APB2ENR_TIM1EN 0x00000800u
#define RCC_APB2ENR_USART1EN 0x00004000u
#define RCC_CR1_PSISON 0x00000100u
#define RCC_CR1_PSISRDY 0x00001000u
#define RCC_CR1_HSEON 0x00010000u
#define RCC_CR1_HSERDY 0x00020000u
#define RCC_CR1_HSEBYP 0x00040000u
#define RCC_CR2_PSIREFSRC 0x00030000u
#define RCC_CR2_PSIREF 0x00700000u
#define RCC_CR2_PSIREF_0 0x00100000u
#define RCC_CR2_PSIFREQ 0x30000000u
#define RCC_CR2_PSIFREQ_0 0x10000000u
#define RCC_CFGR1_SW 0x00000003u
#define RCC_CFGR1_SW_0 0x00000001u
#define RCC_CFGR1_SW_1 0x00000002u
#define RCC_CFGR1_SWS 0x00000018u
#define RCC_CFGR1_SWS_0 0x00000008u
#define RCC_CFGR1_SWS_1 0x00000010u
/* C5 latency encoding: LATENCY_0 = 1 WS ... LATENCY_4 = 5 WS. */
#define FLASH_ACR_LATENCY 0x0000000Fu
#define FLASH_ACR_LATENCY_0 0x00000001u
#define FLASH_ACR_LATENCY_1 0x00000002u
#define FLASH_ACR_LATENCY_2 0x00000003u
#define FLASH_ACR_LATENCY_3 0x00000004u
#define FLASH_ACR_LATENCY_4 0x00000005u
#define FLASH_ACR_PRFTEN 0x00000100u
#define GPIO_MODER_MODE10 0x00300000u
#define GPIO_MODER_MODE10_0 0x00100000u
#define GPIO_MODER_MODE10_1 0x00200000u
#define GPIO_OTYPER_OT10 0x00000400u
#define GPIO_OSPEEDR_OSPEED10 0x00300000u
#define GPIO_OSPEEDR_OSPEED10_0 0x00100000u
#define GPIO_OSPEEDR_OSPEED10_1 0x00200000u
#define GPIO_OSPEEDR_OSPEED10_Pos 20u
#define GPIO_AFRH_AFSEL9 0x000000F0u
#define GPIO_AFRH_AFSEL9_Pos 4U
#define GPIO_AFRH_AFSEL10 0x00000F00u
#define GPIO_AFRH_AFSEL10_Pos 8U
#define TIM_BDTR_MOE 0x00008000u
#define TIM_EGR_UG 0x00000001u
#define TIM_SR_UIF 0x00000001u
#define TIM_SR_CC4OF 0x00001000u
#define TIM_CR1_CEN 0x00000001u
#define TIM_CR1_OPM 0x00000008u
#define TIM_CCMR2_OC3M_0 0x00000010u
#define TIM_CCMR2_OC3M_1 0x00000020u
#define TIM_CCMR2_OC3M_2 0x00000040u
#define TIM_CCMR2_OC3PE 0x00000008u
#define TIM_CCMR2_CC4S_1 0x00000200u
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
#define TIM_TISEL_TI4SEL_Pos 24u
#define TIM_TISEL_TI4SEL_Msk 0x0F000000u
#define DMA_CCR_EN 0x00000001u
#define DMA_CCR_EN_Pos 0u
#define DMA_CCR_PRIO_Pos 22u
#define DMA_CTR1_SDW_LOG2_Pos 0u
#define DMA_CTR1_SINC_Pos 3u
#define DMA_CTR1_DDW_LOG2_Pos 16u
#define DMA_CTR1_DINC_Pos 19u
#define DMA_CTR2_REQSEL_Pos 0u
#define DMA_CBR1_BNDT_Pos 0u
#define DMA_CBR1_BNDT_Msk 0x0000FFFFu
#define DMA_CFCR_TCF_Pos 8u
#define DMA_CFCR_SUSPF_Pos 13u
#define USART_CR1_TE 0x00000008u
#define USART_CR1_UE 0x00000001u
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

/* --- Core primitives for the opt-in low-power WFE path (OW_LOW_POWER).
 *     Host stubs mirroring the real CMSIS defines; compiled only into the
 *     low-power test build so the default busy-poll build stays byte-identical.
 *     C5 uses the split TIM1_UPD vector (IRQ 37, stm32c552xx.h). --- */
#if OW_LOW_POWER
#define TIM1_UPD_IRQn 37
#define SCB_SCR_SEVONPEND_Msk 0x00000010u
typedef struct {
    volatile uint32_t SCR;
} SCB_Type;
extern SCB_Type mock_scb;
#define SCB (&mock_scb)
#define __SEV() ((void)0)
#ifdef MOCK_WFE_HOOK
/* Route the sleep through a host hook so a test can model the update event
 * arriving asynchronously (mock_wfe_set_hook). Without the define this stays
 * the plain no-op and every existing low-power build is unchanged. */
void mock_wfe_hook(void);
#define __WFE() mock_wfe_hook()
#else
#define __WFE() ((void)0)
#endif
#define NVIC_ClearPendingIRQ(__IRQn) ((void)(__IRQn))
#endif /* OW_LOW_POWER */

#endif /* STM32C5XX_MOCK_H */
