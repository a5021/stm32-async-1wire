# chips/f401xe.mk — STM32F401xE (F401CD/RD/VD/CE/RE/VE)
#
# Part identity ONLY. See chips/f103xb.mk for what does and does not belong
# here, and for the "no trailing comment on an assignment line" rule.
#
# The xE suffix is the CMSIS flash/pin-density code: 512KB flash / 128KB SRAM.
# That is the only thing separating this part from f401xc — same core, same
# 84MHz cap, same DMA/TIM1 mapping, but twice the memory, so it needs its own
# linker script rather than a different port. Same 84MHz clock default
# (8MHz HSE + PLL, M=8/N=168/P=2); SYSCLK_MHZ=16 remains the no-crystal
# fallback and still overrides it.
#
# Not validated on hardware: no F401xE part was available. The CMSIS device
# header, startup file and SVD are the real upstream ones and both builds
# compile and link, but treat the flashing and the 84MHz PLL as unproven.

# CMSIS device macro; selects stm32f401xe.h via stm32f4xx.h
CHIP_DEV_DEF = -DSTM32F401xE
CHIP_DEVICE_HDR = $(CMSIS_DEVICE_DIR)/stm32f401xe.h
CHIP_STARTUP = $(CMSIS_DEVICE_DIR)/startup_stm32f401xe.s
# 512KB flash / 128KB RAM
CHIP_LINKER = port/stm32f4/STM32F401RE_FLASH.ld
CHIP_JFLASH = port/stm32f4/stm32f401re.jflash
CHIP_JDEBUG = port/stm32f4/project-f401re.jdebug
CHIP_SVD = $(CMSIS_DEVICE_DIR)/STM32F401.svd
# 8MHz HSE + PLL (M=8, N=168, P=2)
CHIP_SYSCLK_MHZ = 84
