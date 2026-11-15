# chips/u585ci.mk - STM32U585CIU6 (bare UFQFPN48 part on the bench adapter)
#
# Part identity only. See chips/f103xb.mk for what does and does not belong
# here, and for the "no trailing comment on an assignment line" rule.
#
# Naming follows the CMSIS device macro (stm32u5xx.h lists the valid device
# macros per family), lowercased: STM32U585xx -> u585ci.
#
# The U5 backend is shared by the family: TIM1 CH3 on PA10, CH4 indirect
# capture off TI3, and GPDMA1 channels 2/3 with REQSEL 43/45. The part file
# carries the memory sizes, the CMSIS device layer and the clock default.
#
# 16MHz from HSI16, 1 flash wait state (VOS range 4 at reset tops out at
# 16MHz with 1WS). No crystal on the board, so there is no HSE default and no
# OW_HSE_MHZ. The reset MSI 4MHz is the fallback when the HSI16 start fails.
# APB1/2 stay /1, so TIM1 = PCLK2 = SYSCLK and the shared PSC =
# SYSCLK_MHZ - 1 rule still gives the 1us tick.
#
# 2MB flash / 768KB contiguous SRAM (SRAM1+2+3); SRAM4 is not used.
#
# The part has TrustZone; the build compiles with -mcmse (secure), so the
# CMSIS S aliases are used regardless of the TZEN option byte.
#
# The CMSIS startup file is assembly, and the GCC linker script is the
# project's own (port/stm32u5/), derived from the DFP stm32u585xx_flash.ld.

CHIP_DEV_DEF = -DSTM32U585xx
CHIP_DEVICE_HDR = $(CMSIS_DEVICE_DIR)/stm32u585xx.h
CHIP_STARTUP = $(CMSIS_DEVICE_DIR)/startup_stm32u585xx.s
CHIP_LINKER = port/stm32u5/STM32U585CI_FLASH.ld
CHIP_JFLASH =
CHIP_JDEBUG = port/stm32u5/project-u585ci.jdebug
CHIP_SVD = $(CMSIS_DEVICE_DIR)/STM32U585.svd
CHIP_SYSCLK_MHZ = 16
