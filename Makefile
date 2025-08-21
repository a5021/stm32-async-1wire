
# Define the name of the project target and the build directory
TARGET = ds18b20_demo
BUILD_DIR = build

ifeq (0, 1)
	# Set build directory based on make goals (debug or release)
	ifeq ($(MAKECMDGOALS), debug)
    		BUILD_DIR := $(addsuffix -debug, $(BUILD_DIR))
	else
		BUILD_DIR := $(addsuffix -release, $(BUILD_DIR))
	endif
endif

# Define the C source files, assembly source file, linker script, and preprocessor definitions
SRC = system_stm32f1xx.c demo.c ds18b20.c
ASM = startup_stm32f103xb.s
LDS = STM32F103XB_FLASH.ld
MCU = -mcpu=cortex-m3 -mthumb
DEF = -DSTM32F103xB
INC = -I.
#OPT = -O3 -fno-omit-frame-pointer -flto -g -fopt-info-inline-all=inline_report.txt --param max-inline-insns-auto=480 # -g0 
OPT = -O3 -flto -g0 -fopt-info-inline-all=inline_report.txt --param max-inline-insns-auto=480

# Define additional preprocessor definitions based on conditional variables
USE := USE_ALL USE_PLL USE_HSE USE_LSE USE_ADC USE_USART USE_I2C USE_SPI 
USE += USE_BTN USE_RTC USE_TIM2 USE_DMA USE_CRC USE_RNG USE_IWDG USE_WWDG 
USE += USE_EXTI USE_FLASH USE_SWD OVRCLK
DEF += $(strip $(foreach def, $(USE), $(if $($(def)), -D$(def)=$($(def)))))

# Define the toolchain to use
TOOLCHAIN := $(if $(GCC_PATH),$(GCC_PATH)/,)arm-none-eabi-

#$(info The value of TOOLCHAIN is: $(TOOLCHAIN))
#$(info The value of DEF is: $(DEF))

# Define the compiler, assembler, object copy, and size utilities
CC = $(TOOLCHAIN)gcc
AS = $(TOOLCHAIN)gcc -x assembler-with-cpp
CP = $(TOOLCHAIN)objcopy
SZ = $(TOOLCHAIN)size

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

# Suppress RWX segment warnings
LDFLAGS += -Wl,--no-warn-rwx-segment

# Build all targets by default: the ELF binary, the HEX file, and the raw binary file
all: $(BUILD_DIR)/$(TARGET).elf $(BUILD_DIR)/$(TARGET).hex $(BUILD_DIR)/$(TARGET).bin

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

# Perform the 'test' target, which enables all conditional definitions and builds the project
test:  FLAG += -DUSE_ALL=1
test:  all

# Perform the 'debug' target, which enables SWD and debug symbols and builds the project
debug: OPT = -Og -g3 -gdwarf
debug: FLAG += -DUSE_SWD=1
debug: all

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

# *** EOF ***
