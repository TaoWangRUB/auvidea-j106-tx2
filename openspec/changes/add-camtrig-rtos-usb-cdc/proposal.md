## Why

The trigger generator has exactly one control transport: `USART1` on `PA9`/`PA10`. Reaching it needs
either a USB-serial dongle or three wires to the M110 `J22` header whose `/dev/ttyTHS*` mapping is
not derivable from the running tree — `add-imx296-hw-trigger` D8 accepted that cost to avoid writing
a USB device stack. Meanwhile the WeAct board's USB-C connector is already wired to the MCU
(`PA11`/`PA12` → `USB_OTG_FS`) and already carries the board's power and its DFU flashing path; at
runtime it enumerates nothing at all.

That matters now because the control link is on the critical path for opto bring-up. D10 made pulse
sense (`pol`) and transport delay (`skew`) runtime settings precisely because neither can be
determined from the driving side of an isolation barrier — they have to be found by experiment, with
the cameras streaming. Requiring a second cable and a dongle to do that, when the cable already
plugged in could carry it, is friction in exactly the wrong place. A CDC-ACM link also sidesteps the
level-domain question that wiring `USART1` to the TX2 would raise: USB is USB, with no 1.8 V/3.3 V
measurement gate of the kind `WIRING.md` §3 imposes on the trigger pads.

The firmware's existing bare-metal construction is what made this expensive. Writing a USB device
stack by hand — the objection D8 raised, correctly — is several hundred lines that cannot be
exercised until hardware is in front of someone. Adopting the standard STM32 project structure
removes that objection rather than arguing with it: ST's USB Device Library with its CDC class is
mature, widely deployed, and already proven on this exact board by WeAct's own shipping examples.

## What Changes

- **Restructure `hw-trigger/firmware` as a standard STM32 project**: CMSIS device headers, the
  STM32H7xx HAL, ST's `STM32_USB_Device_Library` (CDC class), FreeRTOS via CMSIS-RTOS v2, and
  newlib-nano — all vendored from one pinned STM32CubeH7 release. The build stays a `Makefile` driving
  `arm-none-eabi-gcc`, and flashing stays `dfu-util`; CubeIDE is not required.
- **BREAKING (firmware-internal, no external contract):** this retires the firmware's stated design —
  `no HAL, no CMSIS, no libc` (`firmware/README.md:30`) and `No interrupts` (`README.md:96`,
  `startup.c:4-5`). Hand-written `src/hw.h`, the hand-rolled `src/startup.c` vector table, and the
  minimal linker script are replaced by their CMSIS/ST equivalents. Maintainability is bought at the
  cost of `small enough to audit in one sitting` (`hw.h:3-4`).
- Add a **USB CDC-ACM command transport**: `USB_OTG_FS` device-only on `PA11`/`PA12`, presenting a
  virtual serial port. The **same line protocol** (`fps`, `exp`, `pol`, `skew`, `start`, `stop`,
  `burst`, `status`, `help`) runs over it unchanged, so `tools/j106-trigctl.py` needs only a
  different `--port`.
- **`USART1` keeps working.** Both transports are live simultaneously; a reply returns to whichever
  transport issued the command. The serial link remains the fallback when USB is unavailable, and the
  only link that works with no host at all.
- **Supersede D5** of `add-imx296-hw-trigger` ("HSE 25 MHz direct to SYSCLK, no PLL"). Adopt the
  clock tree WeAct's own examples use on this board — PLL1 `M=5 N=96 P=2` → **SYSCLK 240 MHz**, with
  **PLL1Q = 48 MHz** feeding USB. D5's rationale was avoiding hand-written PLL/VOS/flash-latency
  sequencing, which is exactly what the HAL now performs.
- **Supersede D8** ("`USART1`, not USB CDC") and **D10's hand-rolled premise**. Both are recorded as
  reversed, with the reasons they were right at the time.
- Restructure the single polled `main()` loop into FreeRTOS tasks: trigger servicing and command
  execution, with USB handled by ST's stack on the `OTG_FS` interrupt.
- Update `hw-trigger/WIRING.md` and `hw-trigger/firmware/README.md` for the second transport, the
  DFU-vs-CDC mode distinction, and the power-coupling caveat when the TX2 is the USB host.

### Added during implementation

Not in the original scope. Recorded here rather than folded in silently, because two of them change
things this proposal originally promised not to touch.

- **BREAKING — the trigger moves from `TIM1` on `PE9/PE11/PE13/PE14` to `TIM5` on `PA0`-`PA3`.**
  Those port E pins are the board's integrated TFT-LCD connector, so the original pinout made the
  display permanently unusable. `TIM5`'s channels land on pins this board leaves free. Two
  consequences: **every document naming the trigger pins is now wrong**, and — because `TIM5` is a
  32-bit counter where `TIM1` is 16-bit — the auto-prescaler settles at 1 and pulse resolution
  improves from ~517 ns to **8.33 ns**. Free only because the optocouplers are not yet wired; after
  wiring it would mean resoldering four channels.
