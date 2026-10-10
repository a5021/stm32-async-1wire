# Select the example application:
#   1_basic             (single sensor, Skip ROM)
#   2_device_search     (device search + per-device polling)
#   3_round_robin       (device search + sequential polling of every sensor)
#   4_scan_mode         (device search + simultaneous broadcast conversion)
#   5_commands          (device search + command transactions: ROM, power supply,
#                        TH/TL, Copy/Recall EEPROM)
#   6_statistics        (device search + sequential polling with signal statistics)
#   7_low_power         (device search + WFE sleep on long stages)
#   make                     -> builds 1_basic   (ds18b20_1_basic.elf)
#   make APP=2_device_search -> builds 2_device_search
#   make APP=3_round_robin   -> builds 3_round_robin
#   make APP=4_scan_mode     -> builds 4_scan_mode
#   make APP=5_commands      -> builds 5_commands
#   make APP=6_statistics    -> builds 6_statistics
#   make APP=7_low_power     -> builds 7_low_power
APP ?= 1_basic
ifeq ($(filter $(APP),1_basic 2_device_search 3_round_robin 4_scan_mode 5_commands 6_statistics 7_low_power),)
$(error APP must be '1_basic', '2_device_search', '3_round_robin', '4_scan_mode', '5_commands', '6_statistics' or '7_low_power')
endif

# 6_statistics is the signal-statistics example: enable the optional stats module by
# default and widen the stats window to 5000 measurement rounds.  Parasite power is
# deliberately NOT set here (it is bus-hardware dependent) — pass EXT="-DOW_PARASITE_POWER=1"
# when the 1-Wire bus is parasite-powered.
ifeq ($(APP),6_statistics)
# Only OW_STATS_ENABLE is forced: the dump period (STATS_DUMP_SWEEPS) is defined
# once, in the example's own source, so it is reachable and overridable. Forcing
# it here with `override ... +=` also defeated any value passed as EXT=..., since
# the appended define lands last on the command line and the last one wins.
override EXT += -DOW_STATS_ENABLE=1
endif

# Define the name of the project target and the build directory
TARGET = ds18b20_$(APP)
BUILD_DIR = build

# CMSIS directory structure for third-party build dependencies
CMSIS_CORE_DIR   = CMSIS/core
CMSIS_DEVICE_DIR = CMSIS/device

# Define the C source files, assembly source file, linker script, and preprocessor definitions
# OW_TARGET selects the MCU family: f1 (STM32F103xB, default), f0 (STM32F030x6),
# f3 (STM32F303xC), g0 (STM32G031xx), f4 (STM32F407xx / STM32F401 family) or
# g4 (STM32G474CB, e.g. the WeAct G474 Long).
#   make                -> F1 firmware
#   make OW_TARGET=f0   -> F0 firmware
#   make OW_TARGET=f3   -> F3 firmware (STM32F303VC, e.g. the F3-DISCOVERY)
#   make OW_TARGET=g0   -> G0 firmware
#   make OW_TARGET=f4   -> F4 firmware (default part f407xx)
#   make OW_TARGET=g4   -> G4 firmware (default part g474cb)
#   make OW_TARGET=h5   -> H5 firmware (default part h503cb)
#
# OW_CHIP selects the part *within* the family; its value is the name of a
# chips/<part>.mk file, which carries the part identity (CMSIS device macro,
# device header, startup file, linker script, debugger projects, SVD and the
# default system clock). Everything shared by a whole family stays here.
#   make OW_TARGET=f4 OW_CHIP=f401xc
#
# An unknown OW_TARGET or OW_CHIP is a hard error. It used to fall through to
# the F1 branch and build the wrong part with a valid-looking binary, which is
# worse than not building at all. Note OW_TARGET never had an explicit default:
# the empty value silently meant "F1", which is exactly how a typo such as
# OW_TARGET=f401-84 ended up compiling a Blue Pill.
OW_TARGET ?= f1
OW_KNOWN_TARGETS = f1 f0 f3 g0 f4 g4 h5
ifeq ($(filter $(OW_TARGET),$(OW_KNOWN_TARGETS)),)
$(error OW_TARGET='$(OW_TARGET)' is not a known family. Use one of: $(OW_KNOWN_TARGETS))
endif
ifeq ($(OW_TARGET),f0)
SRC = $(CMSIS_DEVICE_DIR)/system_stm32f0xx.c examples/$(APP)/main.c src/onewire.c src/ds18b20.c examples/app/app.c src/ow_stats.c src/syscall.c
MCU = -mcpu=cortex-m0 -mthumb
PORT_DEF = OW_PORT_TARGET_F0
else ifeq ($(OW_TARGET),g0)
SRC = $(CMSIS_DEVICE_DIR)/system_stm32g0xx.c examples/$(APP)/main.c src/onewire.c src/ds18b20.c examples/app/app.c src/ow_stats.c src/syscall.c
MCU = -mcpu=cortex-m0plus -mthumb
PORT_DEF = OW_PORT_TARGET_G0
else ifeq ($(OW_TARGET),f4)
SRC = $(CMSIS_DEVICE_DIR)/system_stm32f4xx.c examples/$(APP)/main.c src/onewire.c src/ds18b20.c examples/app/app.c src/ow_stats.c src/syscall.c
MCU = -mcpu=cortex-m4 -mthumb
PORT_DEF = OW_PORT_TARGET_F4
else ifeq ($(OW_TARGET),f3)
SRC = $(CMSIS_DEVICE_DIR)/system_stm32f3xx.c examples/$(APP)/main.c src/onewire.c src/ds18b20.c examples/app/app.c src/ow_stats.c src/syscall.c
MCU = -mcpu=cortex-m4 -mthumb
PORT_DEF = OW_PORT_TARGET_F3
else ifeq ($(OW_TARGET),g4)
SRC = $(CMSIS_DEVICE_DIR)/system_stm32g4xx.c examples/$(APP)/main.c src/onewire.c src/ds18b20.c examples/app/app.c src/ow_stats.c src/syscall.c
MCU = -mcpu=cortex-m4 -mthumb
PORT_DEF = OW_PORT_TARGET_G4
else ifeq ($(OW_TARGET),h5)
SRC = $(CMSIS_DEVICE_DIR)/system_stm32h5xx.c examples/$(APP)/main.c src/onewire.c src/ds18b20.c examples/app/app.c src/ow_stats.c src/syscall.c
MCU = -mcpu=cortex-m33 -mthumb
PORT_DEF = OW_PORT_TARGET_H5
else
SRC = $(CMSIS_DEVICE_DIR)/system_stm32f1xx.c examples/$(APP)/main.c src/onewire.c src/ds18b20.c examples/app/app.c src/ow_stats.c src/syscall.c
MCU = -mcpu=cortex-m3 -mthumb
PORT_DEF = OW_PORT_TARGET_F1
endif

