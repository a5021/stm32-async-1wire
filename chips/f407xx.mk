# chips/f407xx.mk — STM32F407xx (F4DISCOVERY / MB997C, VG part)
#
# Part identity ONLY. See chips/f103xb.mk for what does and does not belong
# here, and for the "no trailing comment on an assignment line" rule.
#
# The F4 port backend is shared by the whole F4 family: TIM1/DMA2 and CHSEL=6
# map identically on every part listed here, so there is no per-part code in
# ow_port_f4.h — only memory sizes, the CMSIS device layer and the clock
# default differ. That is why the parts get a file each here rather than a
# subdirectory: the split is in the build matrix, not in the driver.

# CMSIS device macro; selects stm32f407xx.h via stm32f4xx.h
CHIP_DEV_DEF = -DSTM32F407xx
CHIP_DEVICE_HDR = $(CMSIS_DEVICE_DIR)/stm32f407xx.h
CHIP_STARTUP = $(CMSIS_DEVICE_DIR)/startup_stm32f407xx.s
# 1MB flash / 128KB RAM
CHIP_LINKER = port/stm32f4/STM32F407VGT6_FLASH.ld
CHIP_JFLASH = port/stm32f4/stm32f407vgt6.jflash
CHIP_JDEBUG = port/stm32f4/project.jdebug
CHIP_SVD = $(CMSIS_DEVICE_DIR)/STM32F407.svd
# 8MHz HSE + PLL; the 168MHz default is only valid on F405/F407-class parts
CHIP_SYSCLK_MHZ = 168
