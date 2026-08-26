# camtrig — IMX296 trigger generator firmware

Firmware for a **WeAct MiniSTM32H7xx** (STM32H743VIT6). Generates the waveform that
hardware-synchronises the four IMX296 on the J106/TX2 rig, exposes a command interface over **USB
CDC-ACM and USART1**, and shows live state on the board's **integrated 0.96" TFT**.

Wiring, resistor values and bring-up order: [`../WIRING.md`](../WIRING.md).
Vendored dependencies and their pinned versions: [`VENDOR.md`](VENDOR.md).

## What it does

`TIM5_CH1..CH4` on **`PA0` / `PA1` / `PA2` / `PA3`** (header P2‑17/18/19/20), one channel per camera,
all four on **one 32-bit counter** — so the frame start is identical across cameras by construction
while each channel can carry its own exposure.

The camera modules' trigger inputs are **optocoupler LEDs isolated from module ground** (measured —
see [`../WIRING.md`](../WIRING.md) §3 — a TLP281), so each pin sources ~10.3 mA straight into an LED
whose cathode returns to *this* board's ground. **No external resistor**: the module carries its own
`R4 = 200 Ω`. In Fast Trigger mode the asserted pulse width *is* the exposure
(`t_exp = t_pulse + 14.26 µs`), so the pulse is produced entirely by timer hardware — never by a
software loop, and never by anything the scheduler or the USB stack can delay.

It starts triggering on power-up with compiled-in defaults (**30 fps, 5 ms**), so the rig works with
**no host attached**. The links only exist to change parameters at runtime.

> ⚠ **The trigger pins changed.** Earlier firmware used `TIM1` on `PE9/PE11/PE13/PE14`. Those pins
> are the on-board TFT-LCD connector; on a board with the display fitted they cannot be the trigger.
> See `../WIRING.md` §4.1.

## Build

```bash
make            # -> build/camtrig.bin
```

Needs `arm-none-eabi-gcc` and `make` — no CubeIDE, no network. Standard STM32 project layout:
`Core/{Inc,Src}`, `Drivers/` (CMSIS + HAL + the ST7735 panel driver), `Middlewares/` (ST USB Device
Library + FreeRTOS). The project's own code builds with `-Wall -Wextra -Werror`; the vendored tree
does not, because it is unmodified upstream.

## Flash

**No BOOT0 press needed.** If the board is running and enumerated, `make flash` asks it to reboot
into the ROM bootloader, waits for it to appear, and flashes:

```bash
make flash      # reboots into DFU by itself, flashes, and leaves
make verify     # flash, read back, and compare byte-for-byte
make restore    # put back the pre-restructure bare-metal build
```

If the board is wedged or running firmware without the `dfu` command, fall back to the button: hold
**BOOT0**, tap **NRST**, release BOOT0, then `make flash`.

`make flash` exits **74** on success. That is ST's DfuSe bootloader resetting on the leave request
without answering the final `get_status`; the recipe tolerates exactly that and instead checks the
device left the bus.

### How the software DFU entry works

`dfu` writes a magic word to a `.noinit` RAM section (outside `_sbss.._ebss`, so startup does not
zero it) and calls `NVIC_SystemReset()`. The top of `main()` checks the magic **before** `HAL_Init()`
and jumps — coming through a reset means there is no live FreeRTOS or USB stack to tear down.

**The bootloader vector is at `0x1FF09800`.** RM0433 Rev 7 Table 9 names `0x1FF0 0000` as the boot
address, and jumping there fails: read from user code it returns all zeros. `0x1FF09800` returns a
valid AXI SRAM stack pointer. Both were logged to `status` (`sysmem_sp` / `sysmem_alt`) and settled
on the board rather than argued from the manual.

## Control interface

Identical protocol on **both** transports, simultaneously:

| Transport | Port |
|---|---|
| USB CDC-ACM | `/dev/ttyACM*` — `0483:5740`, serial number derived from the MCU UID |
| `USART1` | `PA9` (TX, P1‑27) / `PA10` (RX, P1‑26), 115200 8N1 |

A reply goes back to the transport that issued the command; unsolicited output (boot banner,
`burst done`) goes to both. Driven by [`tools/j106-trigctl.py`](../../tools/j106-trigctl.py).

