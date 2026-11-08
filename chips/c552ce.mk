# chips/c552ce.mk - STM32C552CET6 (WeAct STM32C5 48-pin Core Board)
#
# Part identity only. See chips/f103xb.mk for what does and does not belong
# here, and for the "no trailing comment on an assignment line" rule.
#
# Naming follows the CMSIS device macro (stm32c5xx.h lists the valid device
# macros per family), lowercased: STM32C552xx -> c552ce.
#
# The C5 backend is shared by the family: TIM1 CH3 on PA10, CH4 indirect
# capture off TI3, and LPDMA1 channels 2/3 with REQSEL 25/27. The part file
# carries the memory sizes, the CMSIS device layer and the clock default.
#
# 144MHz, HSI direct. HSI runs at 144MHz on this family and the reset HSIDIV
# divides it to 48MHz; selecting HSIS gives the full 144MHz without starting
# a PLL or PSI. The 48MHz HSIDIV3 reset point and the raw 8MHz HSE crystal
# path are also implemented and bench-validated. APB1/2/3 stay /1, so
# TIM1 = PCLK2 = SYSCLK and the shared PSC = SYSCLK_MHZ - 1 rule still gives
# the 1us tick.
#
# 512KB flash / 128KB SRAM in one contiguous block.
#
# The CMSIS startup file is C, not assembly (see the C5 DFP), and the GCC
# linker script is derived from the DFP stm32c552xe_flash.ld.

CHIP_DEV_DEF = -DSTM32C552xx
CHIP_DEVICE_HDR = $(CMSIS_DEVICE_DIR)/stm32c552xx.h
CHIP_STARTUP = $(CMSIS_DEVICE_DIR)/startup_stm32c552xx.c
CHIP_LINKER = port/stm32c5/STM32C552CE_FLASH.ld
CHIP_JFLASH =
CHIP_JDEBUG = port/stm32c5/project-c552ce.jdebug
CHIP_SVD = $(CMSIS_DEVICE_DIR)/STM32C552.svd
CHIP_HSE_MHZ = 8
CHIP_SYSCLK_MHZ = 144
