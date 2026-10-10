# chips/h503cb.mk — STM32H503CBT6 (WeAct STM32H503Cx Core Board)
#
# Part identity ONLY. See chips/f103xb.mk for what does and does not belong
# here, and for the "no trailing comment on an assignment line" rule.
#
# Naming follows the CMSIS device macro (stm32h5xx.h lists the valid device
# macros per family), lowercased: STM32H503xx -> h503cb.
#
# The H5 port backend is shared by the whole H5 family as it stands: TIM1 CH3 on
# PA10, the CH4 indirect capture off TI3, and the GPDMA1 request pair
# (TIM1_CH2 -> channel 2, TIM1_CH4 -> channel 3) map the same way on every H5
# part, so there is no per-part code in ow_port_h5.h - only memory sizes, the
# CMSIS device layer and the clock default differ.
#
# 64MHz, HSI direct. Not the part's ceiling (250MHz): HSI is 64MHz straight,
# no PLL, no crystal, no voltage-scaling change - the bring-up clock. The
# other two clocks app.c implements for this family are the raw 8MHz HSE
# (Y2 on this board) and the HSE+PLL1 250MHz step (VOS0 + 5 flash wait
# states); both are bench-validated.
#
# APB prescalers stay /1 at all three clocks, so TIM1 = PCLK2 = SYSCLK
# directly, which is what keeps the ow_port 1us-tick invariant and
# OW_PORT_TIM_PRESCALER at SYSCLK_MHZ - 1.
#
# 128KB flash / 32KB SRAM in one contiguous block, so the linker script needs
# nothing more.
#
# No SVD upstream (cmsis-svd-data carries no STM32H503 file), so CHIP_SVD is
# empty; ST-Link flashing and debugging do not need it. No J-Flash project
# either (the H503 JTAG CoreID is unverified); the Ozone project uses the
# CMSIS-SVD bundled with the install.

# CMSIS device macro; selects stm32h503xx.h via stm32h5xx.h
CHIP_DEV_DEF = -DSTM32H503xx
CHIP_DEVICE_HDR = $(CMSIS_DEVICE_DIR)/stm32h503xx.h
CHIP_STARTUP = $(CMSIS_DEVICE_DIR)/startup_stm32h503xx.s
# 128KB flash / 32KB SRAM
CHIP_LINKER = port/stm32h5/STM32H503CB_FLASH.ld
CHIP_JFLASH =
CHIP_JDEBUG = port/stm32h5/project-h503cb.jdebug
CHIP_SVD =
# Board crystal (Y2): the PLL input for the 8MHz raw-HSE and 250MHz
# HSE+PLL1 clocks in app.c.
CHIP_HSE_MHZ = 8
# Default build clock. Override per build with SYSCLK_MHZ=/HSE_MHZ= (e.g.
# `make OW_TARGET=h5 APP=2_device_search SYSCLK_MHZ=250 HSE_MHZ=8`).
CHIP_SYSCLK_MHZ = 64
