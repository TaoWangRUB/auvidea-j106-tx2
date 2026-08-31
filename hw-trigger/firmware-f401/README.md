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

## Board facts (from the V3.1 schematic)

Taken from [WeActStudio.MiniSTM32F4x1](https://github.com/WeActStudio/WeActStudio.MiniSTM32F4x1)
`Hardware/MiniF4x1Cx_V31 SchDoc.pdf`, not assumed:

| Fact | Consequence |
|---|---|
| **25 MHz HSE**, 8 pF | the PLL config below |
| **`PA0` is the User KEY**: `PA0 —[R1 330 Ω]— SW1 — GND`, **no capacitor** | benign, see below |
| **`PC13` blue LED**: `3V3 → R5 1.5 K → LED → PC13` | **active low** |
| `PA11`/`PA12` = USB `DN`/`DP` | what the CDC driver uses |
| `PA9`/`PA10` plain header pins | USART1 free; VBUS *not* sensed on `PA9` |

### `PA0` doubles as the User KEY — and that is fine

`TIM2_CH1` shares `PA0` with the user button. Two reasons it does not matter:
**no debounce capacitor on `PA0`** (the 0.1 µF nearby is C3, on **NRST**), so
the trigger edge is unaffected; and **R1 = 330 Ω in series**, so pressing KEY
while `PA0` drives high sources ~10 mA, inside the per-pin limit. Worst case is
a slight droop on camera 1. Do not press it while measuring skew.

## Clock

HSE 25 MHz → PLL → **84 MHz** (the F401 ceiling), with an exact 48 MHz for USB:

```
VCO_in  = 25 / PLLM 25  = 1 MHz
VCO_out = 1 * PLLN 336  = 336 MHz
SYSCLK  = 336 / PLLP 4  = 84 MHz
USB     = 336 / PLLQ 7  = 48 MHz exactly
```

These are WeAct's own numbers from their `03-CDC_Standalone` reference project.
TIM2 is on APB1: with APB1 prescaled by 2 the timer clock doubles back to
84 MHz, which is what camtrig's timing maths assumes.

If the crystal fails to start the firmware falls back to HSI (16 MHz, PLLM 16,
same 84 MHz) — the trigger keeps running, but **USB cannot work to spec** and
the frame rate is only as good as a ~1% oscillator. `status` reports which.

## Where the code comes from

| Component | Source |
|---|---|
| HAL, CMSIS, USB Device Library, `usbd_conf.c` | WeAct's `SDK/STM32F401CEU6/HAL/03-CDC_Standalone` — validated for this exact board |
| FreeRTOS V10.3.1 + CMSIS-RTOS2 | the H7 tree, same version |
| FreeRTOS **ARM_CM4F** port | FreeRTOS-Kernel `V10.3.1-kernel-only` |
| `camtrig.c`, `cli.c`, `main.c`, `usbd_cdc_if.c` | ported from the H7 build |

### F4-vs-H7 differences that bite

Recorded because each one is silent rather than loud:

- **`SYSMEM_BASE` is `0x1FFF0000`** (RM0368 Table 3), not the H7's measured
  `0x1FF09800`. Carrying the H7 value over sends `dfu` into unmapped space —
  the board leaves the bus and BOOT0 becomes the only way back, which is the
  exact thing `dfu` exists to avoid.
- **`Sof_enable` must be `ENABLE`.** WeAct's stock config disables it, but
  `cdc_ready()` infers cable presence from SOF timestamps because
  `vbus_sensing_enable` must be `DISABLE` on this board. With SOF off,
  `cdc_ready()` is permanently false and **every byte of USB output is silently
  dropped** — the port enumerates and answers nothing.
- **UART registers**: F4 uses `DR`, not `TDR`/`RDR`; there is no `UART_CLEAR_OREF`
  (ORE/FE/NE clear by reading `SR` then `DR`); no per-USART clock mux.
- **HAL timebase is TIM11**, since the F401 has no TIM6 and FreeRTOS owns SysTick.
- The **`TIM2_IRQHandler` rename** is load-bearing: a stale `TIM5_IRQHandler`
  leaves the vector pointing at `Default_Handler`, which silently kills pulse
  counting, burst termination and the LED while the waveform still runs.

## Build

```bash
make            # -> build/camtrig-f401.bin
```

Needs `arm-none-eabi-gcc`. Current size, against 512 KB flash / 96 KB RAM:

```
   text    data     bss     dec
  33944     508   28860   63312
```

## Flash

The F401 ROM speaks USB DFU, so **no BOOT0 press is needed** when the board is
running this firmware:

```bash
make flash                    # asks over $(PORT), then writes
make backup                   # read the existing flash out first
make flash-swd                # ST-Link alternative
```

`make flash` sends `dfu` to the console, waits for `0483:df11`, then writes.
Verified working over USB CDC: the board entered DFU 500 ms after the command.
If it is not running this firmware, hold **BOOT0**, tap **NRST**, release.

> Expected quirk: ST's DfuSe bootloader resets on the leave request and never
> answers the final `get_status`, so **`dfu-util` prints "Error during download
> get_status" and exits 74 on a completely successful flash**. Judge success by
> `0483:df11` *leaving* the bus.

## Use

Starts triggering on power-up with the compiled-in defaults (**30.000 fps,
5000 µs, `pol 1`**), so the rig works with no host attached.

```bash
tools/j106-trigctl.py --port /dev/ttyACM0 status    # USB CDC
tools/j106-trigctl.py --port /dev/ttyTHS1 status    # UART via M110 J22
```

Both transports are live at once; a reply goes back to the transport the
command arrived on. Commands: `help`, `status`, `start`, `stop`, `fps`,
`period`, `exp`, `pol`, `skew`, `burst`, `dfu`. There is no `bl` — no display.

Two refusals, never a silent clamp: period below `tTGPD` (16.681 ms), and
exposure leaving under 1 ms of readout margin. A rejected command leaves the
timer exactly as it was, so a bad command cannot stop the cameras mid-run.

### ⚠ ModemManager grabs `/dev/ttyACM0`

On the TX2, ModemManager probes new ACM ports with AT commands. Symptoms are a
"Device or resource busy" on open, and `err unknown command` replies that are
answers to *its* traffic, not yours. Retrying works; to fix it properly, add a
udev rule marking `0483:5740` as `ID_MM_DEVICE_IGNORE=1`.

## Status LED (`PC13`)

| LED | Meaning |
|---|---|
| dark | no power, or not running |
| solid | alive, not triggering |
| blinking | triggering |

## Rollback

`known-good-baremetal/` holds the earlier dependency-free build (two sources,
no HAL/RTOS/USB) and the binary that was verified triggering correctly. It has
no USB CDC and is UART-only, but it is one `dfu-util` command away.

## ⚠ Before wiring a replacement board

The H7 most likely died from **5 V landing on the 3.3 V rail** — `3V3` and `5V`
are adjacent holes, and the signature (3V3 shorted, 5 V pristine) matches damage
arriving through the 3.3 V domain rather than VBUS. Check the power pin twice,
and put **220 Ω–1 kΩ in series** on the trigger and UART lines so any future
back-feed is clamp-limited. `PA0`–`PA3` are **not** 5 V-tolerant on the F401 —
they are ADC-capable pins, outside the FT group.
