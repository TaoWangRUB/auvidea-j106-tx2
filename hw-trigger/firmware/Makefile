# camtrig — IMX296 external trigger generator for the J106/TX2 rig.
# Target: WeAct MiniSTM32H7xx (STM32H743VIT6).  Build on the x86-64 host.

TARGET  := camtrig
BUILD   := build
SRC     := src/startup.c src/main.c

CROSS   := arm-none-eabi-
CC      := $(CROSS)gcc
OBJCOPY := $(CROSS)objcopy
SIZE    := $(CROSS)size

# Cortex-M7, soft float: nothing here uses floating point (rates and
# exposures are integer nanoseconds), so the FPU is never enabled and the
# ABI stays trivially correct.
ARCH    := -mcpu=cortex-m7 -mthumb -mfloat-abi=soft

CFLAGS  := $(ARCH) -std=gnu11 -Os -g3 \
           -Wall -Wextra -Werror -Wshadow -Wundef \
           -ffunction-sections -fdata-sections \
           -fno-common -fno-builtin

LDFLAGS := $(ARCH) -T stm32h743.ld -nostdlib -Wl,--gc-sections \
           -Wl,-Map=$(BUILD)/$(TARGET).map

OBJ     := $(patsubst src/%.c,$(BUILD)/%.o,$(SRC))

.PHONY: all clean flash size

all: $(BUILD)/$(TARGET).bin size

$(BUILD):
	@mkdir -p $(BUILD)

$(BUILD)/%.o: src/%.c | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/$(TARGET).elf: $(OBJ) stm32h743.ld
	$(CC) $(LDFLAGS) $(OBJ) -o $@ -lgcc

$(BUILD)/$(TARGET).bin: $(BUILD)/$(TARGET).elf
	$(OBJCOPY) -O binary $< $@

size: $(BUILD)/$(TARGET).elf
	@$(SIZE) $<

# Hold BOOT0, tap NRST, release BOOT0 first — that enters the STM32 ROM
# bootloader, which provides USB DFU with no on-board firmware involved.
flash: $(BUILD)/$(TARGET).bin
	dfu-util -a 0 -s 0x08000000:leave -D $(BUILD)/$(TARGET).bin

clean:
	rm -rf $(BUILD)
