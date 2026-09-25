# chips/f103xb.mk — STM32F103xB (Blue Pill, C8/CB)
#
# Part identity ONLY. Anything shared by the whole family — MCU flags, the
# 1-Wire pin and timer/DMA mapping, the port header itself — lives in the
# OW_TARGET family block of the Makefile. Do not duplicate it here.
#
# Naming follows the CMSIS device macro (stm32f4xx.h lists the valid device
# macros per family), lowercased: STM32F103xB -> f103xb.
#
# Note: do not put a comment at the end of an assignment line. Make keeps the
# whitespace in front of '#' as part of the value, which silently breaks every
# path below. `make test-chips` exists to catch exactly that class of typo.

# CMSIS device macro; selects stm32f103xb.h via stm32f1xx.h
CHIP_DEV_DEF = -DSTM32F103xB
CHIP_DEVICE_HDR = $(CMSIS_DEVICE_DIR)/stm32f103xb.h
CHIP_STARTUP = $(CMSIS_DEVICE_DIR)/startup_stm32f103xb.s
# 64KB flash / 20KB RAM
CHIP_LINKER = port/stm32f1/STM32F103XB_FLASH.ld
CHIP_JFLASH = port/stm32f1/stm32f103cb.jflash
CHIP_JDEBUG = port/stm32f1/project.jdebug
CHIP_SVD = $(CMSIS_DEVICE_DIR)/STM32F103xx.svd
# 8MHz HSE + PLL x9
CHIP_SYSCLK_MHZ = 72
