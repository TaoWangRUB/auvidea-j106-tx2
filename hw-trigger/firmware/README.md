# camtrig — IMX296 trigger generator firmware

Bare-metal STM32H743 firmware for a **WeAct MiniSTM32H7xx**. Generates the trigger waveform that
hardware-synchronises the four IMX296 on the J106/TX2 rig.

Wiring, resistor values and bring-up order: [`../WIRING.md`](../WIRING.md).

## What it does

`TIM1_CH1..CH4` on **`PE9` / `PE11` / `PE13` / `PE14`** (header P2‑32/34/36/37), one channel per
camera, all four on **one counter** — so the frame start is identical across cameras by
construction while each channel can carry its own exposure.

The camera modules' trigger inputs are **optocoupler LEDs isolated from module ground** (measured —
see [`../WIRING.md`](../WIRING.md) §3 — a TLP281), so each pin sources ~10.3 mA straight into an LED
whose cathode returns to *this* board's ground. **No external resistor**: the module carries its own
`R4 = 200 Ω`, which at 3.3 V already limits the current below the 20 mA the vendor recommends. In the IMX296's Fast Trigger mode the asserted
pulse width *is* the exposure (`t_exp = t_pulse + 14.26 µs`), so the pulse is produced entirely by
timer hardware — never by a software loop.

It starts triggering on power-up with compiled-in defaults (**30 fps, 5 ms**), so the rig works with
**no host attached**. The serial link only exists to change parameters at runtime.

## Build

```bash
make            # -> build/camtrig.bin   (~5 kB)
```

Needs `arm-none-eabi-gcc` only. Builds with `-Wall -Wextra -Werror`; no HAL, no CMSIS, no
libc — `src/hw.h` declares the ~40 registers this touches and nothing else.

## Flash

No debugger required — the STM32 ROM bootloader provides USB DFU.

1. Hold **BOOT0**, tap **NRST**, release **BOOT0**.
2. `lsusb` should show `0483:df11 STMicroelectronics STM Device in DFU Mode`.
3. `make flash` (or `dfu-util -a 0 -s 0x08000000:leave -D build/camtrig.bin`).

`:leave` resets into the new firmware.

## Serial protocol

`USART1` on **`PA9`** (TX, P1‑27) / **`PA10`** (RX, P1‑26), **115200 8N1**, line-based ASCII.
Replies end `ok` or `err <reason>`. Driven by [`tools/j106-trigctl.py`](../../tools/j106-trigctl.py).

| Command | Effect |
|---|---|
| `fps <v>` | frame rate, e.g. `30` or `59.94` |
| `period <us>` | frame period directly |
| `exp <us>` | exposure, all four cameras |
| `exp <ch> <us>` | exposure for one camera, `ch` = 1..4 |
| `pol <0\|1>` | `1` (default) = the pulse turns the LED **on**. Use `0` if the module asserts on LED *off* |
| `skew <ns>` | the optocoupler's on/off delay asymmetry, removed from the pulse alongside the sensor's 14.26 µs |
| `start` / `stop` | `stop` parks every channel unasserted, never mid-pulse |
| `burst <n>` | emit `n` pulses then stop |
| `status` | `key=value` report |
| `help` | command list |

## Design notes

**Clock: HSE 25 MHz straight to SYSCLK, no PLL.** Crystal accuracy (±20 ppm) instead of
HSI's ±1 %, and none of the PLL/VOS/flash-latency sequencing that is the usual way a
bare-metal H7 bring-up fails on first power-on. If `HSERDY` never asserts the firmware falls
back to HSI (64 MHz) and says so in `status` — a cold-solder crystal degrades accuracy
instead of bricking the rig. Every timing figure is computed from a runtime `g_timer_hz`, so
both paths are correct.

**`FLASH_ACR` and `PWR` VOS are never written.** Both target clocks are at or below the reset
clock (HSI 64 MHz), so the reset wait-state setting is already valid — over-provisioned, never
under. Under-provisioned flash wait states are the classic first-bring-up brick, and not
writing the register removes that failure mode entirely.

**Auto-prescaler.** `TIM1`'s `ARR` is 16-bit, so the prescaler is chosen per request as
`div = ceil(period_ticks / 65536)` — the finest resolution the requested rate allows
(≈280 ns at 60 fps), while still reaching below 1 fps.

**Limits are datasheet constants, not baked ticks.** `IMX296_1H_NS`, `TTGPD_H`, `TOFFSET_NS`
and `MIN_LOW_NS` sit at the top of `main.c` in physical units so they can be checked against
`IMX296LQR-C_Fulldatasheet_Awin.pdf` directly. Out-of-range requests are **refused with a
reason**, not silently clamped into a waveform that stalls capture:

- period shorter than `tTGPD` (1126 H = 16.6815 ms → **max 59.95 fps**)
- exposure below ~14.31 µs (`tTGSE` + `tOFFSET`)
- exposure leaving less than 1 ms of readout margin before the next trigger

**Two runtime knobs for the optocoupler.** Neither the *sense* of the pulse (does driving the LED
assert `XTRIG` or release it?) nor its transport delay can be determined from this side of an
isolation barrier. Guess-reflash-guess would turn a two-second experiment into a build cycle during
exactly the bring-up step where iteration matters, so both are serial commands. The `pol` default is
idle-LED-off — the state that cannot leave a sensor held inside an exposure.

**Pins idle with a pull-down**, so a board in reset or unplugged leaves the optos dark.

**No interrupts.** The waveform is hardware; the command loop polls. There is no ISR to get
wrong, and no timing that depends on firmware responsiveness.

## Files

| File | |
|---|---|
| `src/main.c` | clock, GPIO, TIM1, USART1, limits, command loop |
| `src/startup.c` | vector table, reset handler, `.data`/`.bss` init |
| `src/hw.h` | the registers this firmware touches |
| `stm32h743.ld` | 1 MB flash bank 1 + 128 KB DTCM |
