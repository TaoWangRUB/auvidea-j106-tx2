## Why

The J106/TX2 rig now carries four IMX296 global-shutter modules (ports C–F, `2-001a` `2-0018`
`7-001a` `7-0018`). Each free-runs on its own on-board oscillator, so the four frame clocks drift
against each other without bound — exactly the failure a global shutter is supposed to remove. For
the BEV/VIO work the four views must be exposed at the same instant, which the sensor supports only
through its `XTRIG` pin. Nothing in the current build drives that pin.

The TX2 cannot drive it well. On tegra186 only a handful of pads can mux to a PWM function
(`gp_pwm6_pl6`, `gp_pwm7_pl7`, `uart5_rx_px5`, a few DIS/SEN pads); on this carrier the only one
broken out is `FAN_PWM`, which `pwm-fan` already owns through an inverting open-drain MOSFET. A
live check of `pinmux-pins` confirms **no pin on the board is muxed to PWM at all**. Any TX2-sourced
trigger would therefore be a software-toggled GPIO — and because IMX296 Fast Trigger mode makes the
`XTRIG` **low pulse width itself the exposure time**, scheduler jitter would land directly on image
brightness. A WeAct MiniSTM32H7 already on hand generates the same pulse from a hardware timer with
~4 ns resolution and zero jitter.

## What Changes

- Add an **external hardware trigger path**: a WeAct MiniSTM32H7xx (STM32H743) drives the four
  camera modules' trigger inputs from `TIM1_CH1..CH4` (`PE9/PE11/PE13/PE14`), one channel per
  camera on a single shared counter. The modules' `XTR+`/`XTR−` pads turned out on measurement to
  be an **optocoupler LED isolated from module ground**, so each channel supplies ~10 mA through a
  series resistor and no ground is shared with the cameras.
- Add **firmware** (`hw-trigger/firmware/`): bare-metal STM32H7 trigger generator — four PWM
  channels on one counter, auto-prescaler, frame-rate and exposure limits derived from the
  datasheet, runtime `pol`/`skew` corrections for the optocoupler, and a line-based serial command
  interface on `USART1`.
- Add a **wiring document** with the pin-level hardware diagram, the measurements that identified
  the input as isolated, resistor sizing, and the bring-up/verification procedure. No adapter PCB.
- Add **driver patch `0003`**: put the IMX296 into Fast Trigger mode (`TRIGEN`, `LOWLAGTRG`,
  `SYNCSEL` = Hi-Z) via the datasheet-mandated standby transition, selected at runtime by a module
  parameter so no DTB rebuild or reflash is needed.
- **BREAKING (in trigger mode only)**: exposure stops being a sensor register and becomes the
  trigger pulse width, so the driver's exposure control becomes advisory and Argus cannot drive it.
  Free-running mode is unchanged and remains the default.
- Add **host tools**: `j106-trigctl.py` (drive the trigger generator) and `j106-sync-check.py`
  (measure inter-camera frame skew from V4L2 buffer timestamps — the acceptance test).

## Capabilities

### New Capabilities
- `camera-hw-trigger`: externally generated, hardware-timed frame trigger shared by every
  IMX296 on the rig — waveform contract, matching the drive to the module's actual input type,
  sensor mode entry/exit, runtime control, and the measurable synchronisation guarantee.

### Modified Capabilities
- `imx296-camera`: the "Exposure and gain control" requirement changes — when the sensor is in
  external-trigger mode its shutter registers no longer determine exposure, so the driver must stop
  claiming to program them and must not let the frame-rate control corrupt readout timing.

## Impact

- **New**: `hw-trigger/WIRING.md`, `hw-trigger/images/`, `hw-trigger/firmware/` (STM32H7,
  `arm-none-eabi-gcc`, flashed with `dfu-util` over the ROM bootloader).
- **New**: `patches/0003-imx296-external-trigger-j106.patch` — kernel `Image` rebuild required;
  deployment stays reversible through an `extlinux` LABEL as always.
- **New**: `tools/j106-trigctl.py`, `tools/j106-sync-check.py`.
- **Modified**: `patches/0002-imx296-tegracam-j106.patch` stays as-is; `0003` applies on top.
- **Modified**: `README.md` (new stage section + status), `CLAUDE.md` (repo layout).
- **Unchanged**: device tree. This change touches no `.dtsi`, so the deployed DTB is unaffected.
- **Hardware**: 5 wires (4 LED anodes + 1 common cathode return) and 4× 220 Ω resistors. No shared
  ground with the cameras. Optionally 3 more wires for the serial link to the M110 `J22` header.