# Part selection: pull the part identity in from chips/<part>.mk. The default
# part per family is the one its OW_TARGET name refers to.
ifeq ($(OW_TARGET),f0)
OW_CHIP ?= f030x6
else ifeq ($(OW_TARGET),g0)
OW_CHIP ?= g031xx
else ifeq ($(OW_TARGET),f4)
OW_CHIP ?= f407xx
else ifeq ($(OW_TARGET),f3)
OW_CHIP ?= f303xc
else ifeq ($(OW_TARGET),g4)
OW_CHIP ?= g474cb
else ifeq ($(OW_TARGET),h5)
OW_CHIP ?= h503cb
else
OW_CHIP ?= f103xb
endif
CHIP_MK = chips/$(OW_CHIP).mk
ifeq ($(wildcard $(CHIP_MK)),)
$(error OW_CHIP='$(OW_CHIP)' has no $(CHIP_MK). Known parts: $(patsubst chips/%.mk,%,$(wildcard chips/*.mk)))
endif
include $(CHIP_MK)

# A comment at the end of an assignment line leaves the whitespace in front of
# '#' inside the value, which silently turns every path below into a name that
# does not exist. Strip once here so a part file written that way still works.
CHIP_DEV_DEF := $(strip $(CHIP_DEV_DEF))
CHIP_DEVICE_HDR := $(strip $(CHIP_DEVICE_HDR))
CHIP_STARTUP := $(strip $(CHIP_STARTUP))
CHIP_LINKER := $(strip $(CHIP_LINKER))
CHIP_JFLASH := $(strip $(CHIP_JFLASH))
CHIP_JDEBUG := $(strip $(CHIP_JDEBUG))
CHIP_SVD := $(strip $(CHIP_SVD))
CHIP_SYSCLK_MHZ := $(strip $(CHIP_SYSCLK_MHZ))

# Crystal (HSE) frequency of the *board*, F4, G4 and H5 only. Not part identity: a
# part file may carry a default for the board it is named after, and HSE_MHZ=
# on the command line wins for any other board.
CHIP_HSE_MHZ := $(strip $(CHIP_HSE_MHZ))

ASM = $(CHIP_STARTUP)
LDS = $(CHIP_LINKER)
JFLASH = $(CHIP_JFLASH)
JDEBUG = $(CHIP_JDEBUG)
DEF = $(CHIP_DEV_DEF) -D$(PORT_DEF)
# The part's own clock default; SYSCLK_MHZ still wins when passed explicitly
# (e.g. 16 for a board without an HSE crystal).
ifndef SYSCLK_MHZ
DEF += -DOW_PORT_SYSCLK_MHZ=$(CHIP_SYSCLK_MHZ)
endif
ifneq ($(CHIP_HSE_MHZ),)
ifndef HSE_MHZ
DEF += -DOW_HSE_MHZ=$(CHIP_HSE_MHZ)
endif
endif
INC = -I. -Iinc -Iexamples/app -Iport/stm32f1 -Iport/stm32f0 -Iport/stm32f3 -Iport/stm32g0 -Iport/stm32f4 -Iport/stm32g4 -Iport/stm32h5 -Iport/common -I$(CMSIS_CORE_DIR) -I$(CMSIS_DEVICE_DIR)

# Per-app USART1 TX ring buffer size (power of two), overrides the app.h default
UART_TX_SIZE_1_basic        = 128
UART_TX_SIZE_2_device_search = 256
UART_TX_SIZE_3_round_robin  = 256
UART_TX_SIZE_4_scan_mode    = 256
UART_TX_SIZE_5_commands     = 256
UART_TX_SIZE_6_statistics    = 1024
UART_TX_SIZE_7_low_power    = 256
DEF += -DUART_TX_BUF_SIZE=$(UART_TX_SIZE_$(APP))

# Optional system clock override:
# make SYSCLK_MHZ=16  →  -DOW_PORT_SYSCLK_MHZ=16
# (run on the raw internal RC instead of the family default:
#  STM32F103 = 72MHz HSE+PLL x9, STM32F030 = 48MHz HSI/2+PLL x12,
#  STM32G031 = 64MHz HSI16+PLL, STM32F303 = 72MHz HSE+PLL (or 64 HSI/2+PLL,
#  8 raw HSI), STM32F407/F401/F446 = 168/84/180MHz HSE+PLL (or 16 raw HSI),
#  STM32G474 = 170MHz HSE+PLL (or 16 raw HSI16);
#  e.g. SYSCLK_MHZ=16 for the raw 16MHz RC on families that have one)
ifdef SYSCLK_MHZ
DEF += -DOW_PORT_SYSCLK_MHZ=$(SYSCLK_MHZ)
endif

# Crystal frequency override for the F4 and G4 backends:
# make HSE_MHZ=25  ->  -DOW_HSE_MHZ=25
# Separate from SYSCLK_MHZ because the crystal belongs to the board and the
# system clock to the application; the F4 PLL derives its M divider from it, so a
# wrong value leaves the PLL unlocked - which the app now reports instead of
# spinning forever. The G4 PLL field is OW_HSE_MHZ/4 - 1 (divider-minus-1,
# so /2 on the 8MHz WeAct board) the same way.
ifdef HSE_MHZ
DEF += -DOW_HSE_MHZ=$(HSE_MHZ)
endif

# Optional experimental active-drive write path:
# make OW_DRIVE_ACTIVE=1  →  -DOW_DRIVE_ACTIVE=1
# Pure-write transactions switch PA10 to push-pull so the master actively
# drives BOTH bus levels (faster, stronger write-1); read/reset phases stay
# open-drain. Experimental; kept off by default. See bus electrical-model doc.
ifeq ($(OW_DRIVE_ACTIVE),1)
DEF += -DOW_DRIVE_ACTIVE=1
endif

# Optional compile-time timing preset: one_pulse zero_pulse guard_band short_pulse_max
_OW_TIMING_FAST     := 5 60  3 10
_OW_TIMING_STANDARD := 5 60  5 10
_OW_TIMING_SLOW     := 8 90 20 15
_OW_TIMING_ROBUST   := 10 110 30 18
_OW_TIMING_CUSTOM   := 1 60  1 15

# make TIMING=SLOW|FAST|STANDARD|ROBUST|CUSTOM  →  sets the pulse/guard
# durations for that profile as -DONEWIRE_* defines. Override any single
# value with EXT="-DONEWIRE_GUARD_BAND=..." etc.
ifdef TIMING
  ifeq ($(filter $(TIMING),FAST STANDARD SLOW ROBUST CUSTOM),)
    $(error TIMING must be FAST, STANDARD, SLOW, ROBUST or CUSTOM)
  endif
  DEF += -DONEWIRE_ONE_PULSE=$(word 1, $(_OW_TIMING_$(TIMING)))
  DEF += -DONEWIRE_ZERO_PULSE=$(word 2, $(_OW_TIMING_$(TIMING)))
  DEF += -DONEWIRE_GUARD_BAND=$(word 3, $(_OW_TIMING_$(TIMING)))
  DEF += -DONEWIRE_SHORT_PULSE_MAX=$(word 4, $(_OW_TIMING_$(TIMING)))
endif

# Optimization flags for the compiler. The profiles live in config/optim.mk,
# which the CMake build reads too, so "make and cmake produce the same
# firmware" is enforced by there being only one copy of the flags.
include config/optim.mk

OPT = $(OPT_RELEASE)

ifeq ($(filter $(OW_TARGET),$(OPT_NO_LTO)),)
else
OPT := $(filter-out -flto,$(OPT))
endif

# Define the toolchain prefix
TOOLCHAIN := $(if $(GCC_PATH),$(GCC_PATH)/,)arm-none-eabi-

# Wrapper to quote paths with spaces
Q = $(if $(findstring $(space),$(1)),"$(1)",$(1))

CC = $(call Q,$(TOOLCHAIN)gcc)
LD = $(call Q,$(TOOLCHAIN)ld)
AS = $(call Q,$(TOOLCHAIN)gcc) -x assembler-with-cpp
CP = $(call Q,$(TOOLCHAIN)objcopy)
SZ = $(call Q,$(TOOLCHAIN)size)

# Define space for the Q function
space := $(subst ,, )

# Define utility programs used for programming the device
HEX = $(CP) -O ihex
BIN = $(CP) -O binary -S

# Set additional compiler flags for dependencies and object file generation
FLAG = $(MCU) $(DEF) $(INC) -Wall -Werror -Wextra -Wpedantic -Wswitch-enum -fdata-sections -ffunction-sections

JLINK_FLAGS = -openprj$(JFLASH) -open$(BUILD_DIR)/$(TARGET).hex -hide -auto -exit -jflashlog./jflash.log

ifeq ($(OS), Windows_NT)

    STLINK = ST-LINK_CLI.exe
    STLINK_FLAGS = -c UR -V -P $(BUILD_DIR)/$(TARGET).hex -HardRst -Run

    JLINK = JFlash.Exe

else

    UNAME_S := $(shell uname -s)
    ifeq ($(UNAME_S), Linux)
        FLAG += -D LINUX
    endif
    ifeq ($(UNAME_S), Darwin)
        FLAG += -D OSX
    endif
    ifneq ($(filter arm%, $(UNAME_P)),)
        FLAG += -D ARM
    endif

    STLINK = st-flash
    STLINK_FLAGS = --reset --format ihex write $(BUILD_DIR)/$(TARGET).hex

    JLINK = JFlashExe

endif

# Set additional compiler flags for dependencies and object file generation
# -MP is dropped: its phony per-header targets defeat the dependency on exactly
# the headers that need to trigger a rebuild. The explicit prerequisite lists in
# the object rule below carry the real dependency set (see there for why the
# depfiles cannot be trusted as generated on Windows).
FLAG += -MMD -MF $(@:%.o=%.d)

# Define linker flags
LIB = -lc -lm -lnosys
LDFLAGS = $(MCU) -specs=nano.specs -T$(LDS) $(LIB) -Wl,-Map=$(BUILD_DIR)/$(TARGET).map,--cref -Wl,--gc-sections

# ---------- compiler / linker version detection ----------
GCC_INFO    := $(shell $(CC) -dumpfullversion 2>/dev/null | awk -F. '{print $$0, ($$1*10000+$$2*100+$$3>=120000)}')
GCC_VERSION := $(word 1,$(GCC_INFO))
GCC_GE_12   := $(word 2,$(GCC_INFO))

LD_INFO     := $(shell $(LD) --version 2>/dev/null | awk '/^GNU ld/ {match($$NF,/([0-9]+)\.([0-9]+)/,v); print v[0], (v[1]*100+v[2]>=239); exit}')
LD_VERSION  := $(word 1,$(LD_INFO))
LD_GE_2_39  := $(word 2,$(LD_INFO))

$(info using GCC $(GCC_VERSION), Binutils $(LD_VERSION))

# ---------- suppress RWX segment warnings ----------
ifneq ($(or $(filter 1,$(GCC_GE_12)),$(filter 1,$(LD_GE_2_39))),)
  LDFLAGS += -Wl,--no-warn-rwx-segments
endif

# =============================================================================
# DEPENDENCY DOWNLOADING SECTION
# =============================================================================

# Tools detection - prefer wget, fall back to curl
WGET := $(shell command -v wget 2> /dev/null)
CURL := $(shell command -v curl 2> /dev/null)
DOWNLOAD_TOOL  = $(or $(WGET),$(CURL))
DOWNLOAD_FLAGS = $(if $(WGET),-q -O,-s -o)

# Base URLs
RAW_URL = https://raw.githubusercontent.com
ST_URL = $(RAW_URL)/STMicroelectronics/
CMSIS_CORE_URL = $(RAW_URL)/ARM-software/CMSIS_5/master/CMSIS/Core/Include
F1_URL = $(ST_URL)cmsis_device_f1/master
F0_URL = $(ST_URL)cmsis_device_f0/master
G0_URL = $(ST_URL)cmsis_device_g0/master
F4_URL = $(ST_URL)cmsis_device_f4/master
G4_URL = $(ST_URL)cmsis_device_g4/master
F3_URL = $(ST_URL)cmsis-device-f3/master
H5_URL = $(ST_URL)cmsis_device_h5/master
SVD_URL_F1 = https://raw.githubusercontent.com/cmsis-svd/cmsis-svd-data/refs/heads/main/data/STMicro/STM32F103xx.svd
SVD_URL_F0 = https://raw.githubusercontent.com/cmsis-svd/cmsis-svd-data/refs/heads/main/data/STMicro/STM32F030.svd
SVD_URL_G0 = https://raw.githubusercontent.com/cmsis-svd/cmsis-svd-data/refs/heads/main/data/STMicro/STM32G031.svd
SVD_URL_F4 = https://raw.githubusercontent.com/cmsis-svd/cmsis-svd-data/refs/heads/main/data/STMicro/STM32F407.svd
SVD_URL_G4 = https://raw.githubusercontent.com/cmsis-svd/cmsis-svd-data/refs/heads/main/data/STMicro/STM32G474xx.svd
SVD_URL_F401 = https://raw.githubusercontent.com/cmsis-svd/cmsis-svd-data/refs/heads/main/data/STMicro/STM32F401.svd
SVD_URL_F446 = https://raw.githubusercontent.com/cmsis-svd/cmsis-svd-data/refs/heads/main/data/STMicro/STM32F446.svd
SVD_URL_F303 = https://raw.githubusercontent.com/cmsis-svd/cmsis-svd-data/refs/heads/main/data/STMicro/STM32F303.svd

# Required external files (needed for build but not in repo)
# The part-specific entries come from chips/<part>.mk, so download-deps fetches
# exactly what the selected part needs instead of every part of the family.
ifeq ($(OW_TARGET),f0)
CMSIS_CORE_HEADERS = $(CMSIS_CORE_DIR)/core_cm0.h
CMSIS_DEVICE_FAMILY_HDR = stm32f0xx.h
CMSIS_SYSTEM_HDR = system_stm32f0xx.h
CMSIS_SYSTEM_SRC = system_stm32f0xx.c
else ifeq ($(OW_TARGET),g0)
CMSIS_CORE_HEADERS = $(CMSIS_CORE_DIR)/core_cm0plus.h \
                     $(CMSIS_CORE_DIR)/mpu_armv7.h
CMSIS_DEVICE_FAMILY_HDR = stm32g0xx.h
CMSIS_SYSTEM_HDR = system_stm32g0xx.h
CMSIS_SYSTEM_SRC = system_stm32g0xx.c
else ifeq ($(OW_TARGET),f4)
# core_cm4.h includes mpu_armv7.h unconditionally (every Cortex-M4 has an MPU),
# so the F4 build fails on a cold cache without it. core_cm3.h/core_cm0.h reach
# it only behind __MPU_PRESENT, which the F1 and F0 parts do not define.
CMSIS_CORE_HEADERS = $(CMSIS_CORE_DIR)/core_cm4.h \
                     $(CMSIS_CORE_DIR)/mpu_armv7.h
CMSIS_DEVICE_FAMILY_HDR = stm32f4xx.h
CMSIS_SYSTEM_HDR = system_stm32f4xx.h
CMSIS_SYSTEM_SRC = system_stm32f4xx.c
else ifeq ($(OW_TARGET),f3)
# Same as F4: a Cortex-M4, so core_cm4.h pulls in mpu_armv7.h unconditionally.
CMSIS_CORE_HEADERS = $(CMSIS_CORE_DIR)/core_cm4.h \
                     $(CMSIS_CORE_DIR)/mpu_armv7.h
CMSIS_DEVICE_FAMILY_HDR = stm32f3xx.h
CMSIS_SYSTEM_HDR = system_stm32f3xx.h
CMSIS_SYSTEM_SRC = system_stm32f3xx.c
else ifeq ($(OW_TARGET),g4)
# Same as F4/F3: a Cortex-M4, so core_cm4.h pulls in mpu_armv7.h
# unconditionally.
CMSIS_CORE_HEADERS = $(CMSIS_CORE_DIR)/core_cm4.h \
                     $(CMSIS_CORE_DIR)/mpu_armv7.h
CMSIS_DEVICE_FAMILY_HDR = stm32g4xx.h
CMSIS_SYSTEM_HDR = system_stm32g4xx.h
CMSIS_SYSTEM_SRC = system_stm32g4xx.c
else ifeq ($(OW_TARGET),h5)
# Cortex-M33: core_cm33.h pulls in mpu_armv8.h unconditionally.
CMSIS_CORE_HEADERS = $(CMSIS_CORE_DIR)/core_cm33.h \
                     $(CMSIS_CORE_DIR)/mpu_armv8.h
CMSIS_DEVICE_FAMILY_HDR = stm32h5xx.h
CMSIS_SYSTEM_HDR = system_stm32h5xx.h
CMSIS_SYSTEM_SRC = system_stm32h5xx.c
else
CMSIS_CORE_HEADERS = $(CMSIS_CORE_DIR)/core_cm3.h
CMSIS_DEVICE_FAMILY_HDR = stm32f1xx.h
CMSIS_SYSTEM_HDR = system_stm32f1xx.h
CMSIS_SYSTEM_SRC = system_stm32f1xx.c
endif
EXTERNAL_DEPS = $(CMSIS_CORE_HEADERS) \
                $(CMSIS_CORE_DIR)/cmsis_compiler.h \
                $(CMSIS_CORE_DIR)/cmsis_gcc.h \
                $(CMSIS_CORE_DIR)/cmsis_version.h \
                $(CMSIS_DEVICE_DIR)/$(CMSIS_DEVICE_FAMILY_HDR) \
                $(CHIP_DEVICE_HDR) \
                $(CMSIS_DEVICE_DIR)/$(CMSIS_SYSTEM_HDR) \
                $(CMSIS_DEVICE_DIR)/$(CMSIS_SYSTEM_SRC) \
                $(CHIP_STARTUP) \
                $(CHIP_SVD)


# License files
CMSIS_CORE_LICENSE_URL = https://raw.githubusercontent.com/ARM-software/CMSIS_5/master/LICENSE.txt
DEVICE_F1_LICENSE_URL = https://raw.githubusercontent.com/STMicroelectronics/cmsis_device_f1/master/License.md

CMSIS_CORE_LICENSE = $(CMSIS_CORE_DIR)/LICENSE.txt
CMSIS_DEVICE_LICENSE = $(CMSIS_DEVICE_DIR)/LICENSE

LICENSE_FILES = $(CMSIS_CORE_LICENSE) $(CMSIS_DEVICE_LICENSE)

# Download function using wget or curl. Retries on transient network
# failures (the CMSIS/CDN hosts occasionally drop a connection) so CI does
# not fail a whole job because of one flaky fetch.
define download_file
	@echo "  Downloading $(1)..."
	@if [ -z "$(DOWNLOAD_TOOL)" ]; then \
		echo "Error: neither wget nor curl found. Please install one of them."; \
		exit 1; \
	fi
	@n=1; ok=0; \
	while [ $$n -le 5 ]; do \
		if $(DOWNLOAD_TOOL) $(DOWNLOAD_FLAGS) "$(2)" "$(1)"; then \
			ok=1; break; \
		fi; \
		echo "    attempt $$n failed, retrying..."; \
		n=$$((n+1)); \
		sleep 2; \
	done; \
	if [ $$ok -eq 1 ]; then echo "    OK"; else echo "    FAILED"; exit 1; fi
endef

# Create CMSIS directories
$(CMSIS_CORE_DIR):
	mkdir -p $@

$(CMSIS_DEVICE_DIR):
	mkdir -p $@

# Downloaded files, one entry per line: target | directory | source URL.
# The target and its URL share a line on purpose. Adding a CMSIS device URL
# variable one at a time made it possible to define a URL and miss the rule
# that used it, and the result was every CI job failing on an empty URL.
# Keep them together; the rules below are generated from this table.
# ARM CMSIS Core headers (Apache 2.0)
CMSIS_DOWNLOADS_CORE = \
  core_cm3.h|$(CMSIS_CORE_DIR)|$(CMSIS_CORE_URL)/core_cm3.h \
  core_cm0.h|$(CMSIS_CORE_DIR)|$(CMSIS_CORE_URL)/core_cm0.h \
  core_cm0plus.h|$(CMSIS_CORE_DIR)|$(CMSIS_CORE_URL)/core_cm0plus.h \
  mpu_armv7.h|$(CMSIS_CORE_DIR)|$(CMSIS_CORE_URL)/mpu_armv7.h \
  cmsis_compiler.h|$(CMSIS_CORE_DIR)|$(CMSIS_CORE_URL)/cmsis_compiler.h \
  cmsis_gcc.h|$(CMSIS_CORE_DIR)|$(CMSIS_CORE_URL)/cmsis_gcc.h \
  cmsis_version.h|$(CMSIS_CORE_DIR)|$(CMSIS_CORE_URL)/cmsis_version.h \
  core_cm4.h|$(CMSIS_CORE_DIR)|$(CMSIS_CORE_URL)/core_cm4.h \
  core_cm33.h|$(CMSIS_CORE_DIR)|$(CMSIS_CORE_URL)/core_cm33.h \
  mpu_armv8.h|$(CMSIS_CORE_DIR)|$(CMSIS_CORE_URL)/mpu_armv8.h
# cmsis_device_f1 headers and sources (Apache 2.0)
CMSIS_DOWNLOADS_F1 = \
  stm32f1xx.h|$(CMSIS_DEVICE_DIR)|$(F1_URL)/Include/stm32f1xx.h \
  stm32f103xb.h|$(CMSIS_DEVICE_DIR)|$(F1_URL)/Include/stm32f103xb.h \
  system_stm32f1xx.h|$(CMSIS_DEVICE_DIR)|$(F1_URL)/Include/system_stm32f1xx.h \
  system_stm32f1xx.c|$(CMSIS_DEVICE_DIR)|$(F1_URL)/Source/Templates/system_stm32f1xx.c \
  startup_stm32f103xb.s|$(CMSIS_DEVICE_DIR)|$(F1_URL)/Source/Templates/gcc/startup_stm32f103xb.s
# cmsis_device_f0 headers and sources (Apache 2.0)
CMSIS_DOWNLOADS_F0 = \
  stm32f0xx.h|$(CMSIS_DEVICE_DIR)|$(F0_URL)/Include/stm32f0xx.h \
  stm32f030x6.h|$(CMSIS_DEVICE_DIR)|$(F0_URL)/Include/stm32f030x6.h \
  system_stm32f0xx.h|$(CMSIS_DEVICE_DIR)|$(F0_URL)/Include/system_stm32f0xx.h \
  system_stm32f0xx.c|$(CMSIS_DEVICE_DIR)|$(F0_URL)/Source/Templates/system_stm32f0xx.c \
  startup_stm32f030x6.s|$(CMSIS_DEVICE_DIR)|$(F0_URL)/Source/Templates/gcc/startup_stm32f030x6.s
# cmsis_device_g0 headers and sources (Apache 2.0)
CMSIS_DOWNLOADS_G0 = \
  stm32g0xx.h|$(CMSIS_DEVICE_DIR)|$(G0_URL)/Include/stm32g0xx.h \
  stm32g031xx.h|$(CMSIS_DEVICE_DIR)|$(G0_URL)/Include/stm32g031xx.h \
  system_stm32g0xx.h|$(CMSIS_DEVICE_DIR)|$(G0_URL)/Include/system_stm32g0xx.h \
  system_stm32g0xx.c|$(CMSIS_DEVICE_DIR)|$(G0_URL)/Source/Templates/system_stm32g0xx.c \
  startup_stm32g031xx.s|$(CMSIS_DEVICE_DIR)|$(G0_URL)/Source/Templates/gcc/startup_stm32g031xx.s
# cmsis_device_f4 headers and sources (Apache 2.0)
CMSIS_DOWNLOADS_F4 = \
  stm32f4xx.h|$(CMSIS_DEVICE_DIR)|$(F4_URL)/Include/stm32f4xx.h \
  stm32f407xx.h|$(CMSIS_DEVICE_DIR)|$(F4_URL)/Include/stm32f407xx.h \
  stm32f401xc.h|$(CMSIS_DEVICE_DIR)|$(F4_URL)/Include/stm32f401xc.h \
  stm32f401xe.h|$(CMSIS_DEVICE_DIR)|$(F4_URL)/Include/stm32f401xe.h \
  stm32f446xx.h|$(CMSIS_DEVICE_DIR)|$(F4_URL)/Include/stm32f446xx.h \
  system_stm32f4xx.h|$(CMSIS_DEVICE_DIR)|$(F4_URL)/Include/system_stm32f4xx.h \
  system_stm32f4xx.c|$(CMSIS_DEVICE_DIR)|$(F4_URL)/Source/Templates/system_stm32f4xx.c \
  startup_stm32f407xx.s|$(CMSIS_DEVICE_DIR)|$(F4_URL)/Source/Templates/gcc/startup_stm32f407xx.s \
  startup_stm32f401xc.s|$(CMSIS_DEVICE_DIR)|$(F4_URL)/Source/Templates/gcc/startup_stm32f401xc.s \
  startup_stm32f401xe.s|$(CMSIS_DEVICE_DIR)|$(F4_URL)/Source/Templates/gcc/startup_stm32f401xe.s \
  startup_stm32f446xx.s|$(CMSIS_DEVICE_DIR)|$(F4_URL)/Source/Templates/gcc/startup_stm32f446xx.s
# cmsis_device_g4 headers and sources (Apache 2.0)
CMSIS_DOWNLOADS_G4 = \
  stm32g4xx.h|$(CMSIS_DEVICE_DIR)|$(G4_URL)/Include/stm32g4xx.h \
  stm32g474xx.h|$(CMSIS_DEVICE_DIR)|$(G4_URL)/Include/stm32g474xx.h \
  system_stm32g4xx.h|$(CMSIS_DEVICE_DIR)|$(G4_URL)/Include/system_stm32g4xx.h \
  system_stm32g4xx.c|$(CMSIS_DEVICE_DIR)|$(G4_URL)/Source/Templates/system_stm32g4xx.c \
  startup_stm32g474xx.s|$(CMSIS_DEVICE_DIR)|$(G4_URL)/Source/Templates/gcc/startup_stm32g474xx.s
# cmsis-device-f3 headers and sources (Apache 2.0). The repository spells its
# name with a hyphen, unlike the cmsis_device_* ones above.
# cmsis_device_h5 headers and sources (Apache 2.0). No SVD upstream
# (cmsis-svd-data carries no STM32H503 file), so the H5 part file leaves
# CHIP_SVD empty; ST-Link flashing and debugging do not need it.
CMSIS_DOWNLOADS_H5 = \
  stm32h5xx.h|$(CMSIS_DEVICE_DIR)|$(H5_URL)/Include/stm32h5xx.h \
  stm32h503xx.h|$(CMSIS_DEVICE_DIR)|$(H5_URL)/Include/stm32h503xx.h \
  system_stm32h5xx.h|$(CMSIS_DEVICE_DIR)|$(H5_URL)/Include/system_stm32h5xx.h \
  system_stm32h5xx.c|$(CMSIS_DEVICE_DIR)|$(H5_URL)/Source/Templates/system_stm32h5xx.c \
  startup_stm32h503xx.s|$(CMSIS_DEVICE_DIR)|$(H5_URL)/Source/Templates/gcc/startup_stm32h503xx.s
CMSIS_DOWNLOADS_F3 = \
  stm32f3xx.h|$(CMSIS_DEVICE_DIR)|$(F3_URL)/Include/stm32f3xx.h \
  stm32f303xc.h|$(CMSIS_DEVICE_DIR)|$(F3_URL)/Include/stm32f303xc.h \
  system_stm32f3xx.h|$(CMSIS_DEVICE_DIR)|$(F3_URL)/Include/system_stm32f3xx.h \
  system_stm32f3xx.c|$(CMSIS_DEVICE_DIR)|$(F3_URL)/Source/Templates/system_stm32f3xx.c \
  startup_stm32f303xc.s|$(CMSIS_DEVICE_DIR)|$(F3_URL)/Source/Templates/gcc/startup_stm32f303xc.s
# SVD files (debug register views for Ozone / VSCode cortex-debug)
CMSIS_DOWNLOADS_SVD = \
  STM32F103xx.svd|$(CMSIS_DEVICE_DIR)|$(SVD_URL_F1) \
  STM32F030.svd|$(CMSIS_DEVICE_DIR)|$(SVD_URL_F0) \
  STM32G031.svd|$(CMSIS_DEVICE_DIR)|$(SVD_URL_G0) \
  STM32F407.svd|$(CMSIS_DEVICE_DIR)|$(SVD_URL_F4) \
  STM32F401.svd|$(CMSIS_DEVICE_DIR)|$(SVD_URL_F401) \
  STM32F446.svd|$(CMSIS_DEVICE_DIR)|$(SVD_URL_F446) \
  STM32F303.svd|$(CMSIS_DEVICE_DIR)|$(SVD_URL_F303) \
  STM32G474xx.svd|$(CMSIS_DEVICE_DIR)|$(SVD_URL_G4)

CMSIS_DOWNLOADS = $(CMSIS_DOWNLOADS_CORE) $(CMSIS_DOWNLOADS_F1) \
                 $(CMSIS_DOWNLOADS_F0) $(CMSIS_DOWNLOADS_G0) \
                 $(CMSIS_DOWNLOADS_F4) $(CMSIS_DOWNLOADS_F3) \
                 $(CMSIS_DOWNLOADS_G4) $(CMSIS_DOWNLOADS_H5) \
                 $(CMSIS_DOWNLOADS_SVD)

# Generate the rules. $$(call ...) rather than $(call ...), and $$@ rather
# than $@: the body is expanded twice before the rule ever runs (once by
# $(call), once by $(eval)), and download_file itself carries $$ escapes for
# its own shell variables. Without the extra $ here the retry loop loses its
# $n and every download dies on a syntax error. The two license downloads
# below stay explicit - their targets are variables, not a path.
define CMSIS_DOWNLOAD_RULE
$(word 2,$(1))/$(word 1,$(1)): | $(word 2,$(1))
	$$(call download_file,$(word 3,$(1)),$$@)
endef
$(foreach d,$(CMSIS_DOWNLOADS),$(eval $(call CMSIS_DOWNLOAD_RULE,$(subst |, ,$(d)))))

# License download targets
$(CMSIS_CORE_LICENSE): | $(CMSIS_CORE_DIR)
	$(call download_file,$(CMSIS_CORE_LICENSE_URL),$@)

$(CMSIS_DEVICE_LICENSE): | $(CMSIS_DEVICE_DIR)
	$(call download_file,$(DEVICE_F1_LICENSE_URL),$@)

# Check if files exist and download if missing
check-deps: $(EXTERNAL_DEPS)
	@echo "All build dependencies present"

# Target to download all dependencies
download-deps: check-deps
	@echo "All build dependencies checked/downloaded successfully"

# Target to download all license files
download-licenses: $(LICENSE_FILES)
	@echo "All license files downloaded"

# Downloaded third-party trees: the Makefile CMSIS headers, and the CMake
# FetchContent clones. Deliberately not in 'clean' - dropping the FetchContent
# cache would cost a re-clone on the next cmake configure.
clean-deps:
	rm -rf $(CMSIS_CORE_DIR) $(CMSIS_DEVICE_DIR) _deps

# =============================================================================
# PART MATRIX CHECKS (chips/*.mk)
# =============================================================================
# Checks that every chips/<part>.mk names repository files that exist, that its
# scalars are well formed, and that an unknown family or part is rejected
# instead of falling through to the F1 default. The work lives in a script:
# reading three values out of a flat file and testing them against the
# filesystem is what a shell does well, and doing it here needed a parse-time
# include plus an eval'd conditional plus a sub-make per part.

.PHONY: test-mocks
# Needs the real CMSIS device headers of *every* family to compare against,
# and a plain download-deps only fetches the active OW_TARGET's. Recursing
# per family keeps the requirement with the target instead of with every
# caller, the way clock-ref-check does.
#
# Per *part* as well as per family, because check_mock_headers.sh reads one F4
# mock against two real F4 headers: stm32f407xx.h for the family default and
# stm32f446xx.h for the over-drive bits, which stm32f407xx.h does not define at
# all. Recursing per family alone leaves the F446 header unfetched, and the check
# then fails on a clean checkout with "missing stm32f446xx.h" - which is exactly
# what it did in CI, where nothing had ever downloaded that part. Locally it
# passed only because the header was already sitting in CMSIS/ from earlier work.
#
# These are the parts the script compares against, so adding one to
# tests/check_mock_headers.sh means adding one here too. Flat target:part pairs
# because a foreach nested inside another cannot resolve MOCK_CHECK_PARTS_$(t)
# - the inner reference expands before t is bound and comes out empty.
MOCK_CHECK_PARTS = f1:f103xb f0:f030x6 f3:f303xc g0:g031xx f4:f407xx f4:f446xx g4:g474cb
test-mocks:
	$(foreach tp,$(MOCK_CHECK_PARTS),$(MAKE) OW_TARGET=$(word 1,$(subst :, ,$(tp))) OW_CHIP=$(word 2,$(subst :, ,$(tp))) download-deps &&) true
	@sh tests/check_mock_headers.sh

.PHONY: test-chips
test-chips:
	@sh tests/check_chips.sh

# --- Does a build produce the firmware it was asked for? ---
# Needs the ARM toolchain, so it cannot live in test-chips or the other
# repo-side checks: it builds two families and compares the resulting bytes.
.PHONY: test-elf-variant
test-elf-variant:
	@sh tests/check_elf_variant.sh

# --- PlatformIO manifest (see tests/check_library_manifest.sh) ---
# PlatformIO has no install tree to check, so library.json is the whole
# contract, and nothing was reading it. Needs python3 and host gcc, both
# present on a stock runner.
.PHONY: test-manifest
test-manifest:
	@sh tests/check_library_manifest.sh

# --- Project version consistency (see tests/check_version.sh) ---
# The version is declared in eight places; inc/ds18b20.h is the source the
# others are compared against. Also requires CHANGELOG.md to carry a dated
# section for it, which is the check whose absence let v1.8.1 ship untitled.
test-version:
	@sh tests/check_version.sh

# --- Library .bss budget (see tests/check_ram_budget.sh) ---
# The phase-union work bought back ~200-430 bytes of .bss; this gate keeps it.
# Needs the Arm toolchain (GCC_PATH as with firmware builds) and CMSIS deps.
# Budgets are exact ceilings per family: any growth must update them knowingly.
.PHONY: test-ram
test-ram:
	@sh tests/check_ram_budget.sh

# =============================================================================
# BUILD TARGETS
# =============================================================================

# Set 'all' as the default target
.DEFAULT_GOAL := all

# Build all targets by default: the ELF binary, the HEX file, and the raw binary file
all: download-deps $(BUILD_DIR)/$(TARGET).elf $(BUILD_DIR)/$(TARGET).hex $(BUILD_DIR)/$(TARGET).bin

# Define the object files that need to be built from C and assembly source files.
# Every object is prefixed with the app name and the selected part, because the
# compile flags differ along both axes and a shared object name would otherwise
# be reused stale across invocations:
#   APP=      -DUART_TX_BUF_SIZE
#   OW_TARGET -mcpu / -DOW_PORT_TARGET_* / -DOW_PORT_SYSCLK_MHZ
#   OW_CHIP   -DSTM32Fxxx, the startup file, the linker script
# The part component is not optional. With only the app prefix, `make OW_TARGET=f1`
# followed by `make OW_TARGET=f0` reused the f1 objects: only the startup and
# system files, whose basenames happen to differ per part, were recompiled, so
# the f0 firmware went on to link Cortex-M3 code compiled against
# port/stm32f1/ow_port_f1.h with a 72 MHz prescaler - the two families' bit-slot
# timings differ by that factor. It showed up as a confusing assembler failure
# ("invalid constant after fixup") on the one freshly-named file rather than as
# the stale link it actually was, and switching families within one build tree
# is ordinary usage, not a corner case.
#
# The clock knobs belong in the stamp for the same reason, and harder: SYSCLK_MHZ
# and HSE_MHZ both reach the firmware as -DOW_PORT_SYSCLK_MHZ / -DOW_HSE_MHZ, so
# two builds of the same part that differ only in either are different firmware.
# Without them in the name, building the 180MHz, raw-HSI 16MHz and raw-HSE 8MHz
# variants of one part in sequence reused the first build's objects for the other
# two, and each of those silently shipped the 180MHz prescaler, bit-slot timings
# and console divisor. That is the most dangerous shape this bug can take: it
# compiles clean, links, and is wrong on hardware only.
#
# The values are the *effective* ones (the command line, else the part default),
# so a default build and an explicit one that agree still share objects.
OW_EFF_SYSCLK := $(if $(SYSCLK_MHZ),$(SYSCLK_MHZ),$(CHIP_SYSCLK_MHZ))
OW_EFF_HSE := $(if $(HSE_MHZ),$(HSE_MHZ),$(CHIP_HSE_MHZ))
# EXT is a per-build knob (-D... user flags) that reaches every translation
# unit, so it belongs in the stamp for the very reason the comment above gives
# for the clock knobs: two builds of the same part that differ only in EXT are
# different firmware, and without EXT in the object name rebuilding one after
# the other silently reuses the first build's objects. Sanitise the flag list
# into a filename-safe tag (spaces -> underscores).
OW_EFF_EXT := $(strip $(EXT))
OW_EXT_TAG := $(subst $(space),_,$(OW_EFF_EXT))
OBJ_STAMP = $(OW_TARGET)_$(OW_CHIP)_$(OW_EFF_SYSCLK)mhz$(if $(OW_EFF_HSE),_hse$(OW_EFF_HSE),)$(if $(OW_EFF_EXT),_$(OW_EXT_TAG))
OBJ = $(addprefix $(BUILD_DIR)/$(APP)_$(OBJ_STAMP)_,$(notdir $(SRC:.c=.o)))
vpath %.c $(sort $(dir $(SRC))) # Set the search path for C source files

OBJ += $(addprefix $(BUILD_DIR)/$(APP)_$(OBJ_STAMP)_,$(notdir $(ASM:.s=.o)))
vpath %.s $(sort $(dir $(ASM))) # Set the search path for A source files

# --- Explicit dependency lists (do NOT rely on the -MMD depfiles) ---
#
# The depfiles this build generates end up with CRLF line endings on Windows,
# and GNU make only honours a trailing backslash as a line continuation when
# it is the very last byte of the line. The CR in front of the LF therefore
# terminates the rule early, so every prerequisite the depfile lists is
# silently dropped: -include still reads the file, but the target keeps only
# the pattern rule's own `%.c` prerequisite.
#
# The failure is invisible and dangerous. `make` prints a normal compile, the
# build is clean, and it links a stale object - editing a header or an
# include-only driver part recompiles nothing. During the F4 rearm bench that
# made several runs flash the previous day's firmware and produced diagnostic
# output that could never appear in the image (the strings were absent from the
# ELF), which sent the investigation after two wrong root causes. Linux CI does
# not see it, the depfiles there use LF.
#
# So the dependencies are listed explicitly instead. Cost: one extra stat per
# header per object, which is negligible next to the compile it may trigger.
OW_INC_HDRS = $(wildcard inc/*.h) $(wildcard examples/app/*.h) \
              $(wildcard port/common/*.h) $(wildcard port/stm32f1/*.h) \
              $(wildcard port/stm32f0/*.h) $(wildcard port/stm32f3/*.h) \
              $(wildcard port/stm32g0/*.h) $(wildcard port/stm32f4/*.h) \
              $(wildcard port/stm32g4/*.h)

# src/ds18b20.c is an umbrella translation unit: it #includes the driver parts
# below, so editing one of them must rebuild the object. -MP gives each part a
# phony target of its own, which would otherwise defeat the dependency even
# where depfiles do parse.
OW_DRIVER_PARTS = src/ds18b20_resolution.c src/ds18b20_txn.c \
                  src/ds18b20_search.c src/ds18b20_measure.c

# Specify how to compile a C source file into an object file
$(BUILD_DIR)/$(APP)_$(OBJ_STAMP)_%.o: %.c $(OW_INC_HDRS) Makefile | $(BUILD_DIR)
	$(CC) -c $(FLAG) $(OPT) $(EXT) $< -o $@

# The umbrella unit needs the include-only parts on top of the shared list.
$(BUILD_DIR)/$(APP)_$(OBJ_STAMP)_ds18b20.o: $(OW_DRIVER_PARTS)

# Specify how to compile an assembly source file into an object file
$(BUILD_DIR)/$(APP)_$(OBJ_STAMP)_%.o: %.s Makefile | $(BUILD_DIR)
	$(AS) -c $(FLAG) $(OPT) $(EXT) -Wa,-a,-ad,-alms=$(BUILD_DIR)/$(APP)_$(OBJ_STAMP)_$(notdir $(<:.s=.lst)) $< -o $@

# Always relink, even when nothing is older than the ELF.
#
# The output name is ds18b20_<APP>.elf, so it encodes the app and nothing else,
# while $(OBJ) is stamped with the family, part and clock. Building one variant
# and then another therefore leaves two different object lists pointing at one
# ELF: after `make OW_TARGET=f0 APP=x` followed by `make OW_TARGET=f4
# OW_CHIP=f446xx APP=x`, the F4 objects are all older than the F0 ELF that is
# still sitting there, make considers the link up to date, and `make program`
# flashes the previous build's hex. It compiles clean, passes every check, and
# puts the wrong firmware on the board - which is how it reached hardware three
# times in one session.
#
# Stamping the output name instead would fix it and break every path that names
# the artefact: st-flash, the J-Flash project, the Ozone project and the README
# all refer to build/ds18b20_<APP>.hex. Forcing the relink keeps those names and
# costs one link invocation. Object-level incrementality - the part that actually
# costs time - is unaffected: $(OBJ) is still per-variant, so a rebuild of the same
# variant recompiles nothing.
.PHONY: FORCE_RELINK
FORCE_RELINK:

# Include the depfiles gcc writes next to the objects (see FLAG).
# NOTE: the depfiles are NOT read. gcc writes them with CRLF on Windows, which
# breaks their continuation lines, and with -MP removed they only ever added
# phony targets. The object rule below carries the real dependency list.

# Specify how to build the final executable file
$(BUILD_DIR)/$(TARGET).elf: $(OBJ) Makefile FORCE_RELINK
	$(CC) $(OBJ) $(LDFLAGS) $(OPT) $(EXT) -o $@
	$(SZ) $@

# Specify how to build the hex file using the elf file
$(BUILD_DIR)/%.hex: $(BUILD_DIR)/%.elf | $(BUILD_DIR)
	$(HEX) $< $@

# Specify how to build the bin file using the elf file
$(BUILD_DIR)/%.bin: $(BUILD_DIR)/%.elf | $(BUILD_DIR)
	$(BIN) $< $@

# Create the build directory if it doesn't exist
$(BUILD_DIR):
	mkdir $@

# Perform the 'debug' target, which enables debug symbols and builds the project
debug: OPT = $(OPT_DEBUG)
debug: download-deps all

# Display compiler version information.
gccversion :
	@$(CC) --version

# Program the device using st-link.
program: $(BUILD_DIR)/$(TARGET).hex
	$(STLINK) $(STLINK_FLAGS)

# Program the device using jlink.
jprogram: $(BUILD_DIR)/$(TARGET).hex
	$(JLINK) $(JLINK_FLAGS)

# Build artifacts, plus anything a CMake install left outside build/ (the
# ci.yml cmake job installs under build/, a local one need not). The glob
# matches nothing on a fresh tree and rm -f on a non-existent path exits 0,
# so this is safe in a 'make clean && make' chain.
.PHONY: clean
clean:
	rm -fR $(BUILD_DIR) prefix*/

# =============================================================================
# HOST TESTS (compiled with the host toolchain, run on the build machine)
# The driver is compiled through tests/mock/ds18b20_test_access.c (which
# #includes src/ds18b20.c); hardware behaviour is simulated by hw_model.c.
# =============================================================================

HOST_CC ?= gcc
TEST_MOCK = tests/mock
TEST_DIR  = tests/test
TEST_OUT  = build/test
TEST_SRC  = $(TEST_DIR)/test_main.c \
            $(TEST_DIR)/test_state_machine.c \
            $(TEST_DIR)/test_scratchpad.c \
            $(TEST_DIR)/test_bus_release.c \
            $(TEST_DIR)/test_search.c \
            $(TEST_DIR)/test_alarm_search.c \
            $(TEST_DIR)/test_crc8.c \
            $(TEST_DIR)/test_pulse_encoding.c \
            $(TEST_DIR)/test_presence.c \
            $(TEST_DIR)/test_rom_addressing.c \
            $(TEST_DIR)/test_timing.c \
            $(TEST_DIR)/test_temperature.c \
            $(TEST_DIR)/test_resolution.c \
            $(TEST_DIR)/test_broadcast.c \
            $(TEST_DIR)/test_read_rom.c \
            $(TEST_DIR)/test_alarm_thresholds.c \
            $(TEST_DIR)/test_eeprom.c \
            $(TEST_DIR)/test_parasite.c \
            $(TEST_DIR)/test_dmamux.c \
            $(TEST_DIR)/test_dma.c \
            $(TEST_DIR)/test_dma_contract.c \
            $(TEST_DIR)/test_port_init_contract.c \
            $(TEST_DIR)/test_ow_stats.c \
            $(TEST_DIR)/test_rcr_limits.c \
            $(TEST_DIR)/test_tim_model.c \
             $(TEST_MOCK)/hw_model.c \
            $(TEST_MOCK)/ds18b20_test_spy.c \
            $(TEST_MOCK)/ds18b20_test_access.c \
            $(TEST_MOCK)/ow_stats_test_access.c \
            $(TEST_DIR)/test_harness_api.c \
            $(TEST_DIR)/test_app_uart.c
# Host tests compile at production optimization (-O2 -flto) instead of -O0,
# which re-reads memory by construction.  The compiler-enforced guard for a
# lost 'volatile' qualifier on the DMA capture path is
# -Werror=discarded-qualifiers in TEST_FLAG below: passing a volatile buffer
# into a non-volatile parameter becomes a hard build error, deterministically.
# (LTO alone is not that net -- the synchronous host mock is value-correct, so
# the optimizer may legally forward the mock's DMA writes to the loads, and
# the tests still pass.)  COVERAGE=1 (gcov) conflicts with LTO and opts out
# via TEST_OPT; override anytime with TEST_OPT='' or e.g. TEST_OPT=-O0.
ifeq ($(COVERAGE),1)
TEST_OPT ?= -O0
else
TEST_OPT ?= -O2 -flto
endif
# Pointer<->register casts (driver targets a 32-bit Cortex-M3) are expected
# on a 64-bit host; suppress the size warnings.
# OW_TARGET=f0 runs the same suite against the STM32F0 backend mock,
# OW_TARGET=g0 against the STM32G0 backend mock.
ifeq ($(OW_TARGET),f0)
TEST_PORT_FLAG = -DOW_PORT_TARGET_F0
TEST_PORT_INC = -Iport/stm32f0
TEST_EXE = $(TEST_OUT)/ds18b20_test_f0.exe
else ifeq ($(OW_TARGET),g0)
TEST_PORT_FLAG = -DOW_PORT_TARGET_G0
TEST_PORT_INC = -Iport/stm32g0
TEST_EXE = $(TEST_OUT)/ds18b20_test_g0.exe
else ifeq ($(OW_TARGET),f4)
TEST_PORT_FLAG = -DOW_PORT_TARGET_F4
TEST_PORT_INC = -Iport/stm32f4
TEST_EXE = $(TEST_OUT)/ds18b20_test_f4.exe
else ifeq ($(OW_TARGET),f3)
TEST_PORT_FLAG = -DOW_PORT_TARGET_F3
TEST_PORT_INC = -Iport/stm32f3
TEST_EXE = $(TEST_OUT)/ds18b20_test_f3.exe
else ifeq ($(OW_TARGET),g4)
TEST_PORT_FLAG = -DOW_PORT_TARGET_G4
TEST_PORT_INC = -Iport/stm32g4
TEST_EXE = $(TEST_OUT)/ds18b20_test_g4.exe
else ifeq ($(OW_TARGET),h5)
TEST_PORT_FLAG = -DOW_PORT_TARGET_H5
TEST_PORT_INC = -Iport/stm32h5
TEST_EXE = $(TEST_OUT)/ds18b20_test_h5.exe
else
TEST_PORT_FLAG = -DOW_PORT_TARGET_F1
TEST_PORT_INC = -Iport/stm32f1
TEST_EXE = $(TEST_OUT)/ds18b20_test.exe
endif
# Hook for defines only the host suite should see, the counterpart of EXT
# for the firmware build. It is how the port setup contract tables were
# generated: `make test TEST_EXTRA_FLAG=-DOW_PORT_INIT_CAPTURE` makes the
# contract test print what each backend programs instead of asserting, so
# the expectations are the backends' real behaviour rather than my
# reading of them. Empty by default, so the normal suite is unaffected.
TEST_EXTRA_FLAG ?=

TEST_FLAG = -DHOST_BUILD -DDS18B20_TEST_HARNESS -DOW_STATS_ENABLE=1 $(TEST_PORT_FLAG) $(TEST_EXTRA_FLAG) -Wall -Wextra -Wswitch-enum \
            -Werror=discarded-qualifiers \
            -Wno-unused-parameter -Wno-pointer-to-int-cast -Wno-int-to-pointer-cast \
            $(if $(COVERAGE),--coverage,)
TEST_INC  = -Iinc -Iexamples/app -Iport/common $(TEST_PORT_INC) -I$(TEST_MOCK)

# Extra host executables for this family, built and run alongside $(TEST_EXE).
# Empty for every family but F4, which carries a second clock that is a
# genuinely different code path rather than a different constant: the F446's
# 180MHz brings the over-drive sequence and its own PLL branch, and the 168MHz
# F407 clock has neither - stm32f407xx.h does not even define PWR_CR_ODEN. One
# suite, two clocks, so the 180MHz asserts in test_timing.c (PWR clock, ODEN,
# ODSWEN, 5 wait states, PLLN=360) are compiled and run at all, and so a
# regression in that branch cannot hide behind the 168MHz build passing.
ifneq ($(filter f4,$(OW_TARGET)),)
TEST_F4_180_FLAG = $(TEST_FLAG) -DSTM32F446xx -DOW_PORT_SYSCLK_MHZ=180
TEST_F4_180_EXE = $(TEST_OUT)/ds18b20_test_f4_180mhz.exe
TEST_EXTRA_EXES = $(TEST_F4_180_EXE)
endif
# Mirror of the F4 second clock: the G4's 16MHz raw-HSI branch is a genuinely
# different code path from the 170MHz HSE+PLL default (no PLL, no flash
# latency change, SWS stays on HSI), so the suite runs at both. The default
# build already covers 170MHz through the header default, hence the extra
# executable forces the low clock instead of the high one.
ifneq ($(filter g4,$(OW_TARGET)),)
TEST_G4_16_FLAG = $(TEST_FLAG) -DSTM32G474xx -DOW_PORT_SYSCLK_MHZ=16
TEST_G4_16_EXE = $(TEST_OUT)/ds18b20_test_g4_16mhz.exe
TEST_EXTRA_EXES = $(TEST_G4_16_EXE)
endif

# Per-family suffix for the host-test artefacts, so the six families do not
# share one output file. Defined once because three near-identical nested-if
# chains had already drifted: adding f3 to the low-power one and not the other
# two left the active-drive and ndebug names with an unbalanced paren, which
# only the ndebug job noticed - and it failed on CI while all five host-test
# jobs went red on an artefact name rather than on a test.
TEST_FAM_SUFFIX = $(if $(filter f0,$(OW_TARGET)),_f0,$(if $(filter f3,$(OW_TARGET)),_f3,$(if $(filter g0,$(OW_TARGET)),_g0,$(if $(filter f4,$(OW_TARGET)),_f4,$(if $(filter g4,$(OW_TARGET)),_g4,$(if $(filter h5,$(OW_TARGET)),_h5,))))))

# Low-power variant: the same suite re-built with -DOW_PORT_LOW_POWER=1.
TEST_LP_FLAG = $(TEST_FLAG) -DOW_PORT_LOW_POWER=1
TEST_LP_EXE = $(TEST_OUT)/ds18b20_test_lowpower$(TEST_FAM_SUFFIX).exe

TEST_CLOCK_FLAG = $(if $(filter f0,$(1)),STM32F0,$(if $(filter f3,$(1)),STM32F3,$(if $(filter g0,$(1)),STM32G0,$(if $(filter f4,$(1)),STM32F4,$(if $(filter g4,$(1)),STM32G4,$(if $(filter h5,$(1)),STM32H5,STM32F1))))))
# This one defaults to _f1 rather than to nothing, so the family-macro compile
# check keeps a distinct object for the default family; $(or) supplies that only
# when the shared suffix is empty.
TEST_CLOCK_OBJ = $(TEST_OUT)/test_sysclk_fallback$(or $(TEST_FAM_SUFFIX),_f1).o
# Per-part F4 fallback compile check: always built as part of `make test`,
# independent of the active OW_TARGET (see F4_PART_CLOCK_CHECKS below).

test: $(TEST_EXE) $(TEST_EXTRA_EXES) $(TEST_CLOCK_OBJ) clock-ref-check
	$(TEST_EXE)
	$(foreach x,$(TEST_EXTRA_EXES),$(x);)

# --- Per-family wrappers, generated ---------------------------------------
# Every variant is written once for the active OW_TARGET and reached from
# another family through a generated test-<variant>-<family> wrapper, so the set
# of families a variant is checked on is OW_KNOWN_TARGETS in one place instead of
# a hand-maintained list per variant. These were 15 near-identical rules that
# had already drifted: test-f1 was missing (f1 being the default, only bare
# `make test` covered it), and each variant listed its own three families, so
# adding one meant editing four places. test-clocks is generated too, but builds
# a compile-check object rather than delegating to a variant target.
# Variant SUFFIXES, not target names: "base" stands for the plain `test` target.
# Suffixes are what let a single function name both a variant's target and its
# wrapper, so the two can never disagree about how a variant is called. Note that
# an empty list element is not usable as the sentinel - `""` in a Make variable
# is two quote characters, not an empty string.
OW_TEST_VARIANTS = base lowpower active ndebug

ow_variant_target = $(if $(filter base,$(1)),test,test-$(1))
ow_wrapper_name = $(call ow_variant_target,$(1))-$(2)

define OWRULE_TEST_VARIANT
$(call ow_wrapper_name,$(1),$(2)):
	$$(MAKE) OW_TARGET=$(2) $(call ow_variant_target,$(1))
endef

define OWRULE_TEST_CLOCKS
test-clocks-$(1):
	$$(MAKE) OW_TARGET=$(1) $(TEST_OUT)/test_sysclk_fallback_$(1).o
endef

OW_TEST_WRAPPERS = $(foreach v,$(OW_TEST_VARIANTS),\
                     $(foreach f,$(OW_KNOWN_TARGETS),$(call ow_wrapper_name,$(v),$(f)))) \
                   $(foreach f,$(OW_KNOWN_TARGETS),test-clocks-$(f))
.PHONY: $(OW_TEST_WRAPPERS)
$(foreach v,$(OW_TEST_VARIANTS),\
   $(foreach f,$(OW_KNOWN_TARGETS),$(eval $(call OWRULE_TEST_VARIANT,$(v),$(f)))))
$(foreach f,$(OW_KNOWN_TARGETS),$(eval $(call OWRULE_TEST_CLOCKS,$(f))))

# --- Family-macro fallback compile check (see test_sysclk_fallback.c) ---
# Compile-only: verifies that selecting a family through the raw family macro
# (STM32F1/F0/G0, the PlatformIO/STM32CubeMX path) resolves both
# OW_PORT_FAMILY_* and the OW_PORT_SYSCLK_MHZ default for the backend under
# test. Runs as part of every test build so the two can never drift.

$(TEST_CLOCK_OBJ): tests/test/test_sysclk_fallback.c Makefile | $(TEST_OUT)
	$(HOST_CC) -c -DOW_CHIP_SYSCLK_MHZ=$(CHIP_SYSCLK_MHZ) -D$(call TEST_CLOCK_FLAG,$(OW_TARGET)) $(TEST_INC) tests/test/test_sysclk_fallback.c -o $@

# --- Per-part F4 fallback compile check (see test_sysclk_fallback.c) ---
# Compile-only, built as part of every `make test` (via clock-ref-check): verifies
# that selecting the F4 family together with a device macro that is not the plain
# F407 one resolves the F4 backend with *that part's* clock default - 84 MHz for
# the STM32F401 parts, 180 MHz for the STM32F446 - instead of the 168 MHz
# F405/F407 default. The macro is taken from chips/<chip>.mk rather than repeated
# here, so renaming the part in one place cannot leave this guard testing a
# macro nothing builds. Deliberately does not pass -DOW_PORT_SYSCLK_MHZ: the
# point is to check the default that onewire.h derives from the part macro alone.
#
# Every F4 part with a default of its own is listed here. Enumerating them in one
# list keeps a newly added variant from silently shipping without a guard - the
# xE part was checked for nothing at all until this was generalised from the
# single f401xc entry it started as, and the F446 is the case that matters most
# (180 MHz is a different PLL branch from 168, not just a different number).
# f407xx is left out on purpose: 168 MHz is the family-wide fallback that
# test_sysclk_fallback_f4.o already covers with the plain STM32F4 macro.
F4_PART_CLOCK_CHECKS = f401xc f401xe f446xx
F4_PART_CLOCK_OBJS = $(foreach c,$(F4_PART_CLOCK_CHECKS),$(TEST_OUT)/test_sysclk_fallback_$(c).o)

# The stem of the target is the part, so one pattern rule serves every F4 part;
# the sub-make below selects OW_CHIP, which is what makes CHIP_DEV_DEF the right
# part's macro. The chips/f4%.mk prerequisite rebuilds the object when the part
# file changes.
$(TEST_OUT)/test_sysclk_fallback_f4%.o: tests/test/test_sysclk_fallback.c Makefile chips/f4%.mk | $(TEST_OUT)
	$(HOST_CC) -c -DOW_CHIP_SYSCLK_MHZ=$(CHIP_SYSCLK_MHZ) -D$(call TEST_CLOCK_FLAG,f4) $(CHIP_DEV_DEF) -Iinc -Iexamples/app -Iport/stm32f4 -Iport/common -I$(TEST_MOCK) \
	    tests/test/test_sysclk_fallback.c -o $@

# Built in a sub-make that selects the f4 family and the part, so CHIP_DEV_DEF is
# that part's macro. Recursion is what makes the part file the single source.
.PHONY: clock-ref-check
clock-ref-check:
	$(foreach c,$(F4_PART_CLOCK_CHECKS),$(MAKE) OW_TARGET=f4 OW_CHIP=$(c) $(TEST_OUT)/test_sysclk_fallback_$(c).o &&) true

.PHONY: test-clocks test-chips
test-clocks: test-chips test-mocks $(foreach f,$(OW_KNOWN_TARGETS),test-clocks-$(f)) clock-ref-check
# Every per-part F4 check, via the list above; kept as a target so the aggregate
# and `make test` reach the same check by a name a reader can find.
test-clocks-f4: clock-ref-check

# --- Opt-in low-power WFE path test build (-DOW_PORT_LOW_POWER=1) ---
# Compiles the SAME suite with the low-power path enabled so the
# __WFE()-related code (SEVONPEND, derived long-stage predicate, UIE) is exercised
# on the host. See tests/test/test_lowpower.c.
.PHONY: test-lowpower
test-lowpower: $(TEST_LP_EXE)
	$(TEST_LP_EXE)

# src/ds18b20.c is an amalgamated translation unit: the search / txn /
# resolution / measurement code lives in these include-only parts (guarded by
# DS18B20_DRIVER_BUILD). They are listed here as prerequisites because the
# test executables compile the sources directly rather than through per-object
# dependency files.
DS18B20_PARTS = src/ds18b20_resolution.c src/ds18b20_txn.c \
                src/ds18b20_search.c src/ds18b20_measure.c

# The headers the host suites compile against, as prerequisites. The firmware
# build gets this from -MMD depfiles, but the host build is a single gcc call
# producing one .exe, so there is no depfile to read - and without this, editing
# a header leaves the executables untouched. That is not a nuisance: it produced
# a *false pass*. Mutating the 180MHz IC4F tier in inc/ow_port.h and re-running
# `make test` reported success, because the binary was stale; only a forced
# rebuild failed. A test that cannot be made to run is worse than no test.
TEST_PORT_DIR = $(patsubst -I%,%,$(TEST_PORT_INC))
TEST_HDRS = $(wildcard inc/*.h) $(wildcard examples/app/*.h) \
            $(wildcard port/common/*.h) $(wildcard $(TEST_PORT_DIR)/*.h) \
            $(wildcard $(TEST_MOCK)/*.h)

$(TEST_EXE): $(TEST_SRC) src/ds18b20.c $(DS18B20_PARTS) src/onewire.c examples/app/app.c $(TEST_HDRS) Makefile | $(TEST_OUT)
	$(HOST_CC) $(TEST_FLAG) $(TEST_INC) $(TEST_OPT) $(TEST_SRC) examples/app/app.c -o $@

# The F4 suite at the F446's 180MHz. Same sources and same test_main, only the
# device macro and the clock differ, so the suite's own expectations have to
# hold at 180MHz as well - the bit-slot constants are µs figures and the TIM
# model is prescaler-driven, which is exactly what should be clock-independent.
# Kept as its own target (rather than a variable in $(TEST_EXE)) so a failure
# names the clock it happened at.
$(TEST_F4_180_EXE): $(TEST_SRC) src/ds18b20.c $(DS18B20_PARTS) src/onewire.c examples/app/app.c $(TEST_HDRS) Makefile | $(TEST_OUT)
	$(HOST_CC) $(TEST_F4_180_FLAG) $(TEST_INC) $(TEST_OPT) $(TEST_SRC) examples/app/app.c -o $@

# The G4 suite at the 16MHz raw-HSI clock. Same sources and same test_main,
# only the clock differs, so the suite's own expectations have to hold at
# 16MHz as well - the bit-slot constants are us figures and the TIM model is
# prescaler-driven, which is exactly what should be clock-independent. Kept
# as its own target (rather than a variable in $(TEST_EXE)) so a failure
# names the clock it happened at.
$(TEST_G4_16_EXE): $(TEST_SRC) src/ds18b20.c $(DS18B20_PARTS) src/onewire.c examples/app/app.c $(TEST_HDRS) Makefile | $(TEST_OUT)
	$(HOST_CC) $(TEST_G4_16_FLAG) $(TEST_INC) $(TEST_OPT) $(TEST_SRC) examples/app/app.c -o $@

$(TEST_LP_EXE): $(TEST_SRC) src/ds18b20.c $(DS18B20_PARTS) src/onewire.c examples/app/app.c tests/test/test_lowpower.c $(TEST_HDRS) Makefile | $(TEST_OUT)
	$(HOST_CC) $(TEST_LP_FLAG) $(TEST_INC) $(TEST_OPT) $(TEST_SRC) tests/test/test_lowpower.c examples/app/app.c -o $@

$(TEST_OUT):
	mkdir -p $@

# --- Active-drive (push-pull write) test build (experimental, -DOW_DRIVE_ACTIVE=1) ---
# Runs only the active-drive test set (the full suite's pin-regression assertions
# assume the pin is never toggled outside the parasite strong-pull-up path).
TEST_ACTIVE_SRC = \
    $(TEST_DIR)/test_main_active.c \
    $(TEST_DIR)/test_active_drive.c \
    $(TEST_MOCK)/hw_model.c \
    $(TEST_MOCK)/ds18b20_test_spy.c \
    $(TEST_MOCK)/ds18b20_test_access.c \
    $(TEST_MOCK)/ow_stats_test_access.c \
    examples/app/app.c
TEST_ACTIVE_FLAG = $(TEST_FLAG) -DOW_DRIVE_ACTIVE=1
TEST_ACTIVE_EXE  = $(TEST_OUT)/ds18b20_test_active$(TEST_FAM_SUFFIX).exe

.PHONY: test-active
test-active: $(TEST_ACTIVE_EXE)
	$(TEST_ACTIVE_EXE)

$(TEST_ACTIVE_EXE): $(TEST_ACTIVE_SRC) src/ds18b20.c $(DS18B20_PARTS) src/onewire.c $(TEST_HDRS) Makefile | $(TEST_OUT)
	$(HOST_CC) $(TEST_ACTIVE_FLAG) $(TEST_INC) $(TEST_OPT) $(TEST_ACTIVE_SRC) -o $@

# --- Release-semantics build (-DNDEBUG + OW_TEST_PARAM_GUARD) ---
# Rebuilds the same suite with the public API asserts compiled out
# (-DNDEBUG), so the onewire_write_slots/read_data reject paths become
# observable as return codes instead of aborting the process. Backend
# reject paths run fail-soft in every test variant.
# See tests/test/test_param_guard.c and test_rcr_limits.c.
TEST_NG_FLAG = $(TEST_FLAG) -DNDEBUG -DOW_TEST_PARAM_GUARD
TEST_NG_SRC  = $(TEST_SRC) $(TEST_DIR)/test_param_guard.c
TEST_NG_EXE  = $(TEST_OUT)/ds18b20_test_ndebug$(TEST_FAM_SUFFIX).exe

.PHONY: test-ndebug
test-ndebug: $(TEST_NG_EXE)
	$(TEST_NG_EXE)

$(TEST_NG_EXE): $(TEST_NG_SRC) src/ds18b20.c $(DS18B20_PARTS) src/onewire.c examples/app/app.c $(TEST_HDRS) Makefile | $(TEST_OUT)
	$(HOST_CC) $(TEST_NG_FLAG) $(TEST_INC) $(TEST_OPT) $(TEST_NG_SRC) examples/app/app.c -o $@

# Depfiles are generated but deliberately NOT included: gcc writes them with
# CRLF on Windows, GNU make does not honour a continuation whose backslash is
# followed by CR, so every prerequisite they list is dropped while the include
# itself still succeeds. The object rule carries the same dependencies
# explicitly (OW_INC_HDRS / OW_DRIVER_PARTS).

# =============================================================================
# FUZZ TARGETS (requires Clang with libFuzzer / SanitizerCoverage,
#              or GCC with AddressSanitizer + UndefinedBehaviorSanitizer)
# =============================================================================

FUZZ_CC      ?= clang
FUZZ_CFLAGS  = -fsanitize=fuzzer,address,undefined -g -O1 \
               -DFUZZING_BUILD_MODE_UNSAFE_FOR_PRODUCTION \
               -DHOST_BUILD -DOW_PORT_TARGET_F1 -Iinc -Iport/stm32f1 -Iport/common -Itests/mock \
               -Wno-pointer-to-int-cast -Wno-int-to-pointer-cast
FUZZ_LDFLAGS = -fsanitize=fuzzer,address,undefined
FUZZ_OUT     = build/fuzz
FUZZ_TIME    ?= 120
FUZZ_HW_MOCK = tests/mock/hw_model.c

.PHONY: fuzz-crc8 fuzz-decode-pulses fuzz-present fuzz-pair-bits \
        fuzz-encode-byte fuzz-bit-from-pulse \
        fuzz-stats fuzz-ds18b20-decode fuzz-search fuzz-resolution fuzz-all

$(FUZZ_OUT):
	mkdir -p $@

# Tier 1-2: standalone onewire.c
fuzz-crc8: | $(FUZZ_OUT)
	$(FUZZ_CC) $(FUZZ_CFLAGS) tests/fuzz/fuzz_crc8.c src/onewire.c $(FUZZ_HW_MOCK) \
	    $(FUZZ_LDFLAGS) -o $(FUZZ_OUT)/fuzz_crc8
	$(FUZZ_OUT)/fuzz_crc8 -max_total_time=$(FUZZ_TIME) -print_final_stats=1

fuzz-decode-pulses: | $(FUZZ_OUT)
	$(FUZZ_CC) $(FUZZ_CFLAGS) tests/fuzz/fuzz_decode_pulses.c src/onewire.c $(FUZZ_HW_MOCK) \
	    $(FUZZ_LDFLAGS) -o $(FUZZ_OUT)/fuzz_decode_pulses
	$(FUZZ_OUT)/fuzz_decode_pulses -max_total_time=$(FUZZ_TIME) -print_final_stats=1

fuzz-present: | $(FUZZ_OUT)
	$(FUZZ_CC) $(FUZZ_CFLAGS) tests/fuzz/fuzz_present.c src/onewire.c $(FUZZ_HW_MOCK) \
	    $(FUZZ_LDFLAGS) -o $(FUZZ_OUT)/fuzz_present
	$(FUZZ_OUT)/fuzz_present -max_total_time=$(FUZZ_TIME) -print_final_stats=1

fuzz-pair-bits: | $(FUZZ_OUT)
	$(FUZZ_CC) $(FUZZ_CFLAGS) tests/fuzz/fuzz_pair_bits.c src/onewire.c $(FUZZ_HW_MOCK) \
	    $(FUZZ_LDFLAGS) -o $(FUZZ_OUT)/fuzz_pair_bits
	$(FUZZ_OUT)/fuzz_pair_bits -max_total_time=$(FUZZ_TIME) -print_final_stats=1

fuzz-encode-byte: | $(FUZZ_OUT)
	$(FUZZ_CC) $(FUZZ_CFLAGS) tests/fuzz/fuzz_encode_byte.c src/onewire.c $(FUZZ_HW_MOCK) \
	    $(FUZZ_LDFLAGS) -o $(FUZZ_OUT)/fuzz_encode_byte
	$(FUZZ_OUT)/fuzz_encode_byte -max_total_time=$(FUZZ_TIME) -print_final_stats=1

fuzz-bit-from-pulse: | $(FUZZ_OUT)
	$(FUZZ_CC) $(FUZZ_CFLAGS) tests/fuzz/fuzz_bit_from_pulse.c src/onewire.c $(FUZZ_HW_MOCK) \
	    $(FUZZ_LDFLAGS) -o $(FUZZ_OUT)/fuzz_bit_from_pulse
	$(FUZZ_OUT)/fuzz_bit_from_pulse -max_total_time=$(FUZZ_TIME) -print_final_stats=1

# Tier 3: ow_stats (single-TU, #include)
fuzz-stats: | $(FUZZ_OUT)
	$(FUZZ_CC) $(FUZZ_CFLAGS) -Isrc -DOW_STATS_ENABLE=1 \
	    tests/fuzz/fuzz_stats.c $(FUZZ_HW_MOCK) tests/mock/uart_stub.c \
	    $(FUZZ_LDFLAGS) -o $(FUZZ_OUT)/fuzz_stats
	$(FUZZ_OUT)/fuzz_stats -max_total_time=$(FUZZ_TIME) -print_final_stats=1

# Tier 4: ds18b20 decode (single-TU via test_access)
fuzz-ds18b20-decode: | $(FUZZ_OUT)
	$(FUZZ_CC) $(FUZZ_CFLAGS) -Isrc -DDS18B20_TEST_HARNESS -DOW_STATS_ENABLE=1 \
	    tests/fuzz/fuzz_ds18b20_decode.c src/ow_stats.c \
	    $(FUZZ_HW_MOCK) tests/mock/uart_stub.c \
	    $(FUZZ_LDFLAGS) -o $(FUZZ_OUT)/fuzz_ds18b20_decode
	$(FUZZ_OUT)/fuzz_ds18b20_decode -max_total_time=$(FUZZ_TIME) -print_final_stats=1

# Tier 5: Search ROM / Alarm Search state machine (single-TU via test_access).
# The fuzz input drives the capture source; every property is checked with
# abort() so libFuzzer reports the failing input.
fuzz-search: | $(FUZZ_OUT)
	$(FUZZ_CC) $(FUZZ_CFLAGS) -Isrc -DDS18B20_TEST_HARNESS -DOW_STATS_ENABLE=1 \
	    tests/fuzz/fuzz_search.c src/ow_stats.c \
	    $(FUZZ_HW_MOCK) tests/mock/uart_stub.c \
	    $(FUZZ_LDFLAGS) -o $(FUZZ_OUT)/fuzz_search
	$(FUZZ_OUT)/fuzz_search -max_total_time=$(FUZZ_TIME) -print_final_stats=1

# Tier 6: resolution change state machine (single-TU via test_access); fuzz
# input drives the presence reset and the address mode (Skip vs Match ROM).
fuzz-resolution: | $(FUZZ_OUT)
	$(FUZZ_CC) $(FUZZ_CFLAGS) -Isrc -DDS18B20_TEST_HARNESS -DOW_STATS_ENABLE=1 \
	    tests/fuzz/fuzz_resolution.c src/ow_stats.c \
	    $(FUZZ_HW_MOCK) tests/mock/uart_stub.c \
	    $(FUZZ_LDFLAGS) -o $(FUZZ_OUT)/fuzz_resolution
	$(FUZZ_OUT)/fuzz_resolution -max_total_time=$(FUZZ_TIME) -print_final_stats=1

fuzz-all: fuzz-crc8 fuzz-decode-pulses fuzz-present fuzz-pair-bits \
          fuzz-encode-byte fuzz-bit-from-pulse \
          fuzz-stats fuzz-ds18b20-decode fuzz-search fuzz-resolution

# Help target
help:
	@echo "Available targets:"
	@echo "  all             - Build project (downloads dependencies if needed) [DEFAULT]"
	@echo "  download-deps   - Download all missing build dependencies"
	@echo "  download-licenses - Download third-party license files to CMSIS/"
	@echo "  clean-deps      - Remove downloaded dependencies (CMSIS/, CMake _deps/)"
	@echo "  clean           - Remove build artifacts and CMake install prefixes"
	@echo "  test            - Build and run host tests (tests/, PC toolchain)"
	@echo "  test-f0         - ... against the STM32F0 backend (also -g0 / -f4)"
	@echo "  test-lowpower   - ... with -DOW_PORT_LOW_POWER=1 (also -f0/-g0/-f4)"
	@echo "  test-ndebug     - ... with NDEBUG (also -f0/-g0/-f4)"
	@echo "  test-active     - ... with the active-drive write path (also -f0/-g0/-f4)"
	@echo "  test-chips      - Check the chips/<part>.mk part matrix (no toolchain needed)"
	@echo "  test-clocks     - Check the per-family and per-part clock defaults (also -f4)"
	@echo "  debug           - Build with debug symbols"
	@echo "  fuzz-all        - Fuzz all targets (requires clang or gcc with sanitizers)"
	@echo "  fuzz-crc8       - Fuzz onewire_crc8 (60s)"
	@echo "  program         - Program device using ST-LINK"
	@echo "  jprogram        - Program device using J-LINK"
	@echo "  gccversion      - Show compiler version"
	@echo "  help            - Show this help"
	@echo "Variables:"
	@echo "  APP=1_basic|2_device_search|3_round_robin|4_scan_mode|5_commands|6_statistics|7_low_power  - example application to build"
	@echo "  OW_TARGET=f1|f0|f3|g0|f4|g4      - MCU family (firmware build)"
	@echo "  OW_CHIP=<part>                   - part within the family: the name of a"
	@echo "                                    chips/<part>.mk (f103xb, f030x6, f303xc, g031xx,"
	@echo "                                    f407xx, f401xc, f401xe, f446xx, g474cb); default per family"
	@echo "  SYSCLK_MHZ=N                     - system clock in MHz, overriding the part"
	@echo "  HSE_MHZ=N                        - crystal (HSE) in MHz, F3/F4/G4 only"
	@echo "                                    default (8/16 on every family, 84 on F401; the"
	@echo "                                    defaults are 72/48/72/64/168/170)"

# *** EOF ***
