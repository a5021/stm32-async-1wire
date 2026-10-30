#ifndef STM32H7XX_MOCK_H
#define STM32H7XX_MOCK_H
/* Host-build stand-in for the STM32H7 CMSIS device header.
 * Provides just enough register types, instance symbols and bit-field
 * constants for the DS18B20 driver, plus the compiler helpers it expects.
 * Register/bit names follow the real stm32h743xx.h spelling (DMA_Stream_TypeDef
 * with CR/NDTR/PAR/M0AR, DMA_SxCR_* positions, MODER/OTYPER/AFR, RCC_AHB1ENR,
 * RCC_AHB4ENR, RCC_APB2ENR) so tests catch wrong-symbol bugs.
 *
 * DMA streams: DMA1_Stream2 (feed, DMAMUX1 ch2 <- TIM1_CH2) and DMA1_Stream3
 * (capture, DMAMUX1 ch3 <- TIM1_CH4). The stream struct keeps the legacy
 * F1-style field aliases (CCR/CNDTR/CPAR/CMAR) in anonymous unions so the
 * target-agnostic hw_model and the register-level tests read one storage
 * through either spelling. Defines the family macro like the real stm32h7xx.h
 * so the ow_board.h PlatformIO/CubeMX fallback path is exercised on the host
 * too. */
#define STM32H7 1
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
    volatile uint32_t TISEL;
    volatile uint32_t AF1;
    volatile uint32_t AF2;
} TIM1_TypeDef;

/* DMA stream registers: real H7 spelling CR/NDTR/PAR/M0AR, with the legacy
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

/* DMA1 controller: interrupt/flag registers. The H7 port writes LIFCR only to
 * retire stream flags after disable (both streams live in the low group). */
typedef struct {
    volatile uint32_t LISR;
    volatile uint32_t HISR;
    volatile uint32_t LIFCR;
    volatile uint32_t HIFCR;
} DMA_TypeDef;

/* DMAMUX1 request router: the H7 backend programs channel 2 <- TIM1_CH2
 * (feed) and channel 3 <- TIM1_CH4 (capture) before each operation. */
typedef struct {
    volatile uint32_t CCR;
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
    volatile uint32_t CRRCR;
    volatile uint32_t CFGR;
    volatile uint32_t D1CFGR;
    volatile uint32_t D2CFGR;
    volatile uint32_t D3CFGR;
    volatile uint32_t PLLCKSELR;
    volatile uint32_t PLLCFGR;
    volatile uint32_t PLL1DIVR;
    volatile uint32_t AHB1ENR;
    volatile uint32_t AHB2ENR;
    volatile uint32_t AHB3ENR;
    volatile uint32_t AHB4ENR;
    volatile uint32_t APB1LENR;
    volatile uint32_t APB1HENR;
    volatile uint32_t APB2ENR;
    volatile uint32_t APB3ENR;
    volatile uint32_t APB4ENR;
} RCC_TypeDef;

/* Flash interface: only ACR is touched by configure_system_clock() latency
 * programming; the remaining registers keep the real layout for fidelity. */
typedef struct {
    volatile uint32_t ACR;
    volatile uint32_t KEYR;
    volatile uint32_t OPTKEYR;
    volatile uint32_t SR;
    volatile uint32_t CR;
    volatile uint32_t ECCSR;
} FLASH_TypeDef;

/* H7 power/supply registers used by the early clock bring-up. */
typedef struct {
    volatile uint32_t CR1;
    volatile uint32_t CSR1;
    volatile uint32_t CR2;
    volatile uint32_t CR3;
    volatile uint32_t CPUCR;
    volatile uint32_t D3CR;
} PWR_TypeDef;

typedef struct {
    volatile uint32_t PWRCR;
} SYSCFG_TypeDef;

typedef struct {
    volatile uint32_t IDCODE;
} DBGMCU_TypeDef;

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

/* Instances: pointers so the port core's (*TIM1), (*DMA1_Stream2), etc. work.
 * Feed rides DMA1_Stream2 (DMAMUX1 channel 2 <- TIM1_CH2), capture rides
 * DMA1_Stream3 (DMAMUX1 channel 3 <- TIM1_CH4). The storage objects keep
 * their legacy names so tests and hw_model stay target-agnostic. */
