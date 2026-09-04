# Vendored dependencies

Everything below is **unmodified upstream code**, committed into this tree rather than fetched at
build time — the same policy as [`../firmware/VENDOR.md`](../firmware/VENDOR.md), and for the same
reason: `make` must work on a machine with no network and no `git submodule update`, years from now.

The one local change is `USB_DEVICE/Target/usbd_conf.c`, noted at the bottom.

| Component | Upstream | Taken from |
|---|---|---|
| CMSIS core headers | ARM CMSIS (`core_cm4.h` and friends) | WeAct's `03-CDC_Standalone` project |
| CMSIS device (STM32F4xx) | `STMicroelectronics/cmsis_device_f4` | WeAct's `03-CDC_Standalone` project |
| STM32F4xx HAL driver | `STMicroelectronics/stm32f4xx_hal_driver` | WeAct's `03-CDC_Standalone` project |
| USB Device Library (Core + CDC) | `STMicroelectronics/stm32-mw-usb-device` | WeAct's `03-CDC_Standalone` project |
| FreeRTOS kernel **V10.3.1** + CMSIS-RTOS2 | `STMicroelectronics/stm32-mw-freertos` | copied from [`../firmware/`](../firmware/), same version as the H7 build |
| FreeRTOS **ARM_CM4F** port | `FreeRTOS/FreeRTOS-Kernel` tag `V10.3.1-kernel-only` | `portable/GCC/ARM_CM4F/{port.c,portmacro.h}` |
| HAL **I2C** driver | `STMicroelectronics/stm32f4xx_hal_driver` tag **`v1.7.8`** | ST's repository directly — see below |

## The I2C driver came from ST, not from WeAct

`Drivers/STM32F4xx_HAL_Driver/{Src,Inc}/stm32f4xx_hal_i2c{,_ex}.{c,h}` are the only HAL files here
that are **not** from WeAct's `03-CDC_Standalone` project: that project does not use I2C, so it does
not ship them. The tag `v1.7.8` was chosen to match the version already vendored, which the tree
states itself:

```c
/* Drivers/STM32F4xx_HAL_Driver/Src/stm32f4xx_hal.c */
#define __STM32F4xx_HAL_VERSION_MAIN   (0x01U)
#define __STM32F4xx_HAL_VERSION_SUB1   (0x07U)
#define __STM32F4xx_HAL_VERSION_SUB2   (0x08U)   /* -> 1.7.8 */
```

Check that macro before adding any further HAL module and take the matching tag — mixing HAL
versions inside one tree works right up until it silently does not. Both files are unmodified.

Nothing in the `Makefile` needed changing: `C_SOURCES` already globs
`$(wildcard Drivers/STM32F4xx_HAL_Driver/Src/*.c)` and `$(wildcard Core/Src/*.c)`, so dropping the
files in and adding `Core/Src/ranger.c` was enough. `stm32f4xx_hal_i2c.h` includes
`stm32f4xx_hal_i2c_ex.h`, so both pairs are required.

This driver is the ranger's default transport. It did not work at first — see the transport note at
the top of [`Core/Src/ranger.c`](Core/Src/ranger.c) for the two faults involved — but with the
cabling corrected and `Mem_Read()` replaced by `Master_Transmit` + `Master_Receive` it runs clean.
The bit-banged master is kept alongside it as a fallback and a diagnostic.

## Why the HAL and USB stack came from WeAct rather than from ST directly

[`WeActStudio/WeActStudio.MiniSTM32F4x1`](https://github.com/WeActStudio/WeActStudio.MiniSTM32F4x1),
`SDK/STM32F401CEU6/HAL/03-CDC_Standalone` — the board vendor's own USB-CDC reference project for
**this exact board**. Using it means the clock tree, the OTG_FS FIFO layout and the VBUS-sensing
choice are the vendor's validated values rather than ours:

```
PLLM 25, PLLN 336, PLLP /4, PLLQ 7   ->  SYSCLK 84 MHz, USB exactly 48 MHz
HAL_PCDEx_SetRxFiFo 0x80, TxFiFo0 0x40, TxFiFo1 0x80      (320 words total)
vbus_sensing_enable = DISABLE        (VBUS is not wired to the sense pin)
```

An earlier attempt wrote the USB device stack by hand against the OTG_FS registers. It never
enumerated, and cost four flash cycles. The vendor stack worked on the first flash. That is the
whole justification for vendoring rather than writing.

The FreeRTOS **kernel** is deliberately the same V10.3.1 the H7 build uses, so the two firmwares
stay comparable; only the CPU port differs (ARM_CM4F here, ARM_CM7 there). The `ARM_CM4F` port must
be built **hard-float** (`-mfpu=fpv4-sp-d16 -mfloat-abi=hard`) to match — it saves and restores
S16–S31 on context switch.

## Local modification

`USB_DEVICE/Target/usbd_conf.c` is WeAct's file with **one change**:

```c
hpcd_USB_OTG_FS.Init.Sof_enable = ENABLE;   /* was DISABLE */
g_last_sof = HAL_GetTick();                 /* stamped in HAL_PCD_SOFCallback */
```

`cdc_ready()` infers cable presence from SOF timestamps, because `vbus_sensing_enable` must be
`DISABLE` on this board and the core therefore cannot detect a detach. With SOF off, `cdc_ready()`
is permanently false and **every byte of USB output is silently dropped** — the port enumerates,
accepts commands and executes them, and answers nothing. See `../firmware/Core/Src/usbd_conf.c`,
which carries the same change for the same reason.
