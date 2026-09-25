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
override EXT += -DOW_STATS_ENABLE=1 -DSTATS_DUMP_INTERVAL=5000
endif

# Define the name of the project target and the build directory
TARGET = ds18b20_$(APP)
BUILD_DIR = build

# CMSIS directory structure for third-party build dependencies
CMSIS_CORE_DIR   = CMSIS/core
CMSIS_DEVICE_DIR = CMSIS/device

# Define the C source files, assembly source file, linker script, and preprocessor definitions
# OW_TARGET selects the MCU family: f1 (STM32F103xB, default), f0 (STM32F030x6),
# g0 (STM32G031xx) or f4 (STM32F407xx / STM32F401 family).
#   make                -> F1 firmware
#   make OW_TARGET=f0   -> F0 firmware
#   make OW_TARGET=g0   -> G0 firmware
#   make OW_TARGET=f4   -> F4 firmware (default part f407xx)
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
OW_KNOWN_TARGETS = f1 f0 g0 f4
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
INC = -I. -Iinc -Iexamples/app -Iport/stm32f1 -Iport/stm32f0 -Iport/stm32g0 -Iport/stm32f4 -I$(CMSIS_CORE_DIR) -I$(CMSIS_DEVICE_DIR)

# Per-app USART1 TX ring buffer size (power of two), overrides the app.h default
UART_TX_SIZE_1_basic        = 128
UART_TX_SIZE_2_device_search = 256
UART_TX_SIZE_3_round_robin  = 256
UART_TX_SIZE_4_scan_mode    = 256
UART_TX_SIZE_5_commands     = 256
UART_TX_SIZE_6_statistics   = 1024
UART_TX_SIZE_7_low_power    = 256
DEF += -DUART_TX_BUF_SIZE=$(UART_TX_SIZE_$(APP))

# Optional system clock override:
# make SYSCLK_MHZ=16  →  -DOW_PORT_SYSCLK_MHZ=16
# (run on the raw internal RC instead of the family default:
#  STM32F103 = 72MHz HSE+PLL x9, STM32F030 = 48MHz HSI/2+PLL x12,
#  STM32G031 = 64MHz HSI16+PLL; e.g. SYSCLK_MHZ=16 for the raw 16MHz HSI16)
ifdef SYSCLK_MHZ
DEF += -DOW_PORT_SYSCLK_MHZ=$(SYSCLK_MHZ)
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

# Optimization flags for the compiler:
# -Os         : Optimize for code size. This driver is polled on a millisecond
#               cadence, so compact code matters more than raw speed. Saves
#               ~65% flash vs the old -O3 + --param max-inline-insns-auto=480
#               (which ballooned main() to ~9 KB by forcing massive inlining).
# -flto       : Link Time Optimization - cross-file optimization during linking
# -g0         : No debug information (reduces binary size, incompatible with debugging)

OPT = -Os -flto -g0

# Cortex-M0 / Cortex-M0+ (F0 / G0) trip a GCC 14 LTO link failure
# ("invalid constant after fixup" in the thin-LTO partitioner) when the code
# shape shifts; drop LTO there. Correctness is unaffected, binaries are just
# slightly larger. Cortex-M3 (F1) keeps LTO.
ifeq ($(OW_TARGET),f0)
OPT := $(filter-out -flto,$(OPT))
endif
ifeq ($(OW_TARGET),g0)
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
FLAG += -MMD -MP -MF $(@:%.o=%.d)

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
SVD_URL_F1 = https://raw.githubusercontent.com/cmsis-svd/cmsis-svd-data/refs/heads/main/data/STMicro/STM32F103xx.svd
SVD_URL_F0 = https://raw.githubusercontent.com/cmsis-svd/cmsis-svd-data/refs/heads/main/data/STMicro/STM32F030.svd
SVD_URL_G0 = https://raw.githubusercontent.com/cmsis-svd/cmsis-svd-data/refs/heads/main/data/STMicro/STM32G031.svd
SVD_URL_F4 = https://raw.githubusercontent.com/cmsis-svd/cmsis-svd-data/refs/heads/main/data/STMicro/STM32F407.svd
SVD_URL_F401 = https://raw.githubusercontent.com/cmsis-svd/cmsis-svd-data/refs/heads/main/data/STMicro/STM32F401.svd

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
CMSIS_CORE_HEADERS = $(CMSIS_CORE_DIR)/core_cm4.h
CMSIS_DEVICE_FAMILY_HDR = stm32f4xx.h
CMSIS_SYSTEM_HDR = system_stm32f4xx.h
CMSIS_SYSTEM_SRC = system_stm32f4xx.c
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

