# chips/g474cb.mk — STM32G474CBT6 (WeAct STM32G474CBT6 Long)
#
# Part identity ONLY. See chips/f103xb.mk for what does and does not belong
# here, and for the "no trailing comment on an assignment line" rule.

# CMSIS device macro; selects stm32g474xx.h via stm32g4xx.h
CHIP_DEV_DEF = -DSTM32G474xx
CHIP_DEVICE_HDR = $(CMSIS_DEVICE_DIR)/stm32g474xx.h
CHIP_STARTUP = $(CMSIS_DEVICE_DIR)/startup_stm32g474xx.s
# 128KB flash / 96KB SRAM (SRAM1 80K + SRAM2 16K, contiguous)
CHIP_LINKER = port/stm32g4/STM32G474CB_FLASH.ld
CHIP_JFLASH = port/stm32g4/stm32g474cb.jflash
CHIP_JDEBUG = port/stm32g4/project.jdebug
CHIP_SVD = $(CMSIS_DEVICE_DIR)/STM32G474xx.svd
# HSE 8MHz crystal + PLL to 170MHz
CHIP_HSE_MHZ = 8
CHIP_SYSCLK_MHZ = 170
