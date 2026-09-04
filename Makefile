# Select the example application: demo (single sensor, Skip ROM),
# demo1 (device search + sequential polling of every sensor, one Convert T
#        per device - no broadcast conversion),
# demo2 (device search + sequential polling of every sensor on the bus),
# demo3 (device search + simultaneous broadcast conversion of every sensor),
# demo4 (device search + command transactions: ROM, power supply, TH/TL,
#        Copy/Recall EEPROM)
# demo5 (device search + sequential polling with signal statistics)
# demo6 (device search + sequential polling, WFE sleep on long stages)
#   make               -> builds demo   (ds18b20_demo.elf)
#   make APP=demo1     -> builds demo1  (ds18b20_demo1.elf)
#   make APP=demo2     -> builds demo2  (ds18b20_demo2.elf)
#   make APP=demo3     -> builds demo3  (ds18b20_demo3.elf)
#   make APP=demo4     -> builds demo4  (ds18b20_demo4.elf)
#   make APP=demo5     -> builds demo5  (ds18b20_demo5.elf)
#   make APP=demo6     -> builds demo6  (ds18b20_demo6.elf)
APP ?= demo
ifeq ($(filter $(APP),demo demo1 demo2 demo3 demo4 demo5 demo6),)
$(error APP must be 'demo', 'demo1', 'demo2', 'demo3', 'demo4', 'demo5' or 'demo6')
endif

# demo5 is the signal-statistics example: enable the optional stats module by
# default, shorten the inter-measurement pause to ~1ms, and widen the stats
# window to 5000 measurement rounds.  Parasite power is deliberately NOT set
# here (it is bus-hardware dependent) — pass EXT="-DPARASITE_POWER=1" when the
# 1-Wire bus is parasite-powered.
ifeq ($(APP),demo5)
override EXT += -DOW_STATS_ENABLE -DSTATS_DUMP_INTERVAL=5000 -DDS18B20_CYCLE_PAUSE_US=10000
endif

# Define the name of the project target and the build directory
TARGET = ds18b20_$(APP)
BUILD_DIR = build

# CMSIS directory structure for third-party build dependencies
CMSIS_CORE_DIR   = CMSIS/core
CMSIS_DEVICE_DIR = CMSIS/device

# Define the C source files, assembly source file, linker script, and preprocessor definitions
# OW_TARGET selects the MCU family: f1 (STM32F103xB, default), f0 (STM32F030x6)
# or g0 (STM32G031xx)
#   make                -> F1 firmware
#   make OW_TARGET=f0   -> F0 firmware
#   make OW_TARGET=g0   -> G0 firmware
ifeq ($(OW_TARGET),f0)
SRC = $(CMSIS_DEVICE_DIR)/system_stm32f0xx.c src/$(APP).c src/onewire.c src/ds18b20.c src/app.c src/ow_stats.c
ASM = $(CMSIS_DEVICE_DIR)/startup_stm32f030x6.s
LDS = port/stm32f0/STM32F030X6_FLASH.ld
MCU = -mcpu=cortex-m0 -mthumb
DEF = -DSTM32F030x6 -DOW_PORT_TARGET_F0
JFLASH = port/stm32f0/stm32f030f4.jflash
else ifeq ($(OW_TARGET),g0)
SRC = $(CMSIS_DEVICE_DIR)/system_stm32g0xx.c src/$(APP).c src/onewire.c src/ds18b20.c src/app.c src/ow_stats.c
ASM = $(CMSIS_DEVICE_DIR)/startup_stm32g031xx.s
LDS = port/stm32g0/STM32G031X6_FLASH.ld
MCU = -mcpu=cortex-m0plus -mthumb
DEF = -DSTM32G031xx -DOW_PORT_TARGET_G0
JFLASH = port/stm32g0/stm32g031f6.jflash
else
SRC = $(CMSIS_DEVICE_DIR)/system_stm32f1xx.c src/$(APP).c src/onewire.c src/ds18b20.c src/app.c src/ow_stats.c
ASM = $(CMSIS_DEVICE_DIR)/startup_stm32f103xb.s
LDS = port/stm32f1/STM32F103XB_FLASH.ld
MCU = -mcpu=cortex-m3 -mthumb
DEF = -DSTM32F103xB -DOW_PORT_TARGET_F1
JFLASH = port/stm32f1/stm32f103cb.jflash
endif
INC = -I. -Iinc -Iport/stm32f1 -Iport/stm32f0 -Iport/stm32g0 -I$(CMSIS_CORE_DIR) -I$(CMSIS_DEVICE_DIR)

