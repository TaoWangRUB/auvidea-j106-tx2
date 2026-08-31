# Makefile — IMX296 trigger generator, STM32F401 (HAL + FreeRTOS + USB CDC).
# Target: WeAct Studio MiniSTM32F4x1, STM32F401CEU6.  Build on the x86-64 host.

TARGET  = camtrig-f401
BUILD   = build
PORT   ?= /dev/ttyUSB0
BAUD   ?= 115200

PREFIX  = arm-none-eabi-
CC      = $(PREFIX)gcc
AS      = $(PREFIX)gcc -x assembler-with-cpp
OBJCOPY = $(PREFIX)objcopy
SIZE    = $(PREFIX)size

# Cortex-M4 with the single-precision FPU.  Hard-float must match the FreeRTOS
# ARM_CM4F port, which saves/restores S16-S31 on context switch.
CPU = -mcpu=cortex-m4 -mthumb -mfpu=fpv4-sp-d16 -mfloat-abi=hard

C_DEFS = -DUSE_HAL_DRIVER -DSTM32F401xE

C_INCLUDES = \
  -ICore/Inc \
  -IDrivers/CMSIS/Include \
  -IDrivers/CMSIS/Device/ST/STM32F4xx/Include \
  -IDrivers/STM32F4xx_HAL_Driver/Inc \
  -IDrivers/STM32F4xx_HAL_Driver/Inc/Legacy \
  -IMiddlewares/ST/STM32_USB_Device_Library/Core/Inc \
  -IMiddlewares/ST/STM32_USB_Device_Library/Class/CDC/Inc \
  -IMiddlewares/Third_Party/FreeRTOS/Source/include \
  -IMiddlewares/Third_Party/FreeRTOS/Source/CMSIS_RTOS_V2 \
  -IMiddlewares/Third_Party/FreeRTOS/Source/portable/GCC/ARM_CM4F \
  -IUSB_DEVICE/App \
  -IUSB_DEVICE/Target

FR = Middlewares/Third_Party/FreeRTOS/Source

C_SOURCES = \
  $(wildcard Core/Src/*.c) \
  $(wildcard Drivers/STM32F4xx_HAL_Driver/Src/*.c) \
  $(wildcard USB_DEVICE/App/*.c) \
  $(wildcard USB_DEVICE/Target/*.c) \
  Middlewares/ST/STM32_USB_Device_Library/Core/Src/usbd_core.c \
  Middlewares/ST/STM32_USB_Device_Library/Core/Src/usbd_ctlreq.c \
  Middlewares/ST/STM32_USB_Device_Library/Core/Src/usbd_ioreq.c \
  Middlewares/ST/STM32_USB_Device_Library/Class/CDC/Src/usbd_cdc.c \
  $(FR)/croutine.c $(FR)/event_groups.c $(FR)/list.c $(FR)/queue.c \
  $(FR)/stream_buffer.c $(FR)/tasks.c $(FR)/timers.c \
  $(FR)/CMSIS_RTOS_V2/cmsis_os2.c \
  $(FR)/portable/MemMang/heap_4.c \
  $(FR)/portable/GCC/ARM_CM4F/port.c

ASM_SOURCES = startup_stm32f401xe.s

LDSCRIPT = STM32F401CEUX_FLASH.ld

OPT = -Og
CFLAGS  = $(CPU) $(C_DEFS) $(C_INCLUDES) $(OPT) -Wall -fdata-sections -ffunction-sections -g -gdwarf-2
ASFLAGS = $(CPU) $(OPT) -Wall -fdata-sections -ffunction-sections -g -gdwarf-2
LDFLAGS = $(CPU) -specs=nano.specs -T$(LDSCRIPT) -lc -lm -lnosys \
          -Wl,-Map=$(BUILD)/$(TARGET).map,--cref -Wl,--gc-sections

OBJECTS  = $(addprefix $(BUILD)/,$(notdir $(C_SOURCES:.c=.o)))
vpath %.c $(sort $(dir $(C_SOURCES)))
OBJECTS += $(addprefix $(BUILD)/,$(notdir $(ASM_SOURCES:.s=.o)))
vpath %.s $(sort $(dir $(ASM_SOURCES)))

.PHONY: all clean flash flash-swd backup size console

all: $(BUILD)/$(TARGET).elf $(BUILD)/$(TARGET).bin size

$(BUILD)/%.o: %.c Makefile | $(BUILD)
	$(CC) -c $(CFLAGS) -Wa,-a,-ad,-alms=$(BUILD)/$(notdir $(<:.c=.lst)) $< -o $@

$(BUILD)/%.o: %.s Makefile | $(BUILD)
	$(AS) -c $(ASFLAGS) $< -o $@

$(BUILD)/$(TARGET).elf: $(OBJECTS) Makefile $(LDSCRIPT)
	$(CC) $(OBJECTS) $(LDFLAGS) -o $@

$(BUILD)/$(TARGET).bin: $(BUILD)/$(TARGET).elf
	$(OBJCOPY) -O binary -S $< $@

$(BUILD):
	mkdir -p $@

size: $(BUILD)/$(TARGET).elf
	@$(SIZE) $<

# The F401 ROM speaks USB DFU, so the H7 build's no-BOOT0 trick works: ask the
# running firmware to reboot into the bootloader, then write.
#
# The leading `-` on dfu-util is required: ST's DfuSe bootloader resets on the
# leave request and never answers the final get_status, so dfu-util reports
# "Error during download get_status" and exits 74 on a successful flash.
# Judge success by 0483:df11 LEAVING the bus.
flash: $(BUILD)/$(TARGET).bin
	@command -v dfu-util >/dev/null || { echo "need: sudo apt install dfu-util"; exit 1; }
	@if [ -e $(PORT) ] && ! lsusb | grep -q 0483:df11; then \
	  echo "asking $(PORT) to enter the bootloader..."; \
	  stty -F $(PORT) $(BAUD) raw -echo 2>/dev/null || true; \
	  printf 'dfu\n' > $(PORT) 2>/dev/null || true; \
	  i=0; while [ $$i -lt 12 ]; do sleep 0.5; \
	    lsusb | grep -q 0483:df11 && break; i=$$((i+1)); done; \
	fi
	@lsusb | grep -q 0483:df11 || { \
	  echo "not in DFU — hold BOOT0, tap NRST, release BOOT0, then retry"; exit 1; }
	-dfu-util -a 0 -s 0x08000000:leave -D $<
	@sleep 2; if lsusb | grep -q 0483:df11; then echo "FAILED: still in DFU"; exit 1; \
	else echo "flashed and left DFU"; fi

backup: | $(BUILD)
	dfu-util -a 0 -s 0x08000000:524288 -U $(BUILD)/flash-backup.bin

flash-swd: $(BUILD)/$(TARGET).bin
	st-flash write $< 0x08000000

console:
	@echo "115200 8N1 on $(PORT)"
	screen $(PORT) 115200

clean:
	rm -rf $(BUILD)
