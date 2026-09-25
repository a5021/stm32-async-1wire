# chips/g031xx.mk — STM32G031x6/G031x8 (Nucleo-G031R8, G031F6)
#
# Part identity ONLY. See chips/f103xb.mk for what does and does not belong
# here, and for the "no trailing comment on an assignment line" rule.

# CMSIS device macro; selects stm32g031xx.h via stm32g0xx.h
CHIP_DEV_DEF = -DSTM32G031xx
CHIP_DEVICE_HDR = $(CMSIS_DEVICE_DIR)/stm32g031xx.h
CHIP_STARTUP = $(CMSIS_DEVICE_DIR)/startup_stm32g031xx.s
# 32KB flash / 8KB RAM
CHIP_LINKER = port/stm32g0/STM32G031X6_FLASH.ld
CHIP_JFLASH = port/stm32g0/stm32g031f6.jflash
CHIP_JDEBUG = port/stm32g0/project.jdebug
CHIP_SVD = $(CMSIS_DEVICE_DIR)/STM32G031.svd
# HSI16 + PLL
CHIP_SYSCLK_MHZ = 64
