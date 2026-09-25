# chips/f401xc.mk — STM32F401xC (F401CB/CC/RB/RC/VB/VC, e.g. WeAct F401 Black Pill)
#
# Part identity ONLY. See chips/f103xb.mk for what does and does not belong
# here, and for the "no trailing comment on an assignment line" rule.
#
# The xC suffix is the CMSIS flash/pin-density code: 256KB flash / 64KB SRAM,
# which is why this part needs its own linker script rather than a different
# port. The 84MHz cap comes from the part, not the board: the F401 maxes out
# at 84MHz, so 8MHz HSE + PLL (M=8, N=168, P=2). SYSCLK_MHZ=16 is the
# no-crystal (HSI) fallback and still overrides it.

# CMSIS device macro; selects stm32f401xc.h via stm32f4xx.h
CHIP_DEV_DEF = -DSTM32F401xC
CHIP_DEVICE_HDR = $(CMSIS_DEVICE_DIR)/stm32f401xc.h
CHIP_STARTUP = $(CMSIS_DEVICE_DIR)/startup_stm32f401xc.s
# 256KB flash / 64KB RAM
CHIP_LINKER = port/stm32f4/STM32F401CC_FLASH.ld
CHIP_JFLASH = port/stm32f4/stm32f401cc.jflash
CHIP_JDEBUG = port/stm32f4/project-f401cc.jdebug
CHIP_SVD = $(CMSIS_DEVICE_DIR)/STM32F401.svd
# 8MHz HSE + PLL (M=8, N=168, P=2)
CHIP_SYSCLK_MHZ = 84
