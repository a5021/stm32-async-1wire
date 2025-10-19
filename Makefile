# Define the name of the project target and the build directory
TARGET = ds18b20_demo
BUILD_DIR = build

# Define the C source files, assembly source file, linker script, and preprocessor definitions
SRC = system_stm32f1xx.c demo.c ds18b20.c
ASM = startup_stm32f103xb.s
LDS = STM32F103XB_FLASH.ld
MCU = -mcpu=cortex-m3 -mthumb
DEF = -DSTM32F103xB
INC = -I.

# Define additional preprocessor definitions based on conditional variables
USE := USE_HSI ELAPSED_TIME
DEF += $(strip $(foreach def, $(USE), $(if $($(def)), -D$(def)=$($(def)))))

# Optimization flags for the compiler:
# -O3          : Maximum optimization level for performance (includes -O2 plus more aggressive optimizations)
# -flto        : Link Time Optimization - enables cross-file optimization during linking
# -g0          : No debug information (reduces binary size, incompatible with debugging)
# -fopt-info-inline-all=inline_report.txt : Generate detailed inline optimization report to file
# --param max-inline-insns-auto=480 : Set auto-inlining threshold to 480 instructions (more aggressive inlining)

OPT = -O3 -flto -g0 -fopt-info-inline-all=inline_report.txt --param max-inline-insns-auto=480

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

    FLAG += -D WIN32
    ifeq ($(PROCESSOR_ARCHITEW6432), AMD64)
        FLAG += -D AMD64
    else
        ifeq ($(PROCESSOR_ARCHITECTURE), AMD64)
            FLAG += -D AMD64
        endif
        ifeq ($(PROCESSOR_ARCHITECTURE), x86)
            FLAG += -D IA32
        endif
    endif

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
    UNAME_P := $(shell uname -p)
    ifeq ($(UNAME_P), x86_64)
        FLAG += -D AMD64
    endif
    ifneq ($(filter %86, $(UNAME_P)),)
        FLAG += -D IA32
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

# Tools detection - prefer wget
WGET := $(shell command -v wget 2> /dev/null)

# Base URLs
RAW_URL = https://raw.githubusercontent.com
ST_URL = $(RAW_URL)/STMicroelectronics/
CMSIS_URL = $(ST_URL)STM32CubeF1/master/Drivers/CMSIS/Include
F1_URL = $(ST_URL)cmsis_device_f1/master
SVD_URL = https://raw.githubusercontent.com/cmsis-svd/cmsis-svd-data/refs/heads/main/data/STMicro/STM32F103xx.svd

# Required external files (needed for build but not in repo)
EXTERNAL_DEPS = system_stm32f1xx.c \
                startup_stm32f103xb.s \
                stm32f1xx.h \
                stm32f103xb.h \
                system_stm32f1xx.h \
                cmsis_compiler.h \
                cmsis_gcc.h \
                cmsis_version.h \
                core_cm3.h \
                STM32F103xx.svd

# Download function using wget
define download_file
	@echo "Downloading $(2)..."
	@if [ -z "$(WGET)" ]; then \
		echo "Error: wget is required but not found. Please install wget."; \
		exit 1; \
	fi
	@$(WGET) -q -O "$(2)" "$(1)" && echo "  OK" || (echo "  FAILED"; exit 1)
endef

# File-specific download rules
download-system_stm32f1xx.c:
	$(call download_file,$(F1_URL)/Source/Templates/system_stm32f1xx.c,system_stm32f1xx.c)

download-startup_stm32f103xb.s:
	$(call download_file,$(F1_URL)/Source/Templates/gcc/startup_stm32f103xb.s,startup_stm32f103xb.s)

download-stm32f1xx.h:
	$(call download_file,$(F1_URL)/Include/stm32f1xx.h,stm32f1xx.h)

download-stm32f103xb.h:
	$(call download_file,$(F1_URL)/Include/stm32f103xb.h,stm32f103xb.h)

download-system_stm32f1xx.h:
	$(call download_file,$(F1_URL)/Include/system_stm32f1xx.h,system_stm32f1xx.h)

download-cmsis_compiler.h:
	$(call download_file,$(CMSIS_URL)/cmsis_compiler.h,cmsis_compiler.h)

download-cmsis_gcc.h:
	$(call download_file,$(CMSIS_URL)/cmsis_gcc.h,cmsis_gcc.h)

download-cmsis_version.h:
	$(call download_file,$(CMSIS_URL)/cmsis_version.h,cmsis_version.h)

download-core_cm3.h:
	$(call download_file,$(CMSIS_URL)/core_cm3.h,core_cm3.h)

download-STM32F103xx.svd:
	$(call download_file,$(SVD_URL),STM32F103xx.svd)

# Check if files exist and download if missing
check-deps:
	@for file in $(EXTERNAL_DEPS); do \
		if [ ! -f "$$file" ]; then \
			echo "$$file not found, downloading..."; \
			$(MAKE) download-$$file; \
		else \
			echo "$$file already exists"; \
		fi \
	done

# Target to download all dependencies
download-deps: check-deps
	@echo "All build dependencies checked/downloaded successfully"

# Clean external dependencies
clean-deps:
	rm -f $(EXTERNAL_DEPS)

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
	@echo "  all           - Build project (downloads dependencies if needed) [DEFAULT]"
	@echo "  download-deps - Download all missing build dependencies"
	@echo "  clean-deps    - Remove downloaded dependencies"
	@echo "  clean         - Remove build artifacts"
	@echo "  debug         - Build with debug symbols"
	@echo "  program       - Program device using ST-LINK"
	@echo "  jprogram      - Program device using J-LINK"
	@echo "  gccversion    - Show compiler version"
	@echo "  help          - Show this help"

# *** EOF ***