# Per-app USART1 TX ring buffer size (power of two), overrides the app.h default
UART_TX_SIZE_demo  = 128
UART_TX_SIZE_demo1 = 256
UART_TX_SIZE_demo2 = 256
UART_TX_SIZE_demo3 = 256
UART_TX_SIZE_demo4 = 256
UART_TX_SIZE_demo5 = 1024
UART_TX_SIZE_demo6 = 256
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
# make OW_DRIVE_ACTIVE=1  →  -DOW_DRIVE_ACTIVE
# Pure-write transactions switch PA10 to push-pull so the master actively
# drives BOTH bus levels (faster, stronger write-1); read/reset phases stay
# open-drain. Experimental; kept off by default. See bus electrical-model doc.
ifeq ($(OW_DRIVE_ACTIVE),1)
DEF += -DOW_DRIVE_ACTIVE
endif

# Optional default timing profile:
# make TIMING=SLOW|FAST|STANDARD|ROBUST|CUSTOM  →  -DONEWIRE_TIMING_PROFILE_DEFAULT=ONEWIRE_TIMING_SLOW etc.
# Short alias for the railway-station EXT="-DONEWIRE_TIMING_PROFILE_DEFAULT=...".
ifdef TIMING
  ifeq ($(filter $(TIMING),FAST STANDARD SLOW ROBUST CUSTOM),)
    $(error TIMING must be FAST, STANDARD, SLOW, ROBUST or CUSTOM)
  endif
  DEF += -DONEWIRE_TIMING_PROFILE_DEFAULT=ONEWIRE_TIMING_$(TIMING)
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
SVD_URL_F1 = https://raw.githubusercontent.com/cmsis-svd/cmsis-svd-data/refs/heads/main/data/STMicro/STM32F103xx.svd
SVD_URL_F0 = https://raw.githubusercontent.com/cmsis-svd/cmsis-svd-data/refs/heads/main/data/STMicro/STM32F030.svd
SVD_URL_G0 = https://raw.githubusercontent.com/cmsis-svd/cmsis-svd-data/refs/heads/main/data/STMicro/STM32G031.svd

# Required external files (needed for build but not in repo)
ifeq ($(OW_TARGET),f0)
EXTERNAL_DEPS = $(CMSIS_CORE_DIR)/core_cm0.h \
                $(CMSIS_CORE_DIR)/cmsis_compiler.h \
                $(CMSIS_CORE_DIR)/cmsis_gcc.h \
                $(CMSIS_CORE_DIR)/cmsis_version.h \
                $(CMSIS_DEVICE_DIR)/stm32f0xx.h \
                $(CMSIS_DEVICE_DIR)/stm32f030x6.h \
                $(CMSIS_DEVICE_DIR)/system_stm32f0xx.h \
                $(CMSIS_DEVICE_DIR)/system_stm32f0xx.c \
                $(CMSIS_DEVICE_DIR)/startup_stm32f030x6.s \
                $(CMSIS_DEVICE_DIR)/STM32F030.svd
else ifeq ($(OW_TARGET),g0)
EXTERNAL_DEPS = $(CMSIS_CORE_DIR)/core_cm0plus.h \
                $(CMSIS_CORE_DIR)/mpu_armv7.h \
                $(CMSIS_CORE_DIR)/cmsis_compiler.h \
                $(CMSIS_CORE_DIR)/cmsis_gcc.h \
                $(CMSIS_CORE_DIR)/cmsis_version.h \
                $(CMSIS_DEVICE_DIR)/stm32g0xx.h \
                $(CMSIS_DEVICE_DIR)/stm32g031xx.h \
                $(CMSIS_DEVICE_DIR)/system_stm32g0xx.h \
                $(CMSIS_DEVICE_DIR)/system_stm32g0xx.c \
                $(CMSIS_DEVICE_DIR)/startup_stm32g031xx.s \
                $(CMSIS_DEVICE_DIR)/STM32G031.svd
