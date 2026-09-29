# chips/f303xc.mk — STM32F303xC (STM32F3-DISCOVERY, MB1035B = F303VC)
#
# Part identity ONLY. See chips/f103xb.mk for what does and does not belong
# here, and for the "no trailing comment on an assignment line" rule.
#
# Naming follows the CMSIS device macro (stm32f3xx.h lists the valid device
# macros per family), lowercased: STM32F303xC -> f303xc.
#
# The F3 port backend is shared by the whole F3 family as it stands: TIM1 CH3 on
# PA10, the CH4 indirect capture off TI3, and the DMA1 request pair from
# RM0316 Table 78 (TIM1_CC2 -> channel 2, TIM1_CH4 -> channel 3) map the same
# way on the xB/C/D/E parts, so there is no per-part code in ow_port_f3.h - only
# memory sizes, the CMSIS device layer and the clock default differ.
#
# 72MHz, the part's ceiling. Not reachable from the internal RC: this family
# has HSI (8MHz) and no HSI16 at all, PLLSRC offers only HSI/2 (4MHz) or
# HSE/PREDIV, and PLLMUL tops out at 16 - so 4 x 16 = 64MHz is all the HSI path
# can reach and 72MHz needs the external oscillator. On this board that is the
# 8MHz signal the ST-LINK drives onto OSC_IN, which is a square wave rather than
# a crystal and so is taken in bypass mode (RCC_CR.HSEBYP). PREDIV=1 is its
# reset value, so 8 x 9 = 72MHz needs no CFGR2 write at all.
#
# APB1 /2 = 36MHz (the family maximum), APB2 /1 = 72MHz. TIM1 is on APB2, so
# with a prescaler of 1 the timer clock is PCLK2 = SYSCLK, which is what keeps
# the ow_port 1us-tick invariant and OW_PORT_TIM_PRESCALER at SYSCLK_MHZ - 1.
#
# The other two supported clocks are SYSCLK_MHZ=8 (raw HSI, which is what the
# part comes up on: RCC_CFGR.SW = 00 selects HSI undivided, the /2 is only on
# the path into the PLL) and SYSCLK_MHZ=64 (HSI/2 + PLLMUL16, the HSI ceiling).
# All three are run over the full seven-example matrix on this part.
#
# 256KB flash / 32KB SRAM, and no CCM RAM - the 1-Wire backend's DMA writes stay
# in the single contiguous SRAM block, so the linker script needs nothing more.

# CMSIS device macro; selects stm32f303xc.h via stm32f3xx.h
CHIP_DEV_DEF = -DSTM32F303xC
CHIP_DEVICE_HDR = $(CMSIS_DEVICE_DIR)/stm32f303xc.h
CHIP_STARTUP = $(CMSIS_DEVICE_DIR)/startup_stm32f303xc.s
# 256KB flash / 32KB SRAM
CHIP_LINKER = port/stm32f3/STM32F303XC_FLASH.ld
CHIP_JFLASH = port/stm32f3/stm32f303vc.jflash
CHIP_JDEBUG = port/stm32f3/project-f303vc.jdebug
CHIP_SVD = $(CMSIS_DEVICE_DIR)/STM32F303.svd
# Board oscillator, not part identity: the 8MHz comes from the ST-LINK MCO and is
# read in bypass mode, so it is the frequency the 72MHz default is built against.
# The 64MHz and 8MHz builds ignore the PLL input entirely and need no crystal.
CHIP_HSE_MHZ = 8
CHIP_SYSCLK_MHZ = 72
