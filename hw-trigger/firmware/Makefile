# camtrig — IMX296 external trigger generator for the J106/TX2 rig.
# Target: WeAct MiniSTM32H7xx (STM32H743VIT6).  Build on the x86-64 host.
#
# Standard STM32 project: CMSIS + HAL + ST USB Device Library + FreeRTOS,
# vendored at the versions recorded in VENDOR.md.  No CubeIDE required —
# arm-none-eabi-gcc, make, and dfu-util are the whole toolchain.

TARGET  := camtrig
BUILD   := build
DEBUG   ?= 0

CROSS   := arm-none-eabi-
CC      := $(CROSS)gcc
AS      := $(CROSS)gcc -x assembler-with-cpp
OBJCOPY := $(CROSS)objcopy
SIZE    := $(CROSS)size

# Cortex-M7 with the double-precision FPU.  Hard float, unlike the previous
# bare-metal build: the newlib-nano multilib and the prebuilt-style HAL are
# selected by ABI, and mixing ABIs is a link-time or run-time failure (D9).
ARCH    := -mcpu=cortex-m7 -mthumb -mfpu=fpv5-d16 -mfloat-abi=hard

HALDIR  := Drivers/STM32H7xx_HAL_Driver
CMSIS   := Drivers/CMSIS
USBDIR  := Middlewares/ST/STM32_USB_Device_Library
RTOSDIR := Middlewares/Third_Party/FreeRTOS/Source

C_INCLUDES := \
  -ICore/Inc \
  -I$(HALDIR)/Inc \
  -I$(HALDIR)/Inc/Legacy \
  -I$(CMSIS)/Device/ST/STM32H7xx/Include \
  -I$(CMSIS)/Include \
  -I$(RTOSDIR)/include \
  -I$(RTOSDIR)/CMSIS_RTOS_V2 \
  -I$(RTOSDIR)/portable/GCC/ARM_CM7/r0p1 \
  -I$(USBDIR)/Core/Inc \
  -I$(USBDIR)/Class/CDC/Inc \
  -IDrivers/BSP/ST7735

# CMSIS_device_header: required by freertos_os2.h, which does a bare
#   `#include CMSIS_device_header` and provides no default.
# USE_CUSTOM_SYSTICK_HANDLER_IMPLEMENTATION=0: use cmsis_os2.c's own
#   SysTick_Handler, which ticks the kernel only once the scheduler is running.
#   Stated explicitly rather than left to an implicit default.
C_DEFS  := -DUSE_HAL_DRIVER -DSTM32H743xx -DCMSIS_device_header=\"stm32h7xx.h\" \
           -DUSE_CUSTOM_SYSTICK_HANDLER_IMPLEMENTATION=0 -DTFT96