else
EXTERNAL_DEPS = $(CMSIS_CORE_DIR)/core_cm3.h \
                $(CMSIS_CORE_DIR)/cmsis_compiler.h \
                $(CMSIS_CORE_DIR)/cmsis_gcc.h \
                $(CMSIS_CORE_DIR)/cmsis_version.h \
                $(CMSIS_DEVICE_DIR)/stm32f1xx.h \
                $(CMSIS_DEVICE_DIR)/stm32f103xb.h \
                $(CMSIS_DEVICE_DIR)/system_stm32f1xx.h \
                $(CMSIS_DEVICE_DIR)/system_stm32f1xx.c \
                $(CMSIS_DEVICE_DIR)/startup_stm32f103xb.s \
                $(CMSIS_DEVICE_DIR)/STM32F103xx.svd
endif

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

# SVD files (debug register views for Ozone / VSCode cortex-debug)
$(CMSIS_DEVICE_DIR)/STM32F103xx.svd: | $(CMSIS_DEVICE_DIR)
	$(call download_file,$(SVD_URL_F1),$@)

$(CMSIS_DEVICE_DIR)/STM32F030.svd: | $(CMSIS_DEVICE_DIR)
	$(call download_file,$(SVD_URL_F0),$@)

$(CMSIS_DEVICE_DIR)/STM32G031.svd: | $(CMSIS_DEVICE_DIR)
	$(call download_file,$(SVD_URL_G0),$@)

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
            $(TEST_DIR)/test_ow_stats.c \
             $(TEST_MOCK)/hw_model.c \
            $(TEST_MOCK)/ds18b20_test_spy.c \
            $(TEST_MOCK)/ds18b20_test_access.c \
            $(TEST_MOCK)/ow_stats_test_access.c \
            $(TEST_DIR)/test_harness_api.c \
            $(TEST_DIR)/test_app_uart.c
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
else
TEST_PORT_FLAG = -DOW_PORT_TARGET_F1
TEST_PORT_INC = -Iport/stm32f1
TEST_EXE = $(TEST_OUT)/ds18b20_test.exe
endif
TEST_FLAG = -DHOST_BUILD -DDS18B20_TEST_HARNESS -DOW_STATS_ENABLE $(TEST_PORT_FLAG) -Wall -Wextra -Wswitch-enum \
            -Wno-unused-parameter -Wno-pointer-to-int-cast -Wno-int-to-pointer-cast \
            $(if $(COVERAGE),--coverage,)
TEST_INC  = -Iinc $(TEST_PORT_INC) -I$(TEST_MOCK)

# Low-power variant: the same suite re-built with -DOW_PORT_LOW_POWER.
TEST_LP_FLAG = $(TEST_FLAG) -DOW_PORT_LOW_POWER
TEST_LP_EXE = $(TEST_OUT)/ds18b20_test_lowpower$(if $(filter f0,$(OW_TARGET)),_f0,$(if $(filter g0,$(OW_TARGET)),_g0,)).exe

.PHONY: test test-f0 test-g0
test: $(TEST_EXE)
	$(TEST_EXE)

test-f0:
	$(MAKE) OW_TARGET=f0 test

test-g0:
	$(MAKE) OW_TARGET=g0 test

# --- Opt-in low-power WFE path test build (-DOW_PORT_LOW_POWER) ---
# Compiles the SAME suite with the low-power path enabled so the
# __WFE()-related code (SEVONPEND, ow_long_pending, UIE) is exercised
# on the host. See tests/test/test_lowpower.c.
.PHONY: test-lowpower test-lowpower-f0 test-lowpower-g0
test-lowpower: $(TEST_LP_EXE)
	$(TEST_LP_EXE)

test-lowpower-f0:
	$(MAKE) OW_TARGET=f0 test-lowpower

test-lowpower-g0:
	$(MAKE) OW_TARGET=g0 test-lowpower

$(TEST_EXE): $(TEST_SRC) src/ds18b20.c src/onewire.c src/app.c Makefile | $(TEST_OUT)
	$(HOST_CC) $(TEST_FLAG) $(TEST_INC) $(TEST_SRC) src/app.c -o $@

$(TEST_LP_EXE): $(TEST_SRC) src/ds18b20.c src/onewire.c src/app.c tests/test/test_lowpower.c Makefile | $(TEST_OUT)
	$(HOST_CC) $(TEST_LP_FLAG) $(TEST_INC) $(TEST_SRC) tests/test/test_lowpower.c src/app.c -o $@

