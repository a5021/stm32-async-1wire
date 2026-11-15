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
# 160MHz from the 25MHz HSE through PLL1: M=5 (5MHz input, 4..8MHz RGE),
# N=64 (320MHz VCO), R=2 (SYSCLK = PLL1R). VOS range 1 + EPOD booster, 4 flash
# wait states. PLL1MBOOST=DIV2 (12.5MHz EPOD input) is bench-validated.
# The 16MHz HSI16 path (1 WS) is the fallback; a 4MHz build is not supported
# (the polled read path returns CRC failures at that clock).
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
CHIP_HSE_MHZ = 25
CHIP_SYSCLK_MHZ = 160
