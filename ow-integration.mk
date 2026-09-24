# =============================================================================
# ow-integration.mk — drop-in fragment for plain Makefile projects
# =============================================================================
# Include this file from your application Makefile to compile the
# stm32-async-1wire library sources into your firmware:
#
#   OW_1WIRE_DIR := third_party/stm32-async-1wire
#   include $(OW_1WIRE_DIR)/ow-integration.mk
#
#   my_firmware.elf: $(APP_OBJ) $(OW_1WIRE_OBJ)
#           $(CC) $^ $(LDFLAGS) -o $@
#
# Variables you may set BEFORE the include:
#   OW_TARGET        f1 (default) | f0 | g0
#   OW_1WIRE_CFLAGS  extra flags for library TUs only (e.g. -DOW_STATS_ENABLE=1)
#   OW_1WIRE_DEFS    extra -D defines for library TUs (app defines stay yours)
#
# The fragment provides:
#   OW_1WIRE_ROOT   absolute path to the library checkout
#   OW_1WIRE_SRC    library .c files (onewire, ds18b20, ow_stats)
#   OW_1WIRE_INC    -I flags for include/ and port/
#   OW_1WIRE_DEFS   -D flags for the selected MCU family
#   OW_1WIRE_OBJ    object files (built into $(OW_1WIRE_BUILD_DIR))
#
# CMSIS device headers are NOT pulled in here — your project already has them
# (Cube firmware tree, `make download-deps`, FetchContent, ...). Add their -I
# to your global CPPFLAGS as you already do for your own sources.
# =============================================================================

ifndef OW_1WIRE_DIR
$(error OW_1WIRE_DIR is not set — point it at the stm32-async-1wire checkout \
before including ow-integration.mk)
endif

OW_1WIRE_ROOT := $(abspath $(OW_1WIRE_DIR))

# --- MCU family ---------------------------------------------------------------
OW_TARGET ?= f1
ifeq ($(OW_TARGET),f0)
OW_1WIRE_FAMILY_DEF := -DOW_PORT_TARGET_F0
else ifeq ($(OW_TARGET),g0)
OW_1WIRE_FAMILY_DEF := -DOW_PORT_TARGET_G0
else ifeq ($(OW_TARGET),f1)
OW_1WIRE_FAMILY_DEF := -DOW_PORT_TARGET_F1
else
$(error OW_TARGET must be f1, f0 or g0 (got: $(OW_TARGET)))
endif

# --- Public surface -----------------------------------------------------------
OW_1WIRE_SRC := \
	$(OW_1WIRE_ROOT)/src/onewire.c \
	$(OW_1WIRE_ROOT)/src/ds18b20.c \
	$(OW_1WIRE_ROOT)/src/ow_stats.c

OW_1WIRE_INC := \
	-I$(OW_1WIRE_ROOT)/include \
	-I$(OW_1WIRE_ROOT)/port

OW_1WIRE_DEFS := $(OW_1WIRE_FAMILY_DEF) $(OW_1WIRE_DEFS)
OW_1WIRE_CFLAGS ?=

# Objects land next to your own — override if you prefer a dedicated dir.
OW_1WIRE_BUILD_DIR ?= $(BUILD_DIR)
OW_1WIRE_OBJ := $(addprefix $(OW_1WIRE_BUILD_DIR)/,$(notdir $(OW_1WIRE_SRC:.c=.o)))

# VPATH so the generic %.c -> %.o rule below can find the library sources.
VPATH += $(OW_1WIRE_ROOT)/src

# --- Compile rule -------------------------------------------------------------
# Uses your $(CC), $(CFLAGS) and $(BUILD_DIR) — the fragment adapts to the
# including Makefile. Library-specific flags are appended, not mixed in.
$(OW_1WIRE_BUILD_DIR)/%.o: $(OW_1WIRE_ROOT)/src/%.c | $(OW_1WIRE_BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(OW_1WIRE_INC) $(OW_1WIRE_DEFS) $(OW_1WIRE_CFLAGS) -c $< -o $@

$(OW_1WIRE_BUILD_DIR):
	mkdir -p $@

# Convenience: mark the library objects as up-to-date dependencies of `all`
# if the including Makefile wants them, or reference $(OW_1WIRE_OBJ) explicitly.
OW_1WIRE_READY := 1
