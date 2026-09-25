# chips/f030x6.mk — STM32F030x6 (F030F4 board, 8-pin)
#
# Part identity ONLY. See chips/f103xb.mk for what does and does not belong
# here, and for the "no trailing comment on an assignment line" rule.

# CMSIS device macro; selects stm32f030x6.h via stm32f0xx.h
CHIP_DEV_DEF = -DSTM32F030x6
CHIP_DEVICE_HDR = $(CMSIS_DEVICE_DIR)/stm32f030x6.h
CHIP_STARTUP = $(CMSIS_DEVICE_DIR)/startup_stm32f030x6.s
# 16KB flash / 4KB RAM
CHIP_LINKER = port/stm32f0/STM32F030X6_FLASH.ld
CHIP_JFLASH = port/stm32f0/stm32f030f4.jflash
CHIP_JDEBUG = port/stm32f0/project.jdebug
CHIP_SVD = $(CMSIS_DEVICE_DIR)/STM32F030.svd
# HSI/2 + PLL x12
CHIP_SYSCLK_MHZ = 48