# ARM CMSIS Core headers (Apache 2.0)
$(CMSIS_CORE_DIR)/core_cm3.h: | $(CMSIS_CORE_DIR)
	$(call download_file,$(CMSIS_CORE_URL)/core_cm3.h,$@)

$(CMSIS_CORE_DIR)/core_cm0.h: | $(CMSIS_CORE_DIR)
	$(call download_file,$(CMSIS_CORE_URL)/core_cm0.h,$@)

$(CMSIS_CORE_DIR)/core_cm0plus.h: | $(CMSIS_CORE_DIR)
	$(call download_file,$(CMSIS_CORE_URL)/core_cm0plus.h,$@)

$(CMSIS_CORE_DIR)/mpu_armv7.h: | $(CMSIS_CORE_DIR)
	$(call download_file,$(CMSIS_CORE_URL)/mpu_armv7.h,$@)

$(CMSIS_CORE_DIR)/cmsis_compiler.h: | $(CMSIS_CORE_DIR)
	$(call download_file,$(CMSIS_CORE_URL)/cmsis_compiler.h,$@)

$(CMSIS_CORE_DIR)/cmsis_gcc.h: | $(CMSIS_CORE_DIR)
	$(call download_file,$(CMSIS_CORE_URL)/cmsis_gcc.h,$@)

$(CMSIS_CORE_DIR)/cmsis_version.h: | $(CMSIS_CORE_DIR)
	$(call download_file,$(CMSIS_CORE_URL)/cmsis_version.h,$@)

$(CMSIS_CORE_DIR)/core_cm4.h: | $(CMSIS_CORE_DIR)
	$(call download_file,$(CMSIS_CORE_URL)/core_cm4.h,$@)

# cmsis_device_f1 headers and sources (Apache 2.0)
$(CMSIS_DEVICE_DIR)/stm32f1xx.h: | $(CMSIS_DEVICE_DIR)
	$(call download_file,$(F1_URL)/Include/stm32f1xx.h,$@)

$(CMSIS_DEVICE_DIR)/stm32f103xb.h: | $(CMSIS_DEVICE_DIR)
	$(call download_file,$(F1_URL)/Include/stm32f103xb.h,$@)

$(CMSIS_DEVICE_DIR)/system_stm32f1xx.h: | $(CMSIS_DEVICE_DIR)
	$(call download_file,$(F1_URL)/Include/system_stm32f1xx.h,$@)

# cmsis_device_f1 sources (Apache 2.0)
$(CMSIS_DEVICE_DIR)/system_stm32f1xx.c: | $(CMSIS_DEVICE_DIR)
	$(call download_file,$(F1_URL)/Source/Templates/system_stm32f1xx.c,$@)

$(CMSIS_DEVICE_DIR)/startup_stm32f103xb.s: | $(CMSIS_DEVICE_DIR)
	$(call download_file,$(F1_URL)/Source/Templates/gcc/startup_stm32f103xb.s,$@)

# cmsis_device_f0 headers and sources (Apache 2.0)
$(CMSIS_DEVICE_DIR)/stm32f0xx.h: | $(CMSIS_DEVICE_DIR)
	$(call download_file,$(F0_URL)/Include/stm32f0xx.h,$@)

$(CMSIS_DEVICE_DIR)/stm32f030x6.h: | $(CMSIS_DEVICE_DIR)
	$(call download_file,$(F0_URL)/Include/stm32f030x6.h,$@)

$(CMSIS_DEVICE_DIR)/system_stm32f0xx.h: | $(CMSIS_DEVICE_DIR)
	$(call download_file,$(F0_URL)/Include/system_stm32f0xx.h,$@)

$(CMSIS_DEVICE_DIR)/system_stm32f0xx.c: | $(CMSIS_DEVICE_DIR)
	$(call download_file,$(F0_URL)/Source/Templates/system_stm32f0xx.c,$@)

$(CMSIS_DEVICE_DIR)/startup_stm32f030x6.s: | $(CMSIS_DEVICE_DIR)
	$(call download_file,$(F0_URL)/Source/Templates/gcc/startup_stm32f030x6.s,$@)

