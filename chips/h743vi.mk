# chips/h743vi.mk — STM32H743VI on the WeAct Studio STM32H7XX Core Board
#
# Part identity ONLY. See chips/f103xb.mk for what does and does not belong
# here, and for the "no trailing comment on an assignment line" rule.
#
# Naming follows the CMSIS device macro (stm32h7xx.h lists the valid device
# macros per family), lowercased: STM32H743xx -> h743vi.
#
# The H7 port backend is shared by the whole H7 family as it stands: TIM1 CH3 on
# PA10, the CH4 indirect capture off TI3, and the DMAMUX1 request pair
# (TIM1_CH2 = 12 on DMAMUX channel 2, TIM1_CH4 = 14 on DMAMUX channel 3) map the
# same way on every H7 part, so there is no per-part code in the ow_mcu.h H7
# section - only memory sizes, the CMSIS device layer and the clock default
# differ.
#
# 240MHz, HSE+PLL. Not the part's silicon ceiling (480MHz): TIM1 must equal
# SYSCLK for the 1us tick, and the APB2 timer clock is 2 x PCLK2 with a /2
# prescaler, so 2 x 120MHz is all the timer invariant allows. The voltage
# range and flash wait states for 240MHz (per DS12110) are set by app.c.
#
# 2MB flash / 1MB RAM; the linker script puts everything in the 512KB AXI
# SRAM1 (0x24000000). DTCM/ITCM are deliberately unused: the CPU-only tightly
# coupled memories are not reachable by DMA, so a pulse or capture buffer
# placed there would silently never transfer.
#
# No SVD at the pinned cmsis-svd-data revision and no J-Flash project (the
# bench flashes via ST-Link); ST-Link flashing and debugging do not need
# either. Both blanks are sanctioned: check_chips.sh lists CHIP_JFLASH in
# OPTIONAL_PATHS and calls CHIP_SVD informational, and the CMake part guard
# excludes the two from its empty-key check.

# CMSIS device macro; selects stm32h743xx.h via stm32h7xx.h
CHIP_DEV_DEF = -DSTM32H743xx
CHIP_DEVICE_HDR = $(CMSIS_DEVICE_DIR)/stm32h743xx.h
CHIP_STARTUP = $(CMSIS_DEVICE_DIR)/startup_stm32h743xx.s
# 2MB flash / 512KB AXI SRAM1
CHIP_LINKER = port/stm32h7/STM32H743VI_FLASH.ld
CHIP_JFLASH =
CHIP_JDEBUG = port/stm32h7/project-h743vi.jdebug
CHIP_SVD =
# Board crystal: the PLL input for the 240MHz HSE+PLL clock in app.c.
CHIP_HSE_MHZ = 25
# Default build clock. Override per build with SYSCLK_MHZ=/HSE_MHZ=.
CHIP_SYSCLK_MHZ = 240