$(TEST_OUT):
	mkdir -p $@

# --- Active-drive (push-pull write) test build (experimental, -DOW_DRIVE_ACTIVE) ---
# Runs only the active-drive test set (the full suite's pin-regression assertions
# assume the pin is never toggled outside the parasite strong-pull-up path).
TEST_ACTIVE_SRC = \
    $(TEST_DIR)/test_main_active.c \
    $(TEST_DIR)/test_active_drive.c \
    $(TEST_MOCK)/hw_model.c \
    $(TEST_MOCK)/ds18b20_test_spy.c \
    $(TEST_MOCK)/ds18b20_test_access.c \
    $(TEST_MOCK)/ow_stats_test_access.c \
    src/app.c
TEST_ACTIVE_FLAG = $(TEST_FLAG) -DOW_DRIVE_ACTIVE
TEST_ACTIVE_EXE  = $(TEST_OUT)/ds18b20_test_active$(if $(filter f0,$(OW_TARGET)),_f0,$(if $(filter g0,$(OW_TARGET)),_g0,)).exe

.PHONY: test-active test-active-f0 test-active-g0
test-active: $(TEST_ACTIVE_EXE)
	$(TEST_ACTIVE_EXE)

test-active-f0:
	$(MAKE) OW_TARGET=f0 test-active

test-active-g0:
	$(MAKE) OW_TARGET=g0 test-active

$(TEST_ACTIVE_EXE): $(TEST_ACTIVE_SRC) src/ds18b20.c src/onewire.c Makefile | $(TEST_OUT)
	$(HOST_CC) $(TEST_ACTIVE_FLAG) $(TEST_INC) $(TEST_ACTIVE_SRC) -o $@

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
        fuzz-encode-byte fuzz-bit-from-pulse fuzz-timing \
        fuzz-stats fuzz-ds18b20-decode fuzz-all

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

fuzz-timing: | $(FUZZ_OUT)
	$(FUZZ_CC) $(FUZZ_CFLAGS) tests/fuzz/fuzz_timing.c src/onewire.c $(FUZZ_HW_MOCK) \
	    $(FUZZ_LDFLAGS) -o $(FUZZ_OUT)/fuzz_timing
	$(FUZZ_OUT)/fuzz_timing -max_total_time=$(FUZZ_TIME) -print_final_stats=1

# Tier 3: ow_stats (single-TU, #include)
fuzz-stats: | $(FUZZ_OUT)
	$(FUZZ_CC) $(FUZZ_CFLAGS) -Isrc -DOW_STATS_ENABLE \
	    tests/fuzz/fuzz_stats.c $(FUZZ_HW_MOCK) \
	    $(FUZZ_LDFLAGS) -o $(FUZZ_OUT)/fuzz_stats
	$(FUZZ_OUT)/fuzz_stats -max_total_time=$(FUZZ_TIME) -print_final_stats=1

# Tier 4: ds18b20 decode (single-TU via test_access)
fuzz-ds18b20-decode: | $(FUZZ_OUT)
	$(FUZZ_CC) $(FUZZ_CFLAGS) -Isrc -DDS18B20_TEST_HARNESS -DOW_STATS_ENABLE \
	    tests/fuzz/fuzz_ds18b20_decode.c src/ow_stats.c \
	    $(FUZZ_HW_MOCK) \
	    $(FUZZ_LDFLAGS) -o $(FUZZ_OUT)/fuzz_ds18b20_decode
	$(FUZZ_OUT)/fuzz_ds18b20_decode -max_total_time=$(FUZZ_TIME) -print_final_stats=1

fuzz-all: fuzz-crc8 fuzz-decode-pulses fuzz-present fuzz-pair-bits \
          fuzz-encode-byte fuzz-bit-from-pulse fuzz-timing \
          fuzz-stats fuzz-ds18b20-decode

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
	@echo "  APP=demo|demo1|demo2|demo3|demo4|demo5|demo6  - example application to build"
	@echo "  OW_TARGET=f1|f0|g0               - MCU family (firmware build)"
	@echo "  SYSCLK_MHZ=N                     - run on the raw internal RC (8MHz F1/F0, 16MHz G0) instead of family default"

# *** EOF ***