# cmsis_device_g0 headers and sources (Apache 2.0)
$(CMSIS_DEVICE_DIR)/stm32g0xx.h: | $(CMSIS_DEVICE_DIR)
	$(call download_file,$(G0_URL)/Include/stm32g0xx.h,$@)

$(CMSIS_DEVICE_DIR)/stm32g031xx.h: | $(CMSIS_DEVICE_DIR)
	$(call download_file,$(G0_URL)/Include/stm32g031xx.h,$@)

$(CMSIS_DEVICE_DIR)/system_stm32g0xx.h: | $(CMSIS_DEVICE_DIR)
	$(call download_file,$(G0_URL)/Include/system_stm32g0xx.h,$@)

$(CMSIS_DEVICE_DIR)/system_stm32g0xx.c: | $(CMSIS_DEVICE_DIR)
	$(call download_file,$(G0_URL)/Source/Templates/system_stm32g0xx.c,$@)

$(CMSIS_DEVICE_DIR)/startup_stm32g031xx.s: | $(CMSIS_DEVICE_DIR)
	$(call download_file,$(G0_URL)/Source/Templates/gcc/startup_stm32g031xx.s,$@)

# cmsis_device_f4 headers and sources (Apache 2.0)
$(CMSIS_DEVICE_DIR)/stm32f4xx.h: | $(CMSIS_DEVICE_DIR)
	$(call download_file,$(F4_URL)/Include/stm32f4xx.h,$@)

$(CMSIS_DEVICE_DIR)/stm32f407xx.h: | $(CMSIS_DEVICE_DIR)
	$(call download_file,$(F4_URL)/Include/stm32f407xx.h,$@)

$(CMSIS_DEVICE_DIR)/stm32f401xc.h: | $(CMSIS_DEVICE_DIR)
	$(call download_file,$(F4_URL)/Include/stm32f401xc.h,$@)

$(CMSIS_DEVICE_DIR)/stm32f401xe.h: | $(CMSIS_DEVICE_DIR)
	$(call download_file,$(F4_URL)/Include/stm32f401xe.h,$@)

$(CMSIS_DEVICE_DIR)/system_stm32f4xx.h: | $(CMSIS_DEVICE_DIR)
	$(call download_file,$(F4_URL)/Include/system_stm32f4xx.h,$@)

$(CMSIS_DEVICE_DIR)/system_stm32f4xx.c: | $(CMSIS_DEVICE_DIR)
	$(call download_file,$(F4_URL)/Source/Templates/system_stm32f4xx.c,$@)

$(CMSIS_DEVICE_DIR)/startup_stm32f407xx.s: | $(CMSIS_DEVICE_DIR)
	$(call download_file,$(F4_URL)/Source/Templates/gcc/startup_stm32f407xx.s,$@)

$(CMSIS_DEVICE_DIR)/startup_stm32f401xc.s: | $(CMSIS_DEVICE_DIR)
	$(call download_file,$(F4_URL)/Source/Templates/gcc/startup_stm32f401xc.s,$@)

$(CMSIS_DEVICE_DIR)/startup_stm32f401xe.s: | $(CMSIS_DEVICE_DIR)
	$(call download_file,$(F4_URL)/Source/Templates/gcc/startup_stm32f401xe.s,$@)

# SVD files (debug register views for Ozone / VSCode cortex-debug)
$(CMSIS_DEVICE_DIR)/STM32F103xx.svd: | $(CMSIS_DEVICE_DIR)
	$(call download_file,$(SVD_URL_F1),$@)

$(CMSIS_DEVICE_DIR)/STM32F030.svd: | $(CMSIS_DEVICE_DIR)
	$(call download_file,$(SVD_URL_F0),$@)

$(CMSIS_DEVICE_DIR)/STM32G031.svd: | $(CMSIS_DEVICE_DIR)
	$(call download_file,$(SVD_URL_G0),$@)

$(CMSIS_DEVICE_DIR)/STM32F407.svd: | $(CMSIS_DEVICE_DIR)
	$(call download_file,$(SVD_URL_F4),$@)

$(CMSIS_DEVICE_DIR)/STM32F401.svd: | $(CMSIS_DEVICE_DIR)
	$(call download_file,$(SVD_URL_F401),$@)

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

# Clean external dependencies
clean-deps:
	rm -rf $(CMSIS_CORE_DIR) $(CMSIS_DEVICE_DIR)