| Command | Effect |
|---|---|
| `fps <v>` | frame rate, e.g. `30` or `59.94` |
| `period <us>` | frame period directly |
| `exp <us>` | exposure, all four cameras |
| `exp <ch> <us>` | exposure for one camera, `ch` = 1..4 |
| `pol <0\|1>` | `1` (default) = the pulse turns the LED **on** |
| `skew <ns>` | the optocoupler's on/off delay asymmetry, removed from the pulse |
| `start` / `stop` | `stop` parks every channel unasserted, never mid-pulse |
| `burst <n>` | emit `n` pulses then stop |
| `status` | `key=value` report |
| `dfu` | reboot into the ROM bootloader for reflashing |
| `help` | command list |

`fps`, `period` and `skew` **preserve per-channel exposures**. They used not to — all three reset
every channel to channel 1's value, which mattered most for `skew`, the command the optocoupler
bring-up procedure depends on.

## Display

The integrated 0.96" ST7735 (160×80) shows rate, exposure, pulse count, polarity/skew, and clock +
link state, so the rig is legible with no host attached. `*` after the exposure means the four
channels differ.

It runs at the **lowest** task priority and repaints twice a second, only the rows that changed.
Nothing waits on it: the waveform is `TIM5` hardware and the command path sits above it, so an
unplugged or faulty panel costs a stale screen and nothing else. Values are read under the same mutex
as command execution, so a half-applied change is never rendered.

Panel driver (`st7735.c`, `st7735_reg.c`, `font.h`) is WeAct's, vendored unmodified; only their
`lcd.c` is adapted — its `LCD_Test()` drew a 260 kB logo and then blocked on the KEY button.

## Design notes

**Clock: HSE 25 MHz → PLL1 → SYSCLK 240 MHz, HCLK/AXI 120 MHz, VOS1, 1 flash wait state.**
`FLASH_LATENCY_1` at **VOS1** is deliberate and is *not* what the vendor reference code does — it
pairs VOS2 with 1 WS, which RM0433 Rev 7 Table 17 says is wrong: at VOS2, 120 MHz AXI needs **2**
wait states. Under-provisioned flash wait states are the classic silent brick. If `HSERDY` never
asserts, the firmware falls back to a PLL-off HSI path, reports it in `status`, and USB is
unavailable — a cold-solder crystal degrades the rig instead of bricking it.

**Resolution is one timer tick, 8.33 ns.** `TIM5` is 32-bit, so the auto-prescaler
`div = ceil(pt / ARR_max)` stays at 1. On 16-bit `TIM1` the same code was pinned near
`period / 65536` (~517 ns at 30 fps) *regardless of clock speed* — the counter width was the
constraint, not the clock.

**Limits are datasheet constants, not baked ticks.** `IMX296_1H_NS`, `TTGPD_H`, `TOFFSET_NS` and
`MIN_LOW_NS` sit at the top of `Core/Inc/camtrig.h` in physical units. Out-of-range requests are
**refused with a reason**, never silently clamped.

**Three tasks, and none of them can move an edge.** `trig` (AboveNormal) counts pulses and ends
bursts; `cli` (Normal) executes commands; `lcd` (Low) draws. USB runs from `OTG_FS_IRQHandler`. Once
`PSC`/`ARR`/`CCRx` are written, `TIM5` emits edges with no software involvement at all — every task
could stop and the four cameras would keep exposing in lockstep.

**Faults are visible without a debugger.** `panic_blink(n)` flashes the PE3 LED: 1 = `Error_Handler`,
2 = scheduler failed to start, 3 = stack overflow, 4 = malloc failed, 5 = `configASSERT`,
6 = HardFault, 7 = MemManage, 8 = BusFault, 9 = UsageFault, 10 = NMI. The hooks deliberately **leave
the trigger running**: a camera in Fast Trigger mode stalls when `XTRIG` stops, and `TIM5`'s
registers are unaffected by RAM corruption, so parking would turn a control-path bug into a capture
outage.

## Files

| File | |
|---|---|
| `Core/Src/camtrig.c` | trigger core: limits, TIM programming, command handling |
| `Core/Src/cli.c` | transports, line assembly, reply routing |
| `Core/Src/lcd_ui.c` | what the panel shows |
| `Core/Src/main.c` | clock, GPIO, TIM5, SPI4, backlight, tasks, bootloader entry |
| `Core/Src/usbd_*.c` | USB CDC-ACM device |
| `Core/Inc/camtrig.h` | datasheet constants and the pin map |
| `known-good/` | pre-restructure rollback image |
