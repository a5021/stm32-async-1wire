/* Host-build stand-in for the STM32H7 CMSIS system header. */
#ifndef SYSTEM_STM32H7XX_MOCK_H
#define SYSTEM_STM32H7XX_MOCK_H

#include <stdint.h>

extern uint32_t SystemCoreClock;
extern uint32_t SystemD2Clock;
void SystemCoreClockUpdate(void);

#endif /* SYSTEM_STM32H7XX_MOCK_H */