extern TIM1_TypeDef mock_tim1;
extern DMA1_Channel_TypeDef mock_dma1_ch4; /* DMA1_Stream3: CC4 capture */
extern DMA1_Channel_TypeDef mock_feed_ch; /* DMA1_Stream2: CC2 slot-end feed */
extern DMA_TypeDef mock_dma1;
extern DMAMUX_Channel_TypeDef mock_dmamux_ch2;
extern DMAMUX_Channel_TypeDef mock_dmamux_ch3;
extern GPIO_TypeDef mock_gpioa;
extern GPIO_TypeDef mock_gpioe;
extern RCC_TypeDef mock_rcc;
extern USART_TypeDef mock_usart1;
extern FLASH_TypeDef mock_flash;
extern PWR_TypeDef mock_pwr;
extern SYSCFG_TypeDef mock_syscfg;
extern DBGMCU_TypeDef mock_dbgmcu;
#define TIM1 (&mock_tim1)
#define DMA1_Stream2 (&mock_feed_ch) /* CC2 slot-end marker -> feeds CCR3 */
#define DMA1_Stream3 (&mock_dma1_ch4) /* CC4 capture -> drains CCR4 */
#define DMA1 (&mock_dma1)
#define DMAMUX1_Channel2 (&mock_dmamux_ch2)
#define DMAMUX1_Channel3 (&mock_dmamux_ch3)
#define GPIOA (&mock_gpioa)
#define GPIOE (&mock_gpioe)
#define RCC (&mock_rcc)
#define USART1 (&mock_usart1)
#define FLASH (&mock_flash)
#define PWR (&mock_pwr)
#define SYSCFG (&mock_syscfg)
#define DBGMCU (&mock_dbgmcu)

/* --- Bit-field constants used by the driver (H7 spellings, real values) --- */
#define RCC_AHB1ENR_DMA1EN 0x00000001u
#define RCC_AHB4ENR_GPIOAEN 0x00000001u
#define RCC_AHB4ENR_GPIOEEN 0x00000010u
#define RCC_APB2ENR_TIM1EN 0x00000001u
#define RCC_APB2ENR_USART1EN 0x00000010u
#define RCC_APB4ENR_SYSCFGEN 0x00000002u
#define RCC_CR_HSEON 0x00010000u
#define RCC_CR_HSERDY 0x00020000u
#define RCC_CR_HSIDIV 0x00000018u
#define RCC_CR_HSIDIV_1 0x00000000u
#define RCC_CR_PLL1ON 0x01000000u
#define RCC_CR_PLL1RDY 0x02000000u
#define RCC_PLLCKSELR_PLLSRC 0x00000003u
#define RCC_PLLCKSELR_PLLSRC_HSE 0x00000002u
#define RCC_PLLCKSELR_DIVM1_Pos 4u
#define RCC_PLLCKSELR_DIVM1 0x000003F0u
#define RCC_PLLCFGR_PLL1RGE_0 0x00000000u
#define RCC_PLLCFGR_PLL1RGE_1 0x00000004u
#define RCC_PLLCFGR_PLL1RGE_2 0x00000008u
#define RCC_PLLCFGR_DIVP1EN 0x00010000u
#define RCC_PLL1DIVR_N1_Pos 0u
#define RCC_PLL1DIVR_N1 0x000001FFu
#define RCC_PLL1DIVR_P1_Pos 9u
#define RCC_PLL1DIVR_P1 0x0000FE00u
#define RCC_CFGR_SW 0x00000007u
#define RCC_CFGR_SW_Msk 0x00000007u
#define RCC_CFGR_SW_PLL1 0x00000003u
#define RCC_CFGR_SWS_Msk 0x00000038u
#define RCC_CFGR_SWS_PLL1 0x00000018u
#define RCC_D1CFGR_HPRE 0x0000000Fu
#define RCC_D1CFGR_HPRE_DIV1 0x00000000u
#define RCC_D1CFGR_HPRE_DIV2 0x00000008u
#define RCC_D1CFGR_D1PPRE 0x00000070u
#define RCC_D1CFGR_D1PPRE_DIV2 0x00000040u
#define RCC_D1CFGR_D1CPRE 0x00000F00u
#define RCC_D1CFGR_D1CPRE_DIV1 0x00000000u
#define RCC_D2CFGR_D2PPRE1 0x00000070u
#define RCC_D2CFGR_D2PPRE1_DIV2 0x00000040u
#define RCC_D2CFGR_D2PPRE2 0x00000700u
#define RCC_D2CFGR_D2PPRE2_DIV2 0x00000400u
#define RCC_D3CFGR_D3PPRE 0x00000070u
#define RCC_D3CFGR_D3PPRE_DIV2 0x00000040u
#define PWR_D3CR_VOS 0x0000C000u
#define PWR_D3CR_VOS_0 0x00004000u
#define PWR_D3CR_VOS_1 0x00008000u
#define PWR_D3CR_VOSRDY 0x00002000u
#define PWR_CR3_SCUEN 0x00000004u
#define PWR_CR3_LDOEN 0x00000002u
#define PWR_CR3_BYPASS 0x00000001u
#define PWR_CSR1_ACTVOS 0x0000C000u
#define PWR_CSR1_ACTVOSRDY 0x00002000u
#define SYSCFG_PWRCR_ODEN 0x00000001u
#define DBGMCU_IDCODE_DEV_ID 0x00000FFFu
#define DBGMCU_IDCODE_REV_ID_Pos 16u
#define DBGMCU_IDCODE_REV_ID 0xFFFF0000u
#define STM32H7_DEV_ID 0x450u
#define FLASH_ACR_LATENCY_4WS 0x00000004u
#define GPIO_MODER_MODER10 0x00300000u
#define GPIO_MODER_MODER10_0 0x00100000u
#define GPIO_MODER_MODER10_1 0x00200000u
#define GPIO_MODER_MODER3 0x000000C0u
#define GPIO_MODER_MODER3_0 0x00000040u
#define GPIO_OTYPER_OT10 0x00000400u
#define GPIO_OTYPER_OT3 0x00000008u
#define GPIO_OSPEEDR_OSPEED10 0x00300000u
#define GPIO_OSPEEDR_OSPEED10_0 0x00100000u
#define GPIO_OSPEEDR_OSPEED10_1 0x00200000u
#define GPIO_OSPEEDR_OSPEED10_Pos 20u
#define GPIO_AFRH_AFSEL10 0x00000F00u
#define GPIO_AFRH_AFSEL10_Pos 8U
#define GPIO_AFRH_AFSEL9 0x000000F0u
#define GPIO_AFRH_AFSEL9_Pos 4U
#define GPIO_BSRR_BS3 0x00000008u
#define GPIO_BSRR_BR3 0x00080000u
#define USART_ISR_TXE_TXFNF 0x00000080u
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
/* DMA_SxCR bit positions (H7 streams share the F4 layout: no CHSEL). */
#define DMA_SxCR_EN 0x00000001u
#define DMA_SxCR_DIR_0 0x00000040u
#define DMA_SxCR_MINC 0x00000400u
#define DMA_SxCR_PSIZE_0 0x00000800u
#define DMA_SxCR_MSIZE_0 0x00002000u
#define DMA_SxCR_MSIZE_1 0x00004000u
/* Stream flag-clear bits: Stream2 lives in LIFCR at bits 12..17, Stream3 at
 * bits 18..23 ([FE, X, DME, TE, HT, TC] = [bit0..bit5] of each 6-bit group). */
