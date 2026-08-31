# Makefile — IMX296 trigger generator, STM32F401 stopgap build.
# Target: STM32F401CDU6 (WeAct Black Pill).  Build on the x86-64 host.
#
# Deliberately standalone: no HAL, no CMSIS, no FreeRTOS, no submodules.  Three
# sources and a linker script, so `make` works with nothing but the toolchain.

TARGET  = camtrig-f401
BUILD   = build
PORT   ?= /dev/ttyUSB0
BAUD   ?= 115200

# CDU6 is 384K.  The ROM bootloader advertises 512K because it reports a
# family layout rather than the populated size; if yours really is a CE:
#   make FLASH_SIZE=512K
FLASH_SIZE ?= 384K
RAM_SIZE   ?= 96K

# WeAct Black Pill fits a 25 MHz crystal.  For a board with a different one:
#   make HSE_HZ=8000000
HSE_HZ  ?= 25000000

PREFIX  = arm-none-eabi-
CC      = $(PREFIX)gcc
OBJCOPY = $(PREFIX)objcopy
SIZE    = $(PREFIX)size

SRCS    = src/main.c src/startup.c src/usb_cdc.c
OBJS    = $(SRCS:%.c=$(BUILD)/%.o)

CPU     = -mcpu=cortex-m4 -mthumb

CFLAGS  = $(CPU) -std=gnu11 -Os -g3 \
          -ffunction-sections -fdata-sections \
          -Wall -Wextra -Wno-unused-parameter \
          -fno-common -fno-builtin \
          -DHSE_HZ=$(HSE_HZ)

# -nostdlib keeps the image tiny and free of a libc we never call; -lgcc is
# still required because the timing maths divides 64-bit values and the
# compiler emits __aeabi_uldivmod for that.
LDFLAGS = $(CPU) -T stm32f401.ld -nostdlib -lgcc \
          -Wl,--gc-sections -Wl,-Map=$(BUILD)/$(TARGET).map \
          -Wl,--defsym=FLASH_SIZE=$(FLASH_SIZE) \
          -Wl,--defsym=RAM_SIZE=$(RAM_SIZE)

.PHONY: all clean flash flash-uart flash-swd size console

all: $(BUILD)/$(TARGET).bin size

$(BUILD)/%.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/$(TARGET).elf: $(OBJS) stm32f401.ld
	$(CC) $(OBJS) $(LDFLAGS) -o $@

$(BUILD)/$(TARGET).bin: $(BUILD)/$(TARGET).elf
	$(OBJCOPY) -O binary $< $@

size: $(BUILD)/$(TARGET).elf
	@$(SIZE) $<

# Flash over USB using the F401's ROM DFU bootloader.
#
# Unlike the F103 (UART-only ROM), the F401 speaks USB DFU, so the H7 build's
# no-BOOT0 trick works here: if the board is running this firmware and its
# console is reachable, ask it to reboot into the bootloader first.  Falls
# through silently when no port answers -- already in DFU, or running
# something else.
#
# The leading `-` on dfu-util is required, not sloppiness: ST's DfuSe
# bootloader resets the moment it receives the leave request and never answers
# the final get_status, so dfu-util reports "Error during download get_status"
# and exits 74 on a completely successful flash.  Confirm success by the device
# *leaving* the bus: if 0483:df11 is still listed afterwards, the write really
# did fail.
flash: $(BUILD)/$(TARGET).bin
	@command -v dfu-util >/dev/null || { \
	  echo "dfu-util not installed:  sudo apt install dfu-util"; exit 1; }
	@if [ -e $(PORT) ] && ! lsusb | grep -q 0483:df11; then \
	  echo "asking $(PORT) to enter the bootloader..."; \
	  stty -F $(PORT) $(BAUD) raw -echo 2>/dev/null || true; \
	  printf 'dfu\n' > $(PORT) 2>/dev/null || true; \
	  i=0; while [ $$$$i -lt 12 ]; do \
	    sleep 0.5; \
	    if lsusb | grep -q 0483:df11; then break; fi; \
	    i=$$$$((i+1)); \
	  done; \
	fi
	@lsusb | grep -q 0483:df11 || { \
	  echo "not in DFU — hold BOOT0, tap NRST, release BOOT0, then retry"; exit 1; }
	-dfu-util -a 0 -s 0x08000000:leave -D $<
	@sleep 2; if lsusb | grep -q 0483:df11; then \
	  echo "FAILED: still in DFU mode"; exit 1; \
	else echo "flashed and left DFU (running application firmware)"; fi

# Read the whole flash back before overwriting someone else's firmware.
backup:
	@mkdir -p $(BUILD)
	dfu-util -a 0 -s 0x08000000:$(shell echo $(FLASH_SIZE) | sed 's/K/*1024/' | bc) \
	  -U $(BUILD)/flash-backup.bin

# Alternative path if you would rather use the ST-Link than USB.
flash-swd: $(BUILD)/$(TARGET).bin
	@command -v st-flash >/dev/null || { \
	  echo "st-flash not installed:  sudo apt install stlink-tools"; exit 1; }
	st-flash write $< 0x08000000

# Talk to the running firmware.  tools/j106-trigctl.py speaks the same
# protocol and is the better interface; this is for a quick poke.
console:
	@echo "115200 8N1 on $(PORT) — ctrl-a k to quit screen"
	screen $(PORT) 115200

clean:
	rm -rf $(BUILD)
