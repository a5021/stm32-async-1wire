# Define the name of the project target and the build directory
TARGET = ds18b20_demo
BUILD_DIR = build

# CMSIS directory structure for third-party build dependencies
CMSIS_CORE_DIR   = CMSIS/core
CMSIS_DEVICE_DIR = CMSIS/device

# Define the C source files, assembly source file, linker script, and preprocessor definitions
SRC = $(CMSIS_DEVICE_DIR)/system_stm32f1xx.c src/demo.c src/ds18b20.c
ASM = $(CMSIS_DEVICE_DIR)/startup_stm32f103xb.s
LDS = STM32F103XB_FLASH.ld
MCU = -mcpu=cortex-m3 -mthumb
DEF = -DSTM32F103xB
INC = -I. -Iinc -I$(CMSIS_CORE_DIR) -I$(CMSIS_DEVICE_DIR)

# Define additional preprocessor definitions based on conditional variables
# make HSI_8MHZ=1  →  -DHSI_8MHZ=1  (run on HSI 8MHz instead of HSE+PLL 72MHz)
USE := HSI_8MHZ
DEF += $(strip $(foreach def, $(USE), $(if $($(def)), -D$(def)=$($(def)))))

# Optimization flags for the compiler:
# -O3          : Maximum optimization level for performance (includes -O2 plus more aggressive optimizations)
# -flto        : Link Time Optimization - enables cross-file optimization during linking
# -g0          : No debug information (reduces binary size, incompatible with debugging)
# -fopt-info-inline-all=inline_report.txt : Generate detailed inline optimization report to file
# --param max-inline-insns-auto=480 : Set auto-inlining threshold to 480 instructions (more aggressive inlining)

OPT = -O3 -flto -g0 -fopt-info-inline-all=$(BUILD_DIR)/inline_report.txt --param max-inline-insns-auto=480

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
FLAG = $(MCU) $(DEF) $(INC) -Wall -Werror -Wextra -Wpedantic -fdata-sections -ffunction-sections

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

# Define the object files that need to be built from C and assembly source files
OBJ = $(addprefix $(BUILD_DIR)/,$(notdir $(SRC:.c=.o)))
vpath %.c $(sort $(dir $(SRC))) # Set the search path for C source files

OBJ += $(addprefix $(BUILD_DIR)/,$(notdir $(ASM:.s=.o)))
vpath %.s $(sort $(dir $(ASM))) # Set the search path for assembly source files

# Specify how to compile a C source file into an object file
$(BUILD_DIR)/%.o: %.c Makefile | $(BUILD_DIR)
	$(CC) -c $(FLAG) $(OPT) $(EXT) $< -o $@

# Specify how to compile an assembly source file into an object file
$(BUILD_DIR)/%.o: %.s Makefile | $(BUILD_DIR)
	$(AS) -c $(FLAG) $(OPT) $(EXT) -Wa,-a,-ad,-alms=$(BUILD_DIR)/$(notdir $(<:.c=.lst)) $< -o $@

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
	@echo "  debug           - Build with debug symbols"
	@echo "  program         - Program device using ST-LINK"
	@echo "  jprogram        - Program device using J-LINK"
	@echo "  gccversion      - Show compiler version"
	@echo "  help            - Show this help"

# *** EOF ***