#define DMA_LIFCR_CFEIF2 0x00001000u
#define DMA_LIFCR_CDMEIF2 0x00004000u
#define DMA_LIFCR_CTEIF2 0x00008000u
#define DMA_LIFCR_CHTIF2 0x00010000u
#define DMA_LIFCR_CTCIF2 0x00020000u
#define DMA_LIFCR_CFEIF3 0x00040000u
#define DMA_LIFCR_CDMEIF3 0x00100000u
#define DMA_LIFCR_CTEIF3 0x00200000u
#define DMA_LIFCR_CHTIF3 0x00400000u
#define DMA_LIFCR_CTCIF3 0x00800000u
/* Legacy F1-style aliases: the target-agnostic hw_model and the register-level
 * tests name the same H7 bits through these spellings (DIR maps to the low bit
 * of the two-bit F4/H7 direction field). */
#define DMA_CCR_EN DMA_SxCR_EN
#define DMA_CCR_DIR DMA_SxCR_DIR_0
#define DMA_CCR_MINC DMA_SxCR_MINC
#define DMA_CCR_PSIZE_0 DMA_SxCR_PSIZE_0
#define DMA_CCR_MSIZE_0 DMA_SxCR_MSIZE_0
#define DMA_CCR_MSIZE_1 DMA_SxCR_MSIZE_1
#define NVIC_ClearPendingIRQ(__IRQn) ((void)(__IRQn))

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
     Host stubs mirroring the real CMSIS defines; compiled only into the
     low-power test build so the default busy-poll build stays byte-identical.
     H7 maps OW_PORT_TIM1_UPD_IRQn to TIM1_UP_IRQn (a split vector, like H5).
     --- */
#if OW_LOW_POWER
#define TIM1_UP_IRQn 25
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
#endif

#endif /* STM32H7XX_MOCK_H */
