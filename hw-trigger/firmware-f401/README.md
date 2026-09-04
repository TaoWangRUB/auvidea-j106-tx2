# `camtrig-f401` — IMX296 trigger generator, STM32F401

Replacement for the [WeAct MiniSTM32H7xx firmware](../firmware/), running on a
**WeAct Studio MiniSTM32F4x1** ("Black Pill") with **STM32F401CEU6**.

Same architecture as the H7 build — **HAL + FreeRTOS + USB CDC** — so the two
stay comparable. Verified working on hardware 2026-08-31.

## Why this exists

The H7 board failed on 2026-08-31. Measured with everything disconnected:

| Probe | Reading | Meaning |
|---|---|---|
| `5V` (P2-42) → GND | 15 MΩ | input side intact |
| `3V3` (P2-41) → GND | **0.8 Ω** | **dead short on the 3.3 V rail** |

The LDO was current-limiting into that short (hence the hot chip), the 3.3 V
rail never came up, so the MCU never ran — which is why the board did not
enumerate, said nothing on UART, and could not be reached over SWD.

## Verified on hardware

```
clock=hse25-pll84        timer_hz=84000000
running=1  period_us=33333  fps_milli=30000
ch1..4_exposure_us=5000  pulse_ns=4985740  ccr=418802
psc=0  arr=2799998
usb_ready=1  cmds_dropped=0
stack_free_trig=1912  stack_free_cli=3832
```

`clock=hse25-pll84` confirms the 25 MHz crystal started rather than the HSI
fallback — worth checking after any change, because HSI is ~1% accurate and
that error lands directly on the frame rate.

## What carries over from the H7 unchanged

**The wiring needs no change.** Two mappings line up exactly:

| Function | H7 | F401 |
|---|---|---|
| Trigger | `TIM5_CH1..4` → `PA0`–`PA3` | `TIM2_CH1..4` → **same pins** (AF1) |
| Console | `USART1` → `PA9`/`PA10` | **same pins** (AF7) |

So the harness in [`WIRING.md` §4.1](../WIRING.md#41-trigger--mcu--4-cameras-required-2-nets)
and the J22 link in §4.2 are unchanged, and `tools/j106-trigctl.py` works
against either board.

**Timing resolution is effectively identical.** TIM2 on the F401 is a 32-bit
counter like the H7's TIM5, so the prescaler stays at 0 and one tick is 11.9 ns
(the H7 managed 8.33 ns at 120 MHz). All four channels are compare outputs on
one counter, so the rising edges are the same hardware event — the measured
1.0 µs inter-camera skew is a property of that sharing and survives the move.

**It enumerates as `0483:5740`**, the same VID:PID as the H7.

## Garmin LIDAR-Lite ranger (added 2026-09-04)

`Core/Src/ranger.c` adds `range` and a `lidar` diagnostic family on **PB6 (SCL) / PB7 (SDA)**,
which were free — the trigger lives entirely on port A plus `PC13`. Wiring, pull-ups and the
bulk-cap requirement: [`WIRING.md` §4.5](../WIRING.md#45-garmin-lidar-lite-ranger--mcu--sensor-optional-4-nets).

```
range          -> range_cm=85 pulses=1673
```

Readings are stamped with the trigger's pulse counter, which is what makes them joinable to a
frame with no timebase offset. The stamp is per *command*, so all samples in a `range <n>` burst
share one value — use `range` alone when the stamp matters.

Two things worth knowing before touching this code:

- **Do not twist SDA and SCL together as a pair.** Doing so NAKed every hardware-I2C address
  (`HAL_I2C_ERROR_AF`) while a bit-banged master on the same two pins worked perfectly — SCL edges
  couple onto SDA and the F4 samples SDA at a fixed short delay after the SCL edge, right in the
  spike. A bus-speed sweep that changes nothing (100 → 5 kHz behaved identically here) is evidence
  *for* this, not against it: slowing the bus does not move the edge-to-sample delay.
- **Read with a STOP, not a repeated START.** `HAL_I2C_Mem_Read()` addresses this sensor fine and
  then returns all-zero data with no error. `reg_read()` uses `Master_Transmit` + `Master_Receive`,
  matching what Garmin's own Arduino library does.

And one HAL trap worth remembering generally:

- **`HAL_I2C_Init()` calls `HAL_I2C_MspInit()` only out of the `RESET` state.** Re-initialising an
  already-`READY` peripheral silently skips the pin setup, so after the bit-bang path has taken
  PB6/PB7 as GPIO, a bare re-`Init` configures the block perfectly and leaves it wired to nothing —
  every register reads back correct and every transfer fails. `i2c_setup()` calls `HAL_I2C_DeInit()`
  first for this reason.
