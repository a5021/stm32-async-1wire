/* Host-build stand-in for the STM32H7 CMSIS system header.
 * Declares what app.c needs for the H7 240MHz failure path: the live
 * SystemCoreClock variable (SystemCoreClockUpdate() accounts for HSIDIV, so
 * the fallback console baud stays readable whatever the reset divider is).
 * The real system_stm32h7xx.h declares both; this mock keeps the spelling so
 * a wrong declaration fails loudly instead of silently. */
#ifndef SYSTEM_STM32H7XX_MOCK_H
#define SYSTEM_STM32H7XX_MOCK_H

#include <stdint.h>

extern uint32_t SystemCoreClock;
void SystemCoreClockUpdate(void);

#endif /* SYSTEM_STM32H7XX_MOCK_H */
