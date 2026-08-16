# Select the example application: demo (single sensor, Skip ROM),
# demo2 (device search + sequential polling of every sensor on the bus),
# demo3 (device search + simultaneous broadcast conversion of every sensor)
# or diag (standalone bit-bang 1-Wire diagnostics of a single sensor)
#   make               -> builds demo   (ds18b20_demo.elf)
#   make APP=demo2     -> builds demo2  (ds18b20_demo2.elf)
#   make APP=demo3     -> builds demo3  (ds18b20_demo3.elf)
#   make APP=diag      -> builds diag   (ds18b20_diag.elf)
APP ?= demo
ifeq ($(filter $(APP),demo demo2 demo3 diag),)
$(error APP must be 'demo', 'demo2', 'demo3' or 'diag')
endif

# Define the name of the project target and the build directory
TARGET = ds18b20_$(APP)
BUILD_DIR = build

# CMSIS directory structure for third-party build dependencies
CMSIS_CORE_DIR   = CMSIS/core
CMSIS_DEVICE_DIR = CMSIS/device

# Define the C source files, assembly source file, linker script, and preprocessor definitions
# The diag app is a standalone bit-bang tool and does not link the driver.
ifneq ($(filter $(APP),diag),)
SRC = $(CMSIS_DEVICE_DIR)/system_stm32f1xx.c src/$(APP).c src/app.c
else
SRC = $(CMSIS_DEVICE_DIR)/system_stm32f1xx.c src/$(APP).c src/onewire.c src/ds18b20.c src/app.c
endif
ASM = $(CMSIS_DEVICE_DIR)/startup_stm32f103xb.s
LDS = STM32F103XB_FLASH.ld
MCU = -mcpu=cortex-m3 -mthumb
DEF = -DSTM32F103xB
INC = -I. -Iinc -I$(CMSIS_CORE_DIR) -I$(CMSIS_DEVICE_DIR)

# Per-app USART1 TX ring buffer size (power of two), overrides the app.h default
UART_TX_SIZE_demo  = 128
UART_TX_SIZE_demo2 = 256
UART_TX_SIZE_demo3 = 256
UART_TX_SIZE_diag  = 256
DEF += -DUART_TX_BUF_SIZE=$(UART_TX_SIZE_$(APP))

# Define additional preprocessor definitions based on conditional variables
# make HSI_8MHZ=1  →  -DHSI_8MHZ=1  (run on HSI 8MHz instead of HSE+PLL 72MHz)
USE := HSI_8MHZ
DEF += $(strip $(foreach def, $(USE), $(if $($(def)), -D$(def)=$($(def)))))

# Optimization flags for the compiler:
# -Os         : Optimize for code size. This driver is polled on a millisecond
#               cadence, so compact code matters more than raw speed. Saves
#               ~65% flash vs the old -O3 + --param max-inline-insns-auto=480
#               (which ballooned main() to ~9 KB by forcing massive inlining).
# -flto       : Link Time Optimization - cross-file optimization during linking
# -g0         : No debug information (reduces binary size, incompatible with debugging)

OPT = -Os -flto -g0

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

JLINK_FLAGS = -openprj./stm32f103cb.jflash -open$(BUILD_DIR)/$(TARGET).hex -hide -auto -exit -jflashlog./jflash.log

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
SVD_URL = https://raw.githubusercontent.com/cmsis-svd/cmsis-svd-data/refs/heads/main/data/STMicro/STM32F103xx.svd

# Required external files (needed for build but not in repo)
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

# License files
CMSIS_CORE_LICENSE_URL = https://raw.githubusercontent.com/ARM-software/CMSIS_5/master/LICENSE.txt
DEVICE_F1_LICENSE_URL = https://raw.githubusercontent.com/STMicroelectronics/cmsis_device_f1/master/License.md

CMSIS_CORE_LICENSE = $(CMSIS_CORE_DIR)/LICENSE.txt
CMSIS_DEVICE_LICENSE = $(CMSIS_DEVICE_DIR)/LICENSE

LICENSE_FILES = $(CMSIS_CORE_LICENSE) $(CMSIS_DEVICE_LICENSE)

# Download function using wget or curl
define download_file
	@echo "  Downloading $(1)..."
	@if [ -z "$(DOWNLOAD_TOOL)" ]; then \
		echo "Error: neither wget nor curl found. Please install one of them."; \
		exit 1; \
	fi
	@$(DOWNLOAD_TOOL) $(DOWNLOAD_FLAGS) "$(2)" "$(1)" && echo "    OK" || (echo "    FAILED"; exit 1)
endef

# Create CMSIS directories
$(CMSIS_CORE_DIR):
	mkdir -p $@

$(CMSIS_DEVICE_DIR):
	mkdir -p $@

# ARM CMSIS Core headers (Apache 2.0)
$(CMSIS_CORE_DIR)/core_cm3.h: | $(CMSIS_CORE_DIR)
	$(call download_file,$(CMSIS_CORE_URL)/core_cm3.h,$@)

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

# SVD file
$(CMSIS_DEVICE_DIR)/STM32F103xx.svd: | $(CMSIS_DEVICE_DIR)
	$(call download_file,$(SVD_URL),$@)

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
            $(TEST_MOCK)/hw_model.c \
            $(TEST_MOCK)/ds18b20_test_spy.c \
            $(TEST_MOCK)/ds18b20_test_access.c
# Pointer<->register casts (driver targets a 32-bit Cortex-M3) are expected
# on a 64-bit host; suppress the size warnings.
TEST_FLAG = -DHOST_BUILD -DDS18B20_TEST_HARNESS -Wall -Wextra -Wswitch-enum \
            -Wno-unused-parameter -Wno-pointer-to-int-cast -Wno-int-to-pointer-cast
TEST_INC  = -Iinc -I$(TEST_MOCK)

.PHONY: test
test: $(TEST_OUT)/ds18b20_test.exe
	$(TEST_OUT)/ds18b20_test.exe

$(TEST_OUT)/ds18b20_test.exe: $(TEST_SRC) src/ds18b20.c src/onewire.c Makefile | $(TEST_OUT)
	$(HOST_CC) $(TEST_FLAG) $(TEST_INC) $(TEST_SRC) -o $@

$(TEST_OUT):
	mkdir -p $@

# Include the dependency files generated during compilation
-include $(wildcard $(BUILD_DIR)/*.d)

# Help target
help:
	@echo "Available targets:"
	@echo "  all             - Build project (downloads dependencies if needed) [DEFAULT]"
	@echo "  download-deps   - Download all missing build dependencies"
	@echo "  download-licenses - Download third-party license files to CMSIS/"
	@echo "  clean-deps      - Remove downloaded dependencies and CMSIS/ directories"
	@echo "  clean           - Remove build artifacts"
	@echo "  test            - Build and run host tests (tests/, PC toolchain)"
	@echo "  debug           - Build with debug symbols"
	@echo "  program         - Program device using ST-LINK"
	@echo "  jprogram        - Program device using J-LINK"
	@echo "  gccversion      - Show compiler version"
	@echo "  help            - Show this help"

# *** EOF ***