# =============================================================================
# PART MATRIX CHECKS (chips/*.mk)
# =============================================================================
# Each part file names three files that live in this repository (linker script,
# J-Flash project, Ozone project) plus a CMSIS device macro and a clock. A typo
# in any of them otherwise surfaces only at link time, or - worse - as a valid
# binary for the wrong part. test-chips verifies the repo-side files exist and
# the two scalars are well formed, for every part; test-chip-rejects verifies
# that an unknown family or part is rejected instead of silently falling back.

CHIP_PARTS = $(patsubst chips/%.mk,%,$(wildcard chips/*.mk))

# Include the part under inspection so check-chip-one can read its variables.
# No-op in a normal build, where CHECK_CHIP is empty. Written through a define
# because ifeq/endif are line-oriented and $(eval) needs real newlines.
define CHIP_CHECK_INCLUDE
ifeq ($$(CHECK_CHIP),$(1))
include chips/$(1).mk
endif
endef
$(foreach p,$(CHIP_PARTS),$(eval $(call CHIP_CHECK_INCLUDE,$(p))))

.PHONY: test-chips
test-chips: test-chip-files test-chip-rejects
	@echo "test-chips: OK ($(words $(CHIP_PARTS)) parts: $(CHIP_PARTS))"

.PHONY: test-chip-files
test-chip-files:
	@fail=0; \
	for p in $(CHIP_PARTS); do \
	  $(MAKE) --no-print-directory CHECK_CHIP=$$p check-chip-one || fail=1; \
	done; \
	if [ $$fail -ne 0 ]; then echo "test-chip-files: FAILED"; exit 1; fi

# Runs with exactly one part included; the recipe reports on that part alone.
.PHONY: check-chip-one
check-chip-one:
	@missing=""; \
	for f in "$(CHIP_LINKER)" "$(CHIP_JFLASH)" "$(CHIP_JDEBUG)"; do \
	  if [ ! -f "$$f" ]; then missing="$$missing $$f"; fi; \
	done; \
	if [ -n "$$missing" ]; then \
	  echo "  $(CHECK_CHIP): MISSING$$missing"; exit 1; \
	fi; \
	case "$(CHIP_DEV_DEF)" in \
	  -D*) ;; \
	  *) echo "  $(CHECK_CHIP): bad CHIP_DEV_DEF '$(CHIP_DEV_DEF)'"; exit 1;; \
	esac; \
	case "$(CHIP_SYSCLK_MHZ)" in \
	  ''|*[!0-9]*) echo "  $(CHECK_CHIP): bad CHIP_SYSCLK_MHZ '$(CHIP_SYSCLK_MHZ)'"; exit 1;; \
	esac; \
	echo "  $(CHECK_CHIP): ok"

# An unknown token must fail with a message about the token, not about some
# unrelated prerequisite - otherwise these tests would pass for the wrong
# reason (e.g. an empty APP tripping the APP validation first).
.PHONY: test-chip-rejects
test-chip-rejects:
	@fail=0; \
	check_rejects() { \
	  msg=$$($(MAKE) --no-print-directory APP=1_basic $$1 2>&1 >/dev/null); \
	  case "$$msg" in \
	    *"$$2"*) echo "  rejected $$1";; \
	    *) echo "  FAIL: $$1 was not rejected with '$$2'"; fail=1;; \
	  esac; \
	}; \
	check_rejects OW_TARGET=f401-84 "OW_TARGET='f401-84' is not a known family"; \
	check_rejects OW_TARGET=f9 "is not a known family"; \
	check_rejects "OW_TARGET=f4 OW_CHIP=f401" "OW_CHIP='f401' has no chips/f401.mk"; \
	check_rejects "OW_TARGET=f4 OW_CHIP=f999" "OW_CHIP='f999' has no chips/f999.mk"; \
	if [ $$fail -ne 0 ]; then echo "test-chip-rejects: FAILED"; exit 1; fi

# =============================================================================
# BUILD TARGETS
# =============================================================================

# Set 'all' as the default target
.DEFAULT_GOAL := all

# Build all targets by default: the ELF binary, the HEX file, and the raw binary file
all: download-deps $(BUILD_DIR)/$(TARGET).elf $(BUILD_DIR)/$(TARGET).hex $(BUILD_DIR)/$(TARGET).bin

# Define the object files that need to be built from C and assembly source files.
# Every object is prefixed with the app name (e.g. build/demo_app.o), because
# the compile flags differ per app (-DUART_TX_BUF_SIZE) and shared objects like
# app.o would otherwise be reused stale across `make APP=...` invocations.
OBJ = $(addprefix $(BUILD_DIR)/$(APP)_,$(notdir $(SRC:.c=.o)))
vpath %.c $(sort $(dir $(SRC))) # Set the search path for C source files

OBJ += $(addprefix $(BUILD_DIR)/$(APP)_,$(notdir $(ASM:.s=.o)))
vpath %.s $(sort $(dir $(ASM))) # Set the search path for assembly source files

# Specify how to compile a C source file into an object file
$(BUILD_DIR)/$(APP)_%.o: %.c Makefile | $(BUILD_DIR)
	$(CC) -c $(FLAG) $(OPT) $(EXT) $< -o $@

# Specify how to compile an assembly source file into an object file
$(BUILD_DIR)/$(APP)_%.o: %.s Makefile | $(BUILD_DIR)
	$(AS) -c $(FLAG) $(OPT) $(EXT) -Wa,-a,-ad,-alms=$(BUILD_DIR)/$(APP)_$(notdir $(<:.s=.lst)) $< -o $@

# Specify how to build the final executable file
$(BUILD_DIR)/$(TARGET).elf: $(OBJ) Makefile
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
debug: OPT = -Og -g3 -gdwarf
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

# Clean the build directory by removing all object files, dependency files, binaries, and map files
.PHONY: clean
clean:
	rm -fR $(BUILD_DIR)

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
else
TEST_PORT_FLAG = -DOW_PORT_TARGET_F1
TEST_PORT_INC = -Iport/stm32f1
TEST_EXE = $(TEST_OUT)/ds18b20_test.exe
endif
TEST_FLAG = -DHOST_BUILD -DDS18B20_TEST_HARNESS -DOW_STATS_ENABLE=1 $(TEST_PORT_FLAG) -Wall -Wextra -Wswitch-enum \
            -Werror=discarded-qualifiers \
            -Wno-unused-parameter -Wno-pointer-to-int-cast -Wno-int-to-pointer-cast \
            $(if $(COVERAGE),--coverage,)
TEST_INC  = -Iinc -Iexamples/app $(TEST_PORT_INC) -I$(TEST_MOCK)

# Low-power variant: the same suite re-built with -DOW_PORT_LOW_POWER=1.
TEST_LP_FLAG = $(TEST_FLAG) -DOW_PORT_LOW_POWER=1
TEST_LP_EXE = $(TEST_OUT)/ds18b20_test_lowpower$(if $(filter f0,$(OW_TARGET)),_f0,$(if $(filter g0,$(OW_TARGET)),_g0,$(if $(filter f4,$(OW_TARGET)),_f4,))).exe

.PHONY: test test-f0 test-g0 test-f4
TEST_CLOCK_FLAG = $(if $(filter f0,$(1)),STM32F0,$(if $(filter g0,$(1)),STM32G0,$(if $(filter f4,$(1)),STM32F4,STM32F1)))
TEST_CLOCK_OBJ = $(TEST_OUT)/test_sysclk_fallback$(if $(filter f0,$(OW_TARGET)),_f0,$(if $(filter g0,$(OW_TARGET)),_g0,$(if $(filter f4,$(OW_TARGET)),_f4,_f1))).o
# F4/F401-family fallback compile check: always built as part of `make test`,
# independent of the active OW_TARGET (see rule below test-clocks).
TEST_CLOCK_F401_OBJ = $(TEST_OUT)/test_sysclk_fallback_f401.o

test: $(TEST_EXE) $(TEST_CLOCK_OBJ) $(TEST_CLOCK_F401_OBJ)
	$(TEST_EXE)

test-f0:
	$(MAKE) OW_TARGET=f0 test

test-g0:
	$(MAKE) OW_TARGET=g0 test

test-f4:
	$(MAKE) OW_TARGET=f4 test

# --- Family-macro fallback compile check (see test_sysclk_fallback.c) ---
# Compile-only: verifies that selecting a family through the raw family macro
# (STM32F1/F0/G0, the PlatformIO/STM32CubeMX path) resolves both
# OW_PORT_FAMILY_* and the OW_PORT_SYSCLK_MHZ default for the backend under
# test. Runs as part of every test build so the two can never drift.

$(TEST_CLOCK_OBJ): tests/test/test_sysclk_fallback.c Makefile | $(TEST_OUT)
	$(HOST_CC) -c -D$(call TEST_CLOCK_FLAG,$(OW_TARGET)) $(TEST_INC) tests/test/test_sysclk_fallback.c -o $@

# --- F4/F401 fallback compile check (see test_sysclk_fallback.c) ---
# Compile-only, built as part of every `make test` (dependency of the `test`
# target above): verifies that selecting the F4 family together with the
# STM32F401xC/F401xE device macro (the OW_CHIP=f401 build, or a bare
# PlatformIO/CubeMX F401 project) resolves the F4 backend with an 84 MHz clock
# default instead of the F405/F407 168 MHz one. Always built with the F4 port
# include path, independent of the active OW_TARGET. Also part of
# `test-clocks-f401`.

$(TEST_CLOCK_F401_OBJ): tests/test/test_sysclk_fallback.c Makefile | $(TEST_OUT)
	$(HOST_CC) -c -DSTM32F4 -DSTM32F401xC -Iinc -Iexamples/app -Iport/stm32f4 -I$(TEST_MOCK) \
	    tests/test/test_sysclk_fallback.c -o $@

.PHONY: test-clocks test-chips test-chip-files test-chip-rejects check-chip-one test-clocks-f1 test-clocks-f0 test-clocks-g0 test-clocks-f4 test-clocks-f401
test-clocks: test-chips test-clocks-f1 test-clocks-f0 test-clocks-g0 test-clocks-f4 test-clocks-f401
test-clocks-f1:
	$(MAKE) OW_TARGET=f1 $(TEST_OUT)/test_sysclk_fallback_f1.o
test-clocks-f0:
	$(MAKE) OW_TARGET=f0 $(TEST_OUT)/test_sysclk_fallback_f0.o
test-clocks-g0:
	$(MAKE) OW_TARGET=g0 $(TEST_OUT)/test_sysclk_fallback_g0.o
test-clocks-f4:
	$(MAKE) OW_TARGET=f4 $(TEST_OUT)/test_sysclk_fallback_f4.o
test-clocks-f401:
	$(MAKE) OW_TARGET=f4 $(TEST_OUT)/test_sysclk_fallback_f401.o

# --- Opt-in low-power WFE path test build (-DOW_PORT_LOW_POWER=1) ---
# Compiles the SAME suite with the low-power path enabled so the
# __WFE()-related code (SEVONPEND, ow_long_pending, UIE) is exercised
# on the host. See tests/test/test_lowpower.c.
.PHONY: test-lowpower test-lowpower-f0 test-lowpower-g0 test-lowpower-f4
test-lowpower: $(TEST_LP_EXE)
	$(TEST_LP_EXE)

test-lowpower-f0:
	$(MAKE) OW_TARGET=f0 test-lowpower

test-lowpower-g0:
	$(MAKE) OW_TARGET=g0 test-lowpower

test-lowpower-f4:
	$(MAKE) OW_TARGET=f4 test-lowpower

# src/ds18b20.c is an amalgamated translation unit: the search / txn /
# resolution / measurement code lives in these include-only parts (guarded by
# DS18B20_DRIVER_BUILD). They are listed here as prerequisites because the
# test executables compile the sources directly rather than through per-object
# dependency files.
DS18B20_PARTS = src/ds18b20_resolution.c src/ds18b20_txn.c \
                src/ds18b20_search.c src/ds18b20_measure.c

$(TEST_EXE): $(TEST_SRC) src/ds18b20.c $(DS18B20_PARTS) src/onewire.c examples/app/app.c Makefile | $(TEST_OUT)
	$(HOST_CC) $(TEST_FLAG) $(TEST_INC) $(TEST_OPT) $(TEST_SRC) examples/app/app.c -o $@

$(TEST_LP_EXE): $(TEST_SRC) src/ds18b20.c $(DS18B20_PARTS) src/onewire.c examples/app/app.c tests/test/test_lowpower.c Makefile | $(TEST_OUT)
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
TEST_ACTIVE_EXE  = $(TEST_OUT)/ds18b20_test_active$(if $(filter f0,$(OW_TARGET)),_f0,$(if $(filter g0,$(OW_TARGET)),_g0,$(if $(filter f4,$(OW_TARGET)),_f4,))).exe

.PHONY: test-active test-active-f0 test-active-g0 test-active-f4
test-active: $(TEST_ACTIVE_EXE)
	$(TEST_ACTIVE_EXE)

test-active-f0:
	$(MAKE) OW_TARGET=f0 test-active

test-active-g0:
	$(MAKE) OW_TARGET=g0 test-active

test-active-f4:
	$(MAKE) OW_TARGET=f4 test-active

$(TEST_ACTIVE_EXE): $(TEST_ACTIVE_SRC) src/ds18b20.c $(DS18B20_PARTS) src/onewire.c Makefile | $(TEST_OUT)
	$(HOST_CC) $(TEST_ACTIVE_FLAG) $(TEST_INC) $(TEST_OPT) $(TEST_ACTIVE_SRC) -o $@

# --- Release-semantics build (-DNDEBUG + OW_TEST_PARAM_GUARD) ---
# Rebuilds the same suite with the public API asserts compiled out
# (-DNDEBUG), so the onewire_write_slots/read_data reject paths become
# observable as return codes instead of aborting the process. Backend
# reject paths run fail-soft in every test variant.
# See tests/test/test_param_guard.c and test_rcr_limits.c.
TEST_NG_FLAG = $(TEST_FLAG) -DNDEBUG -DOW_TEST_PARAM_GUARD
TEST_NG_SRC  = $(TEST_SRC) $(TEST_DIR)/test_param_guard.c
TEST_NG_EXE  = $(TEST_OUT)/ds18b20_test_ndebug$(if $(filter f0,$(OW_TARGET)),_f0,$(if $(filter g0,$(OW_TARGET)),_g0,$(if $(filter f4,$(OW_TARGET)),_f4,))).exe

.PHONY: test-ndebug test-ndebug-f0 test-ndebug-g0 test-ndebug-f4
test-ndebug: $(TEST_NG_EXE)
	$(TEST_NG_EXE)

test-ndebug-f0:
	$(MAKE) OW_TARGET=f0 test-ndebug

test-ndebug-g0:
	$(MAKE) OW_TARGET=g0 test-ndebug

test-ndebug-f4:
	$(MAKE) OW_TARGET=f4 test-ndebug

$(TEST_NG_EXE): $(TEST_NG_SRC) src/ds18b20.c $(DS18B20_PARTS) src/onewire.c examples/app/app.c Makefile | $(TEST_OUT)
	$(HOST_CC) $(TEST_NG_FLAG) $(TEST_INC) $(TEST_OPT) $(TEST_NG_SRC) examples/app/app.c -o $@

# Include the dependency files generated during compilation
-include $(wildcard $(BUILD_DIR)/*.d)

# =============================================================================
# FUZZ TARGETS (requires Clang with libFuzzer / SanitizerCoverage,
#              or GCC with AddressSanitizer + UndefinedBehaviorSanitizer)
# =============================================================================

FUZZ_CC      ?= clang
FUZZ_CFLAGS  = -fsanitize=fuzzer,address,undefined -g -O1 \
               -DFUZZING_BUILD_MODE_UNSAFE_FOR_PRODUCTION \
               -DHOST_BUILD -DOW_PORT_TARGET_F1 -Iinc -Iport/stm32f1 -Itests/mock \
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
	@echo "  clean-deps      - Remove downloaded dependencies and CMSIS/ directories"
	@echo "  clean           - Remove build artifacts"
	@echo "  test            - Build and run host tests (tests/, PC toolchain)"
	@echo "  test-f0         - Build and run host tests against the STM32F0 backend"
	@echo "  test-g0         - Build and run host tests against the STM32G0 backend"
	@echo "  debug           - Build with debug symbols"
	@echo "  fuzz-all        - Fuzz all targets (requires clang or gcc with sanitizers)"
	@echo "  fuzz-crc8       - Fuzz onewire_crc8 (60s)"
	@echo "  program         - Program device using ST-LINK"
	@echo "  jprogram        - Program device using J-LINK"
	@echo "  gccversion      - Show compiler version"
	@echo "  help            - Show this help"
	@echo "Variables:"
	@echo "  APP=1_basic|2_device_search|3_round_robin|4_scan_mode|5_commands|6_statistics|7_low_power  - example application to build"
	@echo "  OW_TARGET=f1|f0|g0               - MCU family (firmware build)"
	@echo "  SYSCLK_MHZ=N                     - run on the raw internal RC (8MHz F1/F0, 16MHz G0) instead of family default"

# *** EOF ***
