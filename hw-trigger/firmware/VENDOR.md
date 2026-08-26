# Vendored dependencies

Everything below is **unmodified upstream code**, committed into this tree rather than fetched at
build time. That is deliberate: `README.md` promises the firmware builds with `arm-none-eabi-gcc` and
`make` on a machine with no network and no `git submodule update`, and this project is expected to be
rebuildable years from now.

All four components are pinned to the versions that ship together in **STM32CubeH7 `v1.13.0`**.
Version consistency matters more here than currency — HAL built against one minor version and USB
middleware built against another is a real and hard-to-diagnose failure class (design D2).

| Component | Upstream | Pinned commit |
|---|---|---|
| CMSIS device (STM32H7xx) | `STMicroelectronics/cmsis_device_h7` | `8f922cdc7cc6de2344e75ddd657889f4ff761790` |
| STM32H7xx HAL driver | `STMicroelectronics/stm32h7xx_hal_driver` | `a1996eed9172b59887bafaaa0ea1816ea14d48b5` |
| USB Device Library | `STMicroelectronics/stm32-mw-usb-device` | `0b06460a43c30670a764cb7e58c5a7918e789f6e` |
| FreeRTOS | `STMicroelectronics/stm32-mw-freertos` | `5fe3a380e5eadb6ce0a5149725210c3fe70d1c15` |

CMSIS **core** headers (`Drivers/CMSIS/Include`, ARM's `core_cm7.h` and friends) are not a submodule
upstream; they were taken from the `STM32CubeH7` repository itself at tag `v1.13.0`.

## What was taken, and what was left out

Subsets, not whole repositories — the goal is a tree where everything present is something this
firmware actually compiles or includes.

| Path | Taken | Omitted |
|---|---|---|
| `Drivers/CMSIS/Device/ST/STM32H7xx/Include` | `stm32h7xx.h`, `stm32h743xx.h`, `system_stm32h7xx.h` | the other ~30 H7 variant headers (~40 MB) |
| `Drivers/CMSIS/Device/ST/STM32H7xx/Source/Templates` | `system_stm32h7xx.c`, `gcc/startup_stm32h743xx.s` | other toolchains' startup, other variants |
| `Drivers/CMSIS/Include` | all core headers | — |
| `Drivers/STM32H7xx_HAL_Driver/Inc` | all headers (they cross-include; `stm32h7xx_hal_conf.h` gates what compiles) | — |
| `Drivers/STM32H7xx_HAL_Driver/Src` | `hal`, `rcc(_ex)`, `gpio`, `pwr(_ex)`, `cortex`, `tim(_ex)`, `uart(_ex)`, `dma(_ex)`, `mdma`, `exti`, `pcd(_ex)`, `ll_usb` | every peripheral this board does not use |
| `Middlewares/ST/STM32_USB_Device_Library` | `Core`, `Class/CDC` | AUDIO, HID, MSC, DFU, VIDEO, MTP, … |
| `Middlewares/Third_Party/FreeRTOS/Source` | kernel `.c`, `include/`, `portable/GCC/ARM_CM7/r0p1`, `portable/MemMang/heap_4.c`, `CMSIS_RTOS_V2` | every other port and heap scheme |

`ARM_CM7/r0p1` is the port CubeMX selects for all STM32H7; it carries the Cortex-M7 errata
workaround and is the conservative choice.

## Licences

Upstream `LICENSE.md` / `License.md` files are kept in place beside the code they cover — BSD-3-Clause
for the ST components, MIT for FreeRTOS. Nothing here is relicensed, and nothing is modified; if a
patch ever becomes necessary it must be recorded in this file rather than applied silently, because a
silent local edit to a vendored tree is indistinguishable from upstream until it breaks.

## Refreshing

Re-fetch each repository at the commit above (or at a newer *consistent* set from one STM32CubeH7
tag) and re-copy the same subsets. Then rebuild and re-run the bench checks in
`../../openspec/changes/add-camtrig-rtos-usb-cdc/tasks.md` §8 — in particular §8.6, which confirms the
trigger waveform is unchanged.
