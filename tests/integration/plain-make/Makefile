CC = gcc
BUILD_DIR ?= build/ow-integration-consumer
OW_1WIRE_DIR := $(abspath ../../..)
OW_TARGET := f1

CPPFLAGS += -I$(OW_1WIRE_DIR)/include -I$(OW_1WIRE_DIR)/tests/mock
CFLAGS += -std=c11 -Wall -Wextra -Werror -Wno-pointer-to-int-cast -DOW_PORT_TARGET_F1

include $(OW_1WIRE_DIR)/ow-integration.mk

APP_SRC := main.c mock.c
APP_OBJ := $(addprefix $(BUILD_DIR)/,$(APP_SRC:.c=.o))
TARGET := $(BUILD_DIR)/consumer.elf

.PHONY: all clean
all: $(TARGET)

$(BUILD_DIR)/%.o: %.c | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -MMD -MP -MF $(@:%.o=%.d) -c $< -o $@

$(BUILD_DIR):
	mkdir -p $@

$(TARGET): $(APP_OBJ) $(OW_1WIRE_OBJ)
	$(CC) $(LDFLAGS) $^ -o $@

-include $(APP_OBJ:.o=.d)

clean:
	rm -rf $(BUILD_DIR)
