# chips/l476rg.mk — STM32L476RGT6 (NUCLEO-L476RG)
#
# Part identity ONLY. See chips/f103xb.mk for what does and does not belong
# here, and for the "no trailing comment on an assignment line" rule.
#
# The L4 port backend is shared by the classic-L4 family as it stands: TIM1 CH3
# on PA10 (AF1), the CH4 indirect capture off TI3, and the classic DMA1 channels
# 3/4 with their request selected through DMA1_CSELR. 1MB flash and 96KB SRAM1
# are the only per-part facts, so no per-part code lives in ow_mcu.h.
#
# HSE is deliberately absent: the NUCLEO-L476RG has no crystal populated, so
# the clock tree is HSI16 only. 80MHz is HSI16 (16MHz) / M=1 x N=10 / R=2 with
# voltage Range 1 and 4 flash wait states; 16MHz is raw HSI16 (no PLL). The
# 32KB SRAM2 at 0x10000000 is left out of the linker because the engine needs
# one contiguous DMA-reachable region and SRAM1 is enough.

# CMSIS device macro; selects stm32l476xx.h via stm32l4xx.h
CHIP_DEV_DEF = -DSTM32L476xx
CHIP_DEVICE_HDR = $(CMSIS_DEVICE_DIR)/stm32l476xx.h
CHIP_STARTUP = $(CMSIS_DEVICE_DIR)/startup_stm32l476xx.s
# 1MB flash / 96KB SRAM1 (SRAM2 is a separate 32KB block and is not used)
CHIP_LINKER = port/stm32l4/STM32L476RGTX_FLASH.ld
CHIP_JFLASH =
CHIP_JDEBUG = port/stm32l4/project-l476rg.jdebug
CHIP_SVD = $(CMSIS_DEVICE_DIR)/STM32L4x6.svd
# No HSE: the NUCLEO-L476RG crystal is not populated; HSI16 only.
# Default build clock. Override per build with SYSCLK_MHZ= (80 or 16).
CHIP_SYSCLK_MHZ = 80