- Add a **local status display** on the integrated 0.96" ST7735 (160x80): SPI4 on `PE12`/`PE14`,
  `PE11`/`PE13` as CS/DC, `PE10` backlight via `TIM1_CH2N`. Shows rate, exposure, pulse count,
  polarity/skew and clock+link state, so the rig is legible with no host attached. Reachable only
  because of the `TIM5` move.
- Add a **`dfu` command and hands-free flashing**: the firmware reboots itself into the ROM
  bootloader, so `make flash` needs no BOOT0 press. The bootloader vector is at `0x1FF09800`,
  established by reading both candidates on the board — `0x1FF00000`, which RM0433 Table 9 names as
  the boot address, reads as all zeros from user code.
- **Fix a pre-existing defect** found by reading `status` back over the new link: `fps`, `period` and
  `skew` each reset every channel's exposure to channel 1's — worst where it matters most, since
  `skew` is the command the optocoupler bring-up procedure depends on.

## Capabilities

### New Capabilities

- `camtrig-control-link`: the trigger source's command interface as a contract independent of any
  one wire — multiple concurrent transports carrying one identical protocol, replies returning to
  the transport that issued the command, the device identity a USB host enumerates, and the
  guarantee that the trigger keeps running whether or not any host is attached or connected.

- `camtrig-local-display`: the trigger source's own panel as an observer that never affects what it
  observes — what must be legible without a host, that displayed values are never internally
  inconsistent, and that the trigger outranks the display for both pins and scheduling.

### Modified Capabilities

- `camera-hw-trigger`: the "Timing comes from hardware, not software" requirement currently forbids
  reconstructing the waveform from "software timing loops or operating-system timers", written when
  the only operating system in sight was the host's. With a scheduler and interrupt-driven middleware
  now running on the trigger source itself, that requirement must explicitly extend to them, and must
  state that trigger output survives firmware faults on the control path.

## Impact

- **New**: `hw-trigger/firmware/Drivers/` (CMSIS + STM32H7xx HAL), `Middlewares/ST/STM32_USB_Device_Library/`
  (Core + CDC class), `Middlewares/Third_Party/FreeRTOS/`, `Core/Src/usb_device.c`, `usbd_desc.c`,
  `usbd_cdc_if.c`, `Core/Src/{usb_otg,tim,usart,gpio}.c`, `stm32h7xx_it.c`, `syscalls.c`,
  `FreeRTOSConfig.h`, `stm32h7xx_hal_conf.h`, ST's `startup_stm32h743xx.s`.
- **Replaced**: `src/hw.h` (→ CMSIS `stm32h743xx.h`), `src/startup.c` (→ ST startup + `system_stm32h7xx.c`),
  `stm32h743.ld` (→ standard H743 linker script), `Makefile` (HAL/USB/FreeRTOS sources, include
  paths, `-mfloat-abi=hard -mfpu=fpv5-d16`, `--specs=nano.specs`).
- **Ported, not rewritten**: the trigger logic itself. `pulse_for()`, `check_period()`, the
  auto-prescaler, the datasheet constants, the command parser and every reply string move across
  unchanged in behaviour. This change replaces the substrate under the trigger, not the trigger.
- **Modified docs**: `hw-trigger/WIRING.md`, `hw-trigger/firmware/README.md`, `README.md` §5 Stage 8
  and §7, `CLAUDE.md` repo layout.
- **Unchanged**: every datasheet-derived limit, the command
  protocol's syntax and semantics, `tools/j106-trigctl.py` (beyond `--port`), every kernel patch, and
  the device tree. Nothing deployed on the TX2 changes.
- **Changed after all**: the trigger's timer and pins — see *Added during implementation*.
- **Hardware**: no new components. The USB-C cable already needed for DFU becomes the control link.
  **But the four trigger conductors now land on `PA0`-`PA3`, not `PE9/PE11/PE13/PE14`** — a wiring
  change, free only because nothing is soldered yet.
- **Dependencies**: STM32CubeH7 (BSD-3-Clause / ST Ultimate Liberty), FreeRTOS (MIT), newlib-nano —
  vendored and pinned so the firmware still builds with `arm-none-eabi-gcc` and `make` alone.
- **Size**: the binary grows from ~5 kB to an expected ~60–80 kB. Flash is 2 MB, so headroom is not a
  concern; auditability is, and that is the tradeoff being accepted deliberately in exchange for
  maintainability and a USB stack that does not have to be debugged from scratch.
