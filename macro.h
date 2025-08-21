#ifndef __MACRO_H
#define __MACRO_H

#include "stm32f1xx.h"

#ifdef __cplusplus
 extern C {
#endif

#define NO  0
#define YES (!NO)

#define D11 (*DMA1_Channel1) /* 0x40020008 */
#define D12 (*DMA1_Channel2) /* 0x4002001C */
#define D13 (*DMA1_Channel3) /* 0x40020030 */
#define D14 (*DMA1_Channel4) /* 0x40020044 */
#define D15 (*DMA1_Channel5) /* 0x40020058 */
#define D16 (*DMA1_Channel6) /* 0x4002006C */
#define D17 (*DMA1_Channel7) /* 0x40020080 */

#define A1  (*ADC1)   /* 0x40012400 */
#define A2  (*ADC2)   /* 0x40012800 */
#define A   (*AFIO)   /* 0x40010000 */

#define B   (*BKP)    /* 0x40006C00 */
#define C   (*CRC)    /* 0x40023000 */
#define C1  (*CAN1)   /* 0x40006400 */
#define D   (*DBGMCU) /* 0xE0042000 */
#define E   (*EXTI)   /* 0x40010400 */
#define F   (*FLASH)  /* 0x40022000 */

#define PA  (*GPIOA)  /* 0x40010800 */
#define PB  (*GPIOB)  /* 0x40010C00 */
#define PC  (*GPIOC)  /* 0x40011000 */
#define PD  (*GPIOD)  /* 0x40011400 */

#define I1  (*I2C1)   /* 0x40005400 */
#define I2  (*I2C2)   /* 0x40005800 */
#define I   (*IWDG)   /* 0x40003000 */

#define O   (*OB)     /* 0x1FFFF800 */

#define P   (*PWR)    /* 0x40007000 */
#define R   (*RCC)    /* 0x40021000 */
#define RT  (*RTC)    /* 0x40002800 */

#define S1  (*SPI1)   /* 0x40013000 */
#define S2  (*SPI2)   /* 0x40003800 */

#define T1  (*TIM1)   /* 0x40012C00 */
#define T2  (*TIM2)   /* 0x40000000 */
#define T3  (*TIM3)   /* 0x40000400 */
#define T4  (*TIM4)   /* 0x40000800 */

#define U   (*USB)    /* 0x40005C00 */

#define U1  (*USART1) /* 0x40013800 */
#define U2  (*USART2) /* 0x40004400 */
#define U3  (*USART3) /* 0x40004800 */

#define W   (*WWDG)   /* 0x40002C00 */

#if 1
// --- Universal BITS Macro ---
// Helper macro: paste together PREFIX_, REG, and BF to form a bitfield macro name
#define _BITS_ONE(PREFIX_, REG, BF) PREFIX_##REG##_##BF

// Recursive macros to combine 1–16 bitfields using the '|' operator
#define _BITS_1(PREFIX_, REG, BF) _BITS_ONE(PREFIX_, REG, BF)
#define _BITS_2(PREFIX_, REG, BF, ...) _BITS_ONE(PREFIX_, REG, BF) | _BITS_1(PREFIX_, REG, __VA_ARGS__)
#define _BITS_3(PREFIX_, REG, BF, ...) _BITS_ONE(PREFIX_, REG, BF) | _BITS_2(PREFIX_, REG, __VA_ARGS__)
#define _BITS_4(PREFIX_, REG, BF, ...) _BITS_ONE(PREFIX_, REG, BF) | _BITS_3(PREFIX_, REG, __VA_ARGS__)
#define _BITS_5(PREFIX_, REG, BF, ...) _BITS_ONE(PREFIX_, REG, BF) | _BITS_4(PREFIX_, REG, __VA_ARGS__)
#define _BITS_6(PREFIX_, REG, BF, ...) _BITS_ONE(PREFIX_, REG, BF) | _BITS_5(PREFIX_, REG, __VA_ARGS__)
#define _BITS_7(PREFIX_, REG, BF, ...) _BITS_ONE(PREFIX_, REG, BF) | _BITS_6(PREFIX_, REG, __VA_ARGS__)
#define _BITS_8(PREFIX_, REG, BF, ...) _BITS_ONE(PREFIX_, REG, BF) | _BITS_7(PREFIX_, REG, __VA_ARGS__)
#define _BITS_9(PREFIX_, REG, BF, ...) _BITS_ONE(PREFIX_, REG, BF) | _BITS_8(PREFIX_, REG, __VA_ARGS__)
#define _BITS_10(PREFIX_, REG, BF, ...) _BITS_ONE(PREFIX_, REG, BF) | _BITS_9(PREFIX_, REG, __VA_ARGS__)
#define _BITS_11(PREFIX_, REG, BF, ...) _BITS_ONE(PREFIX_, REG, BF) | _BITS_10(PREFIX_, REG, __VA_ARGS__)
#define _BITS_12(PREFIX_, REG, BF, ...) _BITS_ONE(PREFIX_, REG, BF) | _BITS_11(PREFIX_, REG, __VA_ARGS__)
#define _BITS_13(PREFIX_, REG, BF, ...) _BITS_ONE(PREFIX_, REG, BF) | _BITS_12(PREFIX_, REG, __VA_ARGS__)
#define _BITS_14(PREFIX_, REG, BF, ...) _BITS_ONE(PREFIX_, REG, BF) | _BITS_13(PREFIX_, REG, __VA_ARGS__)
#define _BITS_15(PREFIX_, REG, BF, ...) _BITS_ONE(PREFIX_, REG, BF) | _BITS_14(PREFIX_, REG, __VA_ARGS__)
#define _BITS_16(PREFIX_, REG, BF, ...) _BITS_ONE(PREFIX_, REG, BF) | _BITS_15(PREFIX_, REG, __VA_ARGS__)

// Macro to count the number of variadic arguments (up to 16)
#define _BITS_NARG(...) _BITS_NARG_(__VA_ARGS__,16,15,14,13,12,11,10,9,8,7,6,5,4,3,2,1)
#define _BITS_NARG_(_1,_2,_3,_4,_5,_6,_7,_8,_9,_10,_11,_12,_13,_14,_15,_16,N,...) N

// Macro dispatcher: chooses the correct _BITS_n macro based on argument count
#define _BITS_CHOOSER2(count) _BITS_##count
#define _BITS_CHOOSER1(count) _BITS_CHOOSER2(count)
#define _BITS_CHOOSER(count)  _BITS_CHOOSER1(count)

// Main macro: expands to a mask of all requested bitfields for a given peripheral register
#define BITS(PREFIX_, REG, ...)     (_BITS_CHOOSER(_BITS_NARG(__VA_ARGS__))(PREFIX_, REG, __VA_ARGS__))

// --- Peripheral Type Wrappers ---
// These wrappers make it easy to use the BITS macro for each peripheral type.
// Example: RCC_BITS(APB2ENR, IOPAEN, AFIOEN) expands to (RCC_APB2ENR_IOPAEN | RCC_APB2ENR_AFIOEN)

#define RCC_BITS(reg, ...)      BITS(RCC_, reg, __VA_ARGS__)    // For RCC registers
#define GPIO_BITS(reg, ...)     BITS(GPIO_, reg, __VA_ARGS__)   // For GPIO registers
#define AFIO_BITS(reg, ...)     BITS(AFIO_, reg, __VA_ARGS__)   // For AFIO registers
#define TIM_BITS(reg, ...)      BITS(TIM_, reg, __VA_ARGS__)    // For TIM registers
#define USART_BITS(reg, ...)    BITS(USART_, reg, __VA_ARGS__)  // For USART registers
#define UART_BITS(reg, ...)     BITS(UART_, reg, __VA_ARGS__)   // For UART registers
#define SPI_BITS(reg, ...)      BITS(SPI_, reg, __VA_ARGS__)    // For SPI registers
#define I2C_BITS(reg, ...)      BITS(I2C_, reg, __VA_ARGS__)    // For I2C registers
#define CAN_BITS(reg, ...)      BITS(CAN_, reg, __VA_ARGS__)    // For CAN registers
#define ADC_BITS(reg, ...)      BITS(ADC_, reg, __VA_ARGS__)    // For ADC registers
#define DAC_BITS(reg, ...)      BITS(DAC_, reg, __VA_ARGS__)    // For DAC registers
#define CRC_BITS(reg, ...)      BITS(CRC_, reg, __VA_ARGS__)    // For CRC registers
#define USB_BITS(reg, ...)      BITS(USB_, reg, __VA_ARGS__)    // For USB registers
#define BKP_BITS(reg, ...)      BITS(BKP_, reg, __VA_ARGS__)    // For BKP registers
#define PWR_BITS(reg, ...)      BITS(PWR_, reg, __VA_ARGS__)    // For PWR registers
#define FLASH_BITS(reg, ...)    BITS(FLASH_, reg, __VA_ARGS__)  // For FLASH registers
#define DBGMCU_BITS(reg, ...)   BITS(DBGMCU_, reg, __VA_ARGS__) // For DBGMCU registers
#define WWDG_BITS(reg, ...)     BITS(WWDG_, reg, __VA_ARGS__)   // For WWDG registers
#define IWDG_BITS(reg, ...)     BITS(IWDG_, reg, __VA_ARGS__)   // For IWDG registers
#define RTC_BITS(reg, ...)      BITS(RTC_, reg, __VA_ARGS__)    // For RTC registers
#define EXTI_BITS(reg, ...)     BITS(EXTI_, reg, __VA_ARGS__)   // For EXTI registers
#define DMA_BITS(reg, ...)      BITS(DMA_, reg, __VA_ARGS__)    // For DMA registers

// === Register Shortcuts ===
// The following are convenient macros for common registers,
// reducing the need to repeat BITS(...) explicitly.
// Grouped per peripheral type for organization.

// --- RCC ---
#define RCC_CR(...)           BITS(RCC_, CR, __VA_ARGS__)
#define RCC_CFGR(...)         BITS(RCC_, CFGR, __VA_ARGS__)
#define RCC_CIR(...)          BITS(RCC_, CIR, __VA_ARGS__)
#define RCC_APB2RSTR(...)     BITS(RCC_, APB2RSTR, __VA_ARGS__)
#define RCC_APB1RSTR(...)     BITS(RCC_, APB1RSTR, __VA_ARGS__)
#define RCC_AHBENR(...)       BITS(RCC_, AHBENR, __VA_ARGS__)
#define RCC_APB2ENR(...)      BITS(RCC_, APB2ENR, __VA_ARGS__)
#define RCC_APB1ENR(...)      BITS(RCC_, APB1ENR, __VA_ARGS__)
#define RCC_BDCR(...)         BITS(RCC_, BDCR, __VA_ARGS__)
#define RCC_CSR(...)          BITS(RCC_, CSR, __VA_ARGS__)

// --- GPIO ---
#define GPIO_CRL(...)         BITS(GPIO_, CRL, __VA_ARGS__)
#define GPIO_CRH(...)         BITS(GPIO_, CRH, __VA_ARGS__)
#define GPIO_IDR(...)         BITS(GPIO_, IDR, __VA_ARGS__)
#define GPIO_ODR(...)         BITS(GPIO_, ODR, __VA_ARGS__)
#define GPIO_BSRR(...)        BITS(GPIO_, BSRR, __VA_ARGS__)
#define GPIO_BRR(...)         BITS(GPIO_, BRR, __VA_ARGS__)
#define GPIO_LCKR(...)        BITS(GPIO_, LCKR, __VA_ARGS__)

// --- AFIO ---
#define AFIO_EVCR(...)        BITS(AFIO_, EVCR, __VA_ARGS__)
#define AFIO_MAPR(...)        BITS(AFIO_, MAPR, __VA_ARGS__)
#define AFIO_EXTICR1(...)     BITS(AFIO_, EXTICR1, __VA_ARGS__)
#define AFIO_EXTICR2(...)     BITS(AFIO_, EXTICR2, __VA_ARGS__)
#define AFIO_EXTICR3(...)     BITS(AFIO_, EXTICR3, __VA_ARGS__)
#define AFIO_EXTICR4(...)     BITS(AFIO_, EXTICR4, __VA_ARGS__)
#define AFIO_MAPR2(...)       BITS(AFIO_, MAPR2, __VA_ARGS__)

// --- TIM (general purpose and advanced) ---
#define TIM_CR1(...)          BITS(TIM_, CR1, __VA_ARGS__)
#define TIM_CR2(...)          BITS(TIM_, CR2, __VA_ARGS__)
#define TIM_SMCR(...)         BITS(TIM_, SMCR, __VA_ARGS__)
#define TIM_DIER(...)         BITS(TIM_, DIER, __VA_ARGS__)
#define TIM_BDTR(...)         BITS(TIM_, BDTR, __VA_ARGS__)
#define TIM_SR(...)           BITS(TIM_, SR, __VA_ARGS__)
#define TIM_EGR(...)          BITS(TIM_, EGR, __VA_ARGS__)
#define TIM_CCMR1(...)        BITS(TIM_, CCMR1, __VA_ARGS__)
#define TIM_CCMR2(...)        BITS(TIM_, CCMR2, __VA_ARGS__)
#define TIM_CCER(...)         BITS(TIM_, CCER, __VA_ARGS__)
#define TIM_CNT(...)          BITS(TIM_, CNT, __VA_ARGS__)
#define TIM_PSC(...)          BITS(TIM_, PSC, __VA_ARGS__)
#define TIM_ARR(...)          BITS(TIM_, ARR, __VA_ARGS__)
#define TIM_CCR1(...)         BITS(TIM_, CCR1, __VA_ARGS__)
#define TIM_CCR2(...)         BITS(TIM_, CCR2, __VA_ARGS__)
#define TIM_CCR3(...)         BITS(TIM_, CCR3, __VA_ARGS__)
#define TIM_CCR4(...)         BITS(TIM_, CCR4, __VA_ARGS__)
#define TIM_DCR(...)          BITS(TIM_, DCR, __VA_ARGS__)
#define TIM_DMAR(...)         BITS(TIM_, DMAR, __VA_ARGS__)

// --- USART ---
#define USART_SR(...)         BITS(USART_, SR, __VA_ARGS__)
#define USART_DR(...)         BITS(USART_, DR, __VA_ARGS__)
#define USART_BRR(...)        BITS(USART_, BRR, __VA_ARGS__)
#define USART_CR1(...)        BITS(USART_, CR1, __VA_ARGS__)
#define USART_CR2(...)        BITS(USART_, CR2, __VA_ARGS__)
#define USART_CR3(...)        BITS(USART_, CR3, __VA_ARGS__)
#define USART_GTPR(...)       BITS(USART_, GTPR, __VA_ARGS__)

// --- SPI ---
#define SPI_CR1(...)          BITS(SPI_, CR1, __VA_ARGS__)
#define SPI_CR2(...)          BITS(SPI_, CR2, __VA_ARGS__)
#define SPI_SR(...)           BITS(SPI_, SR, __VA_ARGS__)
#define SPI_DR(...)           BITS(SPI_, DR, __VA_ARGS__)
#define SPI_CRCPR(...)        BITS(SPI_, CRCPR, __VA_ARGS__)
#define SPI_RXCRCR(...)       BITS(SPI_, RXCRCR, __VA_ARGS__)
#define SPI_TXCRCR(...)       BITS(SPI_, TXCRCR, __VA_ARGS__)
#define SPI_I2SCFGR(...)      BITS(SPI_, I2SCFGR, __VA_ARGS__)
#define SPI_I2SPR(...)        BITS(SPI_, I2SPR, __VA_ARGS__)

// --- I2C ---
#define I2C_CR1(...)          BITS(I2C_, CR1, __VA_ARGS__)
#define I2C_CR2(...)          BITS(I2C_, CR2, __VA_ARGS__)
#define I2C_OAR1(...)         BITS(I2C_, OAR1, __VA_ARGS__)
#define I2C_OAR2(...)         BITS(I2C_, OAR2, __VA_ARGS__)
#define I2C_DR(...)           BITS(I2C_, DR, __VA_ARGS__)
#define I2C_SR1(...)          BITS(I2C_, SR1, __VA_ARGS__)
#define I2C_SR2(...)          BITS(I2C_, SR2, __VA_ARGS__)
#define I2C_CCR(...)          BITS(I2C_, CCR, __VA_ARGS__)
#define I2C_TRISE(...)        BITS(I2C_, TRISE, __VA_ARGS__)

// --- ADC ---
#define ADC_SR(...)           BITS(ADC_, SR, __VA_ARGS__)
#define ADC_CR1(...)          BITS(ADC_, CR1, __VA_ARGS__)
#define ADC_CR2(...)          BITS(ADC_, CR2, __VA_ARGS__)
#define ADC_SMPR1(...)        BITS(ADC_, SMPR1, __VA_ARGS__)
#define ADC_SMPR2(...)        BITS(ADC_, SMPR2, __VA_ARGS__)
#define ADC_JOFR1(...)        BITS(ADC_, JOFR1, __VA_ARGS__)
#define ADC_JOFR2(...)        BITS(ADC_, JOFR2, __VA_ARGS__)
#define ADC_JOFR3(...)        BITS(ADC_, JOFR3, __VA_ARGS__)
#define ADC_JOFR4(...)        BITS(ADC_, JOFR4, __VA_ARGS__)
#define ADC_HTR(...)          BITS(ADC_, HTR, __VA_ARGS__)
#define ADC_LTR(...)          BITS(ADC_, LTR, __VA_ARGS__)
#define ADC_SQR1(...)         BITS(ADC_, SQR1, __VA_ARGS__)
#define ADC_SQR2(...)         BITS(ADC_, SQR2, __VA_ARGS__)
#define ADC_SQR3(...)         BITS(ADC_, SQR3, __VA_ARGS__)
#define ADC_JSQR(...)         BITS(ADC_, JSQR, __VA_ARGS__)
#define ADC_JDR1(...)         BITS(ADC_, JDR1, __VA_ARGS__)
#define ADC_JDR2(...)         BITS(ADC_, JDR2, __VA_ARGS__)
#define ADC_JDR3(...)         BITS(ADC_, JDR3, __VA_ARGS__)
#define ADC_JDR4(...)         BITS(ADC_, JDR4, __VA_ARGS__)
#define ADC_DR(...)           BITS(ADC_, DR, __VA_ARGS__)

// --- EXTI ---
#define EXTI_IMR(...)         BITS(EXTI_, IMR, __VA_ARGS__)
#define EXTI_EMR(...)         BITS(EXTI_, EMR, __VA_ARGS__)
#define EXTI_RTSR(...)        BITS(EXTI_, RTSR, __VA_ARGS__)
#define EXTI_FTSR(...)        BITS(EXTI_, FTSR, __VA_ARGS__)
#define EXTI_SWIER(...)       BITS(EXTI_, SWIER, __VA_ARGS__)
#define EXTI_PR(...)          BITS(EXTI_, PR, __VA_ARGS__)

// --- PWR ---
#define PWR_CR(...)           BITS(PWR_, CR, __VA_ARGS__)
#define PWR_CSR(...)          BITS(PWR_, CSR, __VA_ARGS__)

// --- FLASH ---
#define FLASH_ACR(...)        BITS(FLASH_, ACR, __VA_ARGS__)
#define FLASH_KEYR(...)       BITS(FLASH_, KEYR, __VA_ARGS__)
#define FLASH_OPTKEYR(...)    BITS(FLASH_, OPTKEYR, __VA_ARGS__)
#define FLASH_SR(...)         BITS(FLASH_, SR, __VA_ARGS__)
#define FLASH_CR(...)         BITS(FLASH_, CR, __VA_ARGS__)
#define FLASH_AR(...)         BITS(FLASH_, AR, __VA_ARGS__)
#define FLASH_OBR(...)        BITS(FLASH_, OBR, __VA_ARGS__)
#define FLASH_WRPR(...)       BITS(FLASH_, WRPR, __VA_ARGS__)

// --- BKP ---
#define BKP_RTCCR(...)        BITS(BKP_, RTCCR, __VA_ARGS__)
#define BKP_CR(...)           BITS(BKP_, CR, __VA_ARGS__)
#define BKP_CSR(...)          BITS(BKP_, CSR, __VA_ARGS__)

// --- CRC ---
#define CRC_DR(...)           BITS(CRC_, DR, __VA_ARGS__)
#define CRC_IDR(...)          BITS(CRC_, IDR, __VA_ARGS__)
#define CRC_CR(...)           BITS(CRC_, CR, __VA_ARGS__)

// --- WWDG ---
#define WWDG_CR(...)          BITS(WWDG_, CR, __VA_ARGS__)
#define WWDG_CFR(...)         BITS(WWDG_, CFR, __VA_ARGS__)
#define WWDG_SR(...)          BITS(WWDG_, SR, __VA_ARGS__)

// --- IWDG ---
#define IWDG_KR(...)          BITS(IWDG_, KR, __VA_ARGS__)
#define IWDG_PR(...)          BITS(IWDG_, PR, __VA_ARGS__)
#define IWDG_RLR(...)         BITS(IWDG_, RLR, __VA_ARGS__)
#define IWDG_SR(...)          BITS(IWDG_, SR, __VA_ARGS__)

// --- RTC ---
#define RTC_CRH(...)          BITS(RTC_, CRH, __VA_ARGS__)
#define RTC_CRL(...)          BITS(RTC_, CRL, __VA_ARGS__)
#define RTC_PRLH(...)         BITS(RTC_, PRLH, __VA_ARGS__)
#define RTC_PRLL(...)         BITS(RTC_, PRLL, __VA_ARGS__)
#define RTC_DIVH(...)         BITS(RTC_, DIVH, __VA_ARGS__)
#define RTC_DIVL(...)         BITS(RTC_, DIVL, __VA_ARGS__)
#define RTC_CNTH(...)         BITS(RTC_, CNTH, __VA_ARGS__)
#define RTC_CNTL(...)         BITS(RTC_, CNTL, __VA_ARGS__)
#define RTC_ALRH(...)         BITS(RTC_, ALRH, __VA_ARGS__)
#define RTC_ALRL(...)         BITS(RTC_, ALRL, __VA_ARGS__)

// --- DMA Channel Registers (common to all channels) ---
#define DMA_CCR(...)           BITS(DMA_, CCR, __VA_ARGS__)
#define DMA_CNDTR(...)         BITS(DMA_, CNDTR, __VA_ARGS__)
#define DMA_CPAR(...)          BITS(DMA_, CPAR, __VA_ARGS__)
#define DMA_CMAR(...)          BITS(DMA_, CMAR, __VA_ARGS__)

// --- DMA Interrupt and Status Registers ---
#define DMA_ISR(...)           BITS(DMA_, ISR, __VA_ARGS__)
#define DMA_IFCR(...)          BITS(DMA_, IFCR, __VA_ARGS__)

#else
// Helper macro to concatenate tokens
#define CONCAT(a, b) a##b
#define CONCAT3(a, b, c) a##b##_##c

// Main macro to generate bitfield masks
#define BITS(PREFIX, REG, ...) \
    (0 | CONCAT(_BITS_, _NARG(__VA_ARGS__))(PREFIX, REG, __VA_ARGS__))

// Helper macros to count the number of arguments
#define _NARG(...) _NARG_(__VA_ARGS__, 16, 15, 14, 13, 12, 11, 10, 9, 8, 7, 6, 5, 4, 3, 2, 1)
#define _NARG_(...) _NARG__(__VA_ARGS__)
#define _NARG__(_1, _2, _3, _4, _5, _6, _7, _8, _9, _10, _11, _12, _13, _14, _15, _16, N, ...) N

// Macros to handle different numbers of arguments
#define _BITS_1(PREFIX, REG, BF) CONCAT3(PREFIX, REG, BF)
#define _BITS_2(PREFIX, REG, BF, ...) _BITS_1(PREFIX, REG, BF) | _BITS_1(PREFIX, REG, __VA_ARGS__)
#define _BITS_3(PREFIX, REG, BF, ...) _BITS_1(PREFIX, REG, BF) | _BITS_2(PREFIX, REG, __VA_ARGS__)
#define _BITS_4(PREFIX, REG, BF, ...) _BITS_1(PREFIX, REG, BF) | _BITS_3(PREFIX, REG, __VA_ARGS__)
#define _BITS_5(PREFIX, REG, BF, ...) _BITS_1(PREFIX, REG, BF) | _BITS_4(PREFIX, REG, __VA_ARGS__)
#define _BITS_6(PREFIX, REG, BF, ...) _BITS_1(PREFIX, REG, BF) | _BITS_5(PREFIX, REG, __VA_ARGS__)
#define _BITS_7(PREFIX, REG, BF, ...) _BITS_1(PREFIX, REG, BF) | _BITS_6(PREFIX, REG, __VA_ARGS__)
#define _BITS_8(PREFIX, REG, BF, ...) _BITS_1(PREFIX, REG, BF) | _BITS_7(PREFIX, REG, __VA_ARGS__)
#define _BITS_9(PREFIX, REG, BF, ...) _BITS_1(PREFIX, REG, BF) | _BITS_8(PREFIX, REG, __VA_ARGS__)
#define _BITS_10(PREFIX, REG, BF, ...) _BITS_1(PREFIX, REG, BF) | _BITS_9(PREFIX, REG, __VA_ARGS__)
#define _BITS_11(PREFIX, REG, BF, ...) _BITS_1(PREFIX, REG, BF) | _BITS_10(PREFIX, REG, __VA_ARGS__)
#define _BITS_12(PREFIX, REG, BF, ...) _BITS_1(PREFIX, REG, BF) | _BITS_11(PREFIX, REG, __VA_ARGS__)
#define _BITS_13(PREFIX, REG, BF, ...) _BITS_1(PREFIX, REG, BF) | _BITS_12(PREFIX, REG, __VA_ARGS__)
#define _BITS_14(PREFIX, REG, BF, ...) _BITS_1(PREFIX, REG, BF) | _BITS_13(PREFIX, REG, __VA_ARGS__)
#define _BITS_15(PREFIX, REG, BF, ...) _BITS_1(PREFIX, REG, BF) | _BITS_14(PREFIX, REG, __VA_ARGS__)
#define _BITS_16(PREFIX, REG, BF, ...) _BITS_1(PREFIX, REG, BF) | _BITS_15(PREFIX, REG, __VA_ARGS__)

// Peripheral-specific wrappers
#define RCC_BITS(reg, ...) BITS(RCC_, reg, __VA_ARGS__)
#define GPIO_BITS(reg, ...) BITS(GPIO_, reg, __VA_ARGS__)
#define AFIO_BITS(reg, ...) BITS(AFIO_, reg, __VA_ARGS__)
#define TIM_BITS(reg, ...) BITS(TIM_, reg, __VA_ARGS__)
#define USART_BITS(reg, ...) BITS(USART_, reg, __VA_ARGS__)
#define UART_BITS(reg, ...) BITS(UART_, reg, __VA_ARGS__)
#define SPI_BITS(reg, ...) BITS(SPI_, reg, __VA_ARGS__)
#define I2C_BITS(reg, ...) BITS(I2C_, reg, __VA_ARGS__)
#define CAN_BITS(reg, ...) BITS(CAN_, reg, __VA_ARGS__)
#define ADC_BITS(reg, ...) BITS(ADC_, reg, __VA_ARGS__)
#define DAC_BITS(reg, ...) BITS(DAC_, reg, __VA_ARGS__)
#define CRC_BITS(reg, ...) BITS(CRC_, reg, __VA_ARGS__)
#define USB_BITS(reg, ...) BITS(USB_, reg, __VA_ARGS__)
#define BKP_BITS(reg, ...) BITS(BKP_, reg, __VA_ARGS__)
#define PWR_BITS(reg, ...) BITS(PWR_, reg, __VA_ARGS__)
#define FLASH_BITS(reg, ...) BITS(FLASH_, reg, __VA_ARGS__)
#define DBGMCU_BITS(reg, ...) BITS(DBGMCU_, reg, __VA_ARGS__)
#define WWDG_BITS(reg, ...) BITS(WWDG_, reg, __VA_ARGS__)
#define IWDG_BITS(reg, ...) BITS(IWDG_, reg, __VA_ARGS__)
#define RTC_BITS(reg, ...) BITS(RTC_, reg, __VA_ARGS__)
#define EXTI_BITS(reg, ...) BITS(EXTI_, reg, __VA_ARGS__)
#define DMA_BITS(reg, ...) BITS(DMA_, reg, __VA_ARGS__)

// Register shortcuts for RCC
#define RCC_CR(...) RCC_BITS(CR, __VA_ARGS__)
#define RCC_CFGR(...) RCC_BITS(CFGR, __VA_ARGS__)
#define RCC_CIR(...) RCC_BITS(CIR, __VA_ARGS__)
#define RCC_APB2RSTR(...) RCC_BITS(APB2RSTR, __VA_ARGS__)
#define RCC_APB1RSTR(...) RCC_BITS(APB1RSTR, __VA_ARGS__)
#define RCC_AHBENR(...) RCC_BITS(AHBENR, __VA_ARGS__)
#define RCC_APB2ENR(...) RCC_BITS(APB2ENR, __VA_ARGS__)
#define RCC_APB1ENR(...) RCC_BITS(APB1ENR, __VA_ARGS__)
#define RCC_BDCR(...) RCC_BITS(BDCR, __VA_ARGS__)
#define RCC_CSR(...) RCC_BITS(CSR, __VA_ARGS__)

// Register shortcuts for GPIO
#define GPIO_CRL(...) GPIO_BITS(CRL, __VA_ARGS__)
#define GPIO_CRH(...) GPIO_BITS(CRH, __VA_ARGS__)
#define GPIO_IDR(...) GPIO_BITS(IDR, __VA_ARGS__)
#define GPIO_ODR(...) GPIO_BITS(ODR, __VA_ARGS__)
#define GPIO_BSRR(...) GPIO_BITS(BSRR, __VA_ARGS__)
#define GPIO_BRR(...) GPIO_BITS(BRR, __VA_ARGS__)
#define GPIO_LCKR(...) GPIO_BITS(LCKR, __VA_ARGS__)

// Register shortcuts for AFIO
#define AFIO_EVCR(...) AFIO_BITS(EVCR, __VA_ARGS__)
#define AFIO_MAPR(...) AFIO_BITS(MAPR, __VA_ARGS__)
#define AFIO_EXTICR1(...) AFIO_BITS(EXTICR1, __VA_ARGS__)
#define AFIO_EXTICR2(...) AFIO_BITS(EXTICR2, __VA_ARGS__)
#define AFIO_EXTICR3(...) AFIO_BITS(EXTICR3, __VA_ARGS__)
#define AFIO_EXTICR4(...) AFIO_BITS(EXTICR4, __VA_ARGS__)
#define AFIO_MAPR2(...) AFIO_BITS(MAPR2, __VA_ARGS__)

// Register shortcuts for TIM
#define TIM_CR1(...) TIM_BITS(CR1, __VA_ARGS__)
#define TIM_CR2(...) TIM_BITS(CR2, __VA_ARGS__)
#define TIM_SMCR(...) TIM_BITS(SMCR, __VA_ARGS__)
#define TIM_DIER(...) TIM_BITS(DIER, __VA_ARGS__)
#define TIM_SR(...) TIM_BITS(SR, __VA_ARGS__)
#define TIM_EGR(...) TIM_BITS(EGR, __VA_ARGS__)
#define TIM_CCMR1(...) TIM_BITS(CCMR1, __VA_ARGS__)
#define TIM_CCMR2(...) TIM_BITS(CCMR2, __VA_ARGS__)
#define TIM_CCER(...) TIM_BITS(CCER, __VA_ARGS__)
#define TIM_CNT(...) TIM_BITS(CNT, __VA_ARGS__)
#define TIM_PSC(...) TIM_BITS(PSC, __VA_ARGS__)
#define TIM_ARR(...) TIM_BITS(ARR, __VA_ARGS__)
#define TIM_CCR1(...) TIM_BITS(CCR1, __VA_ARGS__)
#define TIM_CCR2(...) TIM_BITS(CCR2, __VA_ARGS__)
#define TIM_CCR3(...) TIM_BITS(CCR3, __VA_ARGS__)
#define TIM_CCR4(...) TIM_BITS(CCR4, __VA_ARGS__)
#define TIM_DCR(...) TIM_BITS(DCR, __VA_ARGS__)
#define TIM_DMAR(...) TIM_BITS(DMAR, __VA_ARGS__)

// Register shortcuts for USART
#define USART_SR(...) USART_BITS(SR, __VA_ARGS__)
#define USART_DR(...) USART_BITS(DR, __VA_ARGS__)
#define USART_BRR(...) USART_BITS(BRR, __VA_ARGS__)
#define USART_CR1(...) USART_BITS(CR1, __VA_ARGS__)
#define USART_CR2(...) USART_BITS(CR2, __VA_ARGS__)
#define USART_CR3(...) USART_BITS(CR3, __VA_ARGS__)
#define USART_GTPR(...) USART_BITS(GTPR, __VA_ARGS__)

// Register shortcuts for SPI
#define SPI_CR1(...) SPI_BITS(CR1, __VA_ARGS__)
#define SPI_CR2(...) SPI_BITS(CR2, __VA_ARGS__)
#define SPI_SR(...) SPI_BITS(SR, __VA_ARGS__)
#define SPI_DR(...) SPI_BITS(DR, __VA_ARGS__)
#define SPI_CRCPR(...) SPI_BITS(CRCPR, __VA_ARGS__)
#define SPI_RXCRCR(...) SPI_BITS(RXCRCR, __VA_ARGS__)
#define SPI_TXCRCR(...) SPI_BITS(TXCRCR, __VA_ARGS__)
#define SPI_I2SCFGR(...) SPI_BITS(I2SCFGR, __VA_ARGS__)
#define SPI_I2SPR(...) SPI_BITS(I2SPR, __VA_ARGS__)

// Register shortcuts for I2C
#define I2C_CR1(...) I2C_BITS(CR1, __VA_ARGS__)
#define I2C_CR2(...) I2C_BITS(CR2, __VA_ARGS__)
#define I2C_OAR1(...) I2C_BITS(OAR1, __VA_ARGS__)
#define I2C_OAR2(...) I2C_BITS(OAR2, __VA_ARGS__)
#define I2C_DR(...) I2C_BITS(DR, __VA_ARGS__)
#define I2C_SR1(...) I2C_BITS(SR1, __VA_ARGS__)
#define I2C_SR2(...) I2C_BITS(SR2, __VA_ARGS__)
#define I2C_CCR(...) I2C_BITS(CCR, __VA_ARGS__)
#define I2C_TRISE(...) I2C_BITS(TRISE, __VA_ARGS__)

// Register shortcuts for ADC
#define ADC_SR(...) ADC_BITS(SR, __VA_ARGS__)
#define ADC_CR1(...) ADC_BITS(CR1, __VA_ARGS__)
#define ADC_CR2(...) ADC_BITS(CR2, __VA_ARGS__)
#define ADC_SMPR1(...) ADC_BITS(SMPR1, __VA_ARGS__)
#define ADC_SMPR2(...) ADC_BITS(SMPR2, __VA_ARGS__)
#define ADC_JOFR1(...) ADC_BITS(JOFR1, __VA_ARGS__)
#define ADC_JOFR2(...) ADC_BITS(JOFR2, __VA_ARGS__)
#define ADC_JOFR3(...) ADC_BITS(JOFR3, __VA_ARGS__)
#define ADC_JOFR4(...) ADC_BITS(JOFR4, __VA_ARGS__)
#define ADC_HTR(...) ADC_BITS(HTR, __VA_ARGS__)
#define ADC_LTR(...) ADC_BITS(LTR, __VA_ARGS__)
#define ADC_SQR1(...) ADC_BITS(SQR1, __VA_ARGS__)
#define ADC_SQR2(...) ADC_BITS(SQR2, __VA_ARGS__)
#define ADC_SQR3(...) ADC_BITS(SQR3, __VA_ARGS__)
#define ADC_JSQR(...) ADC_BITS(JSQR, __VA_ARGS__)
#define ADC_JDR1(...) ADC_BITS(JDR1, __VA_ARGS__)
#define ADC_JDR2(...) ADC_BITS(JDR2, __VA_ARGS__)
#define ADC_JDR3(...) ADC_BITS(JDR3, __VA_ARGS__)
#define ADC_JDR4(...) ADC_BITS(JDR4, __VA_ARGS__)
#define ADC_DR(...) ADC_BITS(DR, __VA_ARGS__)

// Register shortcuts for EXTI
#define EXTI_IMR(...) EXTI_BITS(IMR, __VA_ARGS__)
#define EXTI_EMR(...) EXTI_BITS(EMR, __VA_ARGS__)
#define EXTI_RTSR(...) EXTI_BITS(RTSR, __VA_ARGS__)
#define EXTI_FTSR(...) EXTI_BITS(FTSR, __VA_ARGS__)
#define EXTI_SWIER(...) EXTI_BITS(SWIER, __VA_ARGS__)
#define EXTI_PR(...) EXTI_BITS(PR, __VA_ARGS__)

// Register shortcuts for PWR
#define PWR_CR(...) PWR_BITS(CR, __VA_ARGS__)
#define PWR_CSR(...) PWR_BITS(CSR, __VA_ARGS__)

// Register shortcuts for FLASH
#define FLASH_ACR(...) FLASH_BITS(ACR, __VA_ARGS__)
#define FLASH_KEYR(...) FLASH_BITS(KEYR, __VA_ARGS__)
#define FLASH_OPTKEYR(...) FLASH_BITS(OPTKEYR, __VA_ARGS__)
#define FLASH_SR(...) FLASH_BITS(SR, __VA_ARGS__)
#define FLASH_CR(...) FLASH_BITS(CR, __VA_ARGS__)
#define FLASH_AR(...) FLASH_BITS(AR, __VA_ARGS__)
#define FLASH_OBR(...) FLASH_BITS(OBR, __VA_ARGS__)
#define FLASH_WRPR(...) FLASH_BITS(WRPR, __VA_ARGS__)

// Register shortcuts for BKP
#define BKP_RTCCR(...) BKP_BITS(RTCCR, __VA_ARGS__)
#define BKP_CR(...) BKP_BITS(CR, __VA_ARGS__)
#define BKP_CSR(...) BKP_BITS(CSR, __VA_ARGS__)

// Register shortcuts for CRC
#define CRC_DR(...) CRC_BITS(DR, __VA_ARGS__)
#define CRC_IDR(...) CRC_BITS(IDR, __VA_ARGS__)
#define CRC_CR(...) CRC_BITS(CR, __VA_ARGS__)

// Register shortcuts for WWDG
#define WWDG_CR(...) WWDG_BITS(CR, __VA_ARGS__)
#define WWDG_CFR(...) WWDG_BITS(CFR, __VA_ARGS__)
#define WWDG_SR(...) WWDG_BITS(SR, __VA_ARGS__)

// Register shortcuts for IWDG
#define IWDG_KR(...) IWDG_BITS(KR, __VA_ARGS__)
#define IWDG_PR(...) IWDG_BITS(PR, __VA_ARGS__)
#define IWDG_RLR(...) IWDG_BITS(RLR, __VA_ARGS__)
#define IWDG_SR(...) IWDG_BITS(SR, __VA_ARGS__)

// Register shortcuts for RTC
#define RTC_CRH(...) RTC_BITS(CRH, __VA_ARGS__)
#define RTC_CRL(...) RTC_BITS(CRL, __VA_ARGS__)
#define RTC_PRLH(...) RTC_BITS(PRLH, __VA_ARGS__)
#define RTC_PRLL(...) RTC_BITS(PRLL, __VA_ARGS__)
#define RTC_DIVH(...) RTC_BITS(DIVH, __VA_ARGS__)
#define RTC_DIVL(...) RTC_BITS(DIVL, __VA_ARGS__)
#define RTC_CNTH(...) RTC_BITS(CNTH, __VA_ARGS__)
#define RTC_CNTL(...) RTC_BITS(CNTL, __VA_ARGS__)
#define RTC_ALRH(...) RTC_BITS(ALRH, __VA_ARGS__)
#define RTC_ALRL(...) RTC_BITS(ALRL, __VA_ARGS__)

// Register shortcuts for DMA
#define DMA_CCR(...) DMA_BITS(CCR, __VA_ARGS__)
#define DMA_CNDTR(...) DMA_BITS(CNDTR, __VA_ARGS__)
#define DMA_CPAR(...) DMA_BITS(CPAR, __VA_ARGS__)
#define DMA_CMAR(...) DMA_BITS(CMAR, __VA_ARGS__)
#define DMA_ISR(...) DMA_BITS(ISR, __VA_ARGS__)
#define DMA_IFCR(...) DMA_BITS(IFCR, __VA_ARGS__)

#endif

#if defined(__GNUC__) && ! defined(__clang__)
  //void _close_r(void){} void _close(void){} void _lseek_r(void){} void _lseek(void){} void _read_r(void){} void _read(void){} void _write_r(void){}

  // Stubs to suppress newlib-nano warnings
    int _close(int file) {
        (void)file;
        return -1;
    }
    
    int _lseek(int file, int ptr, int dir) {
        (void)file;
        (void)ptr;
        (void)dir;
        return -1;
    }
    
    int _read(int file, char *ptr, int len) {
        (void)file;
        (void)ptr;
        (void)len;
        return -1;
    }
    
    int _write(int file, char *ptr, int len) {
        (void)file;
        (void)ptr;
        (void)len;
        return -1;
    }

#endif


#ifdef __cplusplus
}
#endif

#endif /* __MACRO_H */
