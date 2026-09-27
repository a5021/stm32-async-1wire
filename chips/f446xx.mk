# chips/f446xx.mk — STM32F446xx (WeAct Studio F446RE / F446RET6)
#
# Part identity ONLY. See chips/f103xb.mk for what does and does not belong
# here, and for the "no trailing comment on an assignment line" rule.
#
# The F4 port backend is shared by the whole F4 family: TIM1/DMA2 and CHSEL=6
# map identically here as on the F405/F407 (RM0390 DMA2 request mapping keeps
# TIM1_CH2 on Stream 2 and TIM1_CH4 on Stream 4, both at CHSEL=6), so there is
# no per-part code in ow_port_f4.h — only memory sizes, the CMSIS device layer
# and the clock default differ.
#
# 180MHz, not 168: the F446 is the only F4 part in the matrix that needs the
# over-drive sequence, and app.c gives it a branch of its own. The clock layout
# is the one ST's own RCC_ClockConfig example uses for this part (8MHz HSE,
# M=8 N=360 P=2, APB1 /4, APB2 /2, 5 flash wait states). The APB2 prescaler of
# /2 still doubles to 180MHz on TIM1, so the ow_port 1us-tick invariant is
# unchanged and OW_PORT_TIM_PRESCALER stays SYSCLK_MHZ - 1.
#
# 512KB flash / 128KB SRAM, and no CCM: the F446 has 128KB of DMA-accessible
# SRAM1/SRAM2 and, unlike the F405/F407, no CCM at all (checked against the
# upstream CMSIS header and SVD). The 1-Wire backend's DMA writes stay in SRAM.
#
# Validated on hardware (WeAct F446RET6, 7x DS18B20 in parasite power on one
# bus, 180MHz and every other supported SYSCLK). Flash/SRAM sizes confirmed via
# st-info; the 180MHz PLL + over-drive sequence boots and ODEN/ODSWEN are both
# set in PWR_CR. All five supported clocks (180/168/84/16/raw-HSE) enumerate all
# seven sensors with valid CRC8 and drive the 115200 console, which is what
# proves the APB prescalers: the console divisor differs per clock, so clean
# text at all five pins PCLK2 down. The 1us-tick invariant holds across the
# range - TIM1 always lands on SYSCLK/2 * 2 with a PSC of SYSCLK_MHZ-1, so
# 1-Wire slot widths are clock-independent in real time (confirmed with a
# logic analyzer: identical pulse distributions at 180 and 16MHz).

# CMSIS device macro; selects stm32f446xx.h via stm32f4xx.h
CHIP_DEV_DEF = -DSTM32F446xx
CHIP_DEVICE_HDR = $(CMSIS_DEVICE_DIR)/stm32f446xx.h
CHIP_STARTUP = $(CMSIS_DEVICE_DIR)/startup_stm32f446xx.s
# 512KB flash / 128KB SRAM
CHIP_LINKER = port/stm32f4/STM32F446RE_FLASH.ld
CHIP_JFLASH = port/stm32f4/stm32f446re.jflash
CHIP_JDEBUG = port/stm32f4/project-f446re.jdebug
CHIP_SVD = $(CMSIS_DEVICE_DIR)/STM32F446.svd
# Board crystal, not part identity. 8MHz is what the WeAct F446RE carries; any
# other board must pass HSE_MHZ=<n> to the build, and the M divider follows it.
CHIP_HSE_MHZ = 8
CHIP_SYSCLK_MHZ = 180