C_SOURCES := \
  $(wildcard Core/Src/*.c) \
  $(HALDIR)/Src/stm32h7xx_hal.c \
  $(HALDIR)/Src/stm32h7xx_hal_cortex.c \
  $(HALDIR)/Src/stm32h7xx_hal_dma.c \
  $(HALDIR)/Src/stm32h7xx_hal_dma_ex.c \
  $(HALDIR)/Src/stm32h7xx_hal_exti.c \
  $(HALDIR)/Src/stm32h7xx_hal_flash.c \
  $(HALDIR)/Src/stm32h7xx_hal_flash_ex.c \
  $(HALDIR)/Src/stm32h7xx_hal_gpio.c \
  $(HALDIR)/Src/stm32h7xx_hal_mdma.c \
  $(HALDIR)/Src/stm32h7xx_hal_pwr.c \
  $(HALDIR)/Src/stm32h7xx_hal_pwr_ex.c \
  $(HALDIR)/Src/stm32h7xx_hal_rcc.c \
  $(HALDIR)/Src/stm32h7xx_hal_rcc_ex.c \
  $(HALDIR)/Src/stm32h7xx_hal_tim.c \
  $(HALDIR)/Src/stm32h7xx_hal_tim_ex.c \
  $(HALDIR)/Src/stm32h7xx_hal_uart.c \
  $(HALDIR)/Src/stm32h7xx_hal_uart_ex.c \
  $(HALDIR)/Src/stm32h7xx_hal_spi.c \
  $(HALDIR)/Src/stm32h7xx_hal_spi_ex.c \
  $(HALDIR)/Src/stm32h7xx_hal_pcd.c \
  $(HALDIR)/Src/stm32h7xx_hal_pcd_ex.c \
  $(HALDIR)/Src/stm32h7xx_ll_usb.c \
  $(CMSIS)/Device/ST/STM32H7xx/Source/Templates/system_stm32h7xx.c \
  $(RTOSDIR)/croutine.c \
  $(RTOSDIR)/event_groups.c \
  $(RTOSDIR)/list.c \
  $(RTOSDIR)/queue.c \
  $(RTOSDIR)/stream_buffer.c \
  $(RTOSDIR)/tasks.c \
  $(RTOSDIR)/timers.c \
  $(RTOSDIR)/CMSIS_RTOS_V2/cmsis_os2.c \
  $(RTOSDIR)/portable/MemMang/heap_4.c \
  $(RTOSDIR)/portable/GCC/ARM_CM7/r0p1/port.c \
  $(USBDIR)/Core/Src/usbd_core.c \
  $(USBDIR)/Core/Src/usbd_ctlreq.c \
  $(USBDIR)/Core/Src/usbd_ioreq.c \
  $(USBDIR)/Class/CDC/Src/usbd_cdc.c \
  Drivers/BSP/ST7735/lcd.c \
  Drivers/BSP/ST7735/st7735.c \
  Drivers/BSP/ST7735/st7735_reg.c

ASM_SOURCES := $(CMSIS)/Device/ST/STM32H7xx/Source/Templates/gcc/startup_stm32h743xx.s

# --- Optimisation ------------------------------------------------------------
# -Os, as before: nothing here is throughput-bound and flash is not the
# constraint, but a smaller image is a faster DFU cycle during bring-up.
ifeq ($(DEBUG),1)
  OPT := -Og -g3 -gdwarf-2
else
  OPT := -Os -g3
endif

CFLAGS  := $(ARCH) $(C_DEFS) $(C_INCLUDES) $(OPT) -std=gnu11 \
           -Wall -Wextra -Wshadow -Wundef \
           -ffunction-sections -fdata-sections -fno-common \
           -MMD -MP

# The project's own code is held to -Werror; the vendored tree is not, because
# it is unmodified upstream and warnings there are not ours to fix (VENDOR.md).
CFLAGS_OWN := -Werror

ASFLAGS := $(ARCH) $(OPT) -Wall -fdata-sections -ffunction-sections

LDSCRIPT := STM32H743VITX_FLASH.ld
LIBS     := -lc -lm -lnosys
LDFLAGS  := $(ARCH) -specs=nano.specs -T$(LDSCRIPT) $(LIBS) \
            -Wl,-Map=$(BUILD)/$(TARGET).map,--cref -Wl,--gc-sections \
            -Wl,--print-memory-usage

OBJECTS := $(addprefix $(BUILD)/,$(notdir $(C_SOURCES:.c=.o)))
vpath %.c $(sort $(dir $(C_SOURCES)))
OBJECTS += $(addprefix $(BUILD)/,$(notdir $(ASM_SOURCES:.s=.o)))
vpath %.s $(sort $(dir $(ASM_SOURCES)))

.PHONY: all clean flash verify restore size

all: $(BUILD)/$(TARGET).elf $(BUILD)/$(TARGET).bin size

# Our own translation units get -Werror; vendored ones do not.
$(BUILD)/main.o $(BUILD)/camtrig.o $(BUILD)/cli.o $(BUILD)/stm32h7xx_it.o \
$(BUILD)/stm32h7xx_hal_msp.o $(BUILD)/syscalls.o $(BUILD)/sysmem.o \
$(BUILD)/usbd_cdc_if.o $(BUILD)/usbd_desc.o $(BUILD)/usb_device.o: CFLAGS += $(CFLAGS_OWN)

$(BUILD)/%.o: %.c Makefile | $(BUILD)
	$(CC) -c $(CFLAGS) $< -o $@

$(BUILD)/%.o: %.s Makefile | $(BUILD)
	$(AS) -c $(ASFLAGS) $< -o $@

$(BUILD)/$(TARGET).elf: $(OBJECTS) $(LDSCRIPT) Makefile
	$(CC) $(OBJECTS) $(LDFLAGS) -o $@

$(BUILD)/%.bin: $(BUILD)/%.elf
	$(OBJCOPY) -O binary -S $< $@

$(BUILD):
	@mkdir -p $@

size: $(BUILD)/$(TARGET).elf
	@$(SIZE) $<

# Hold BOOT0, tap NRST, release BOOT0 first — that enters the STM32 ROM
# bootloader, which provides USB DFU with no on-board firmware involved.
# `lsusb` should show 0483:df11 before running this.
#
# The leading `-` is required, not sloppiness: ST's DfuSe bootloader resets the
# moment it receives the leave request and never answers the final get_status,
# so dfu-util reports "Error during download get_status" and exits 74 on a
# completely successful flash.  Confirm success by the device *leaving* the bus:
# if 0483:df11 is still listed afterwards, the write really did fail.
# If the board is running and enumerated as a CDC port, ask it to reboot into
# the ROM bootloader so no BOOT0 press is needed.  Falls through silently when
# no port is present (already in DFU, or running firmware without the command).
# If the board is running and enumerated as a CDC port, ask it to reboot into
# the ROM bootloader so no BOOT0 press is needed.  Falls through silently when
# no port is present (already in DFU, or firmware without the command).
#
# The port goes into a shell variable rather than being expanded inline: with no
# /dev/ttyACM* present, `> $(DFUPORT)` expands to a redirect with no target and
# the shell fails to parse the whole recipe, even in DFU where it never runs.
flash: $(BUILD)/$(TARGET).bin
	@PORT=`ls /dev/ttyACM* 2>/dev/null | head -1`; \
	if [ -n "$$PORT" ] && ! lsusb | grep -q 0483:df11; then \
	  echo "asking $$PORT to enter the bootloader..."; \
	  stty -F "$$PORT" 115200 raw -echo 2>/dev/null || true; \
	  printf 'dfu\n' > "$$PORT" 2>/dev/null || true; \
	  i=0; while [ $$i -lt 12 ]; do \
	    sleep 0.5; \
	    if lsusb | grep -q 0483:df11; then break; fi; \
	    i=$$((i+1)); \
	  done; \
	fi
	@lsusb | grep -q 0483:df11 || { \
	  echo "not in DFU — hold BOOT0, tap NRST, release BOOT0, then retry"; exit 1; }
	-dfu-util -a 0 -s 0x08000000:leave -D $(BUILD)/$(TARGET).bin
	@sleep 2; if lsusb | grep -q 0483:df11; then \
	  echo "FAILED: still in DFU mode"; exit 1; \
	else echo "flashed and left DFU (running application firmware)"; fi

# Read the flash back and compare against the built image.  Leaves the board in
# DFU, so run `make flash` afterwards to boot it.
verify: $(BUILD)/$(TARGET).bin
	dfu-util -a 0 -s 0x08000000 -D $(BUILD)/$(TARGET).bin
	@sleep 2
	dfu-util -a 0 -s 0x08000000:$$(stat -c%s $(BUILD)/$(TARGET).bin) \
	         -U $(BUILD)/readback.bin
	@cmp $(BUILD)/$(TARGET).bin $(BUILD)/readback.bin \
	  && echo "VERIFIED: flash contents are byte-identical to the image"

# Restore the pre-restructure bare-metal build.  See known-good/README.md.
# Same expected exit-74 on the leave request as `flash` above.
restore:
	-dfu-util -a 0 -s 0x08000000:leave -D known-good/camtrig-baseline.bin
	@sleep 2; if lsusb | grep -q 0483:df11; then \
	  echo "FAILED: still in DFU mode"; exit 1; \
	else echo "restored baseline and left DFU"; fi

clean:
	rm -rf $(BUILD)

-include $(wildcard $(BUILD)/*.d)
