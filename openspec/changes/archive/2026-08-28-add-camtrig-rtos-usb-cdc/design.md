## Context

See `proposal.md` — Why. See `specs/camtrig-control-link/spec.md` for the behaviour contract.

The firmware being restructured is 801 lines across three files, with a stated and mechanically
enforced design: `no HAL, no CMSIS, no libc` (`firmware/README.md:30`, enforced by `-nostdlib
-fno-builtin -fno-common` and linking `-lgcc` alone), `No interrupts` (`README.md:96`,
`startup.c:4-5` — every vector except Reset traps), and `small enough to audit in one sitting`
(`hw.h:3-4`). **This change retires all of that deliberately.** The trade is auditability for
maintainability and for a USB stack that is already proven rather than one written from scratch;
D13 records what is lost so it is a decision on the record, not an erosion.

Board facts, verified against the WeAct MiniSTM32H7xx V1.2 schematic and against WeAct's own shipping
USB examples for this board rather than assumed:

| Fact | Value | How established |
|---|---|---|
| USB-C data pins | `PA11`/`PA12`, **AF10** | Schematic sheet `01-STM32H7xx`: `USB1_DN → PA11`, `USB1_DP → PA12` |
| Peripheral | **`USB_OTG_FS`** = `USB2_OTG_FS` @ `0x4008_0000` | `stm32h743xx.h`; WeAct `05-DCMI_UVC` uses `USB_OTG_FS`, `Device_Only`, `PCD_PHY_EMBEDDED` |
| VBUS sense | **not routed to the MCU** | Sheet `02-DC-DC` shows `VBUS` reaching only `D10`/`SB6` and the CC 5.1 kΩ pair — no MCU pin. WeAct sets `vbus_sensing_enable = DISABLE` |
| ID pin | unavailable — `PA10` is `USART1_RX` | current `main.c` `gpio_init()` muxes `PA9`/`PA10` to AF7 |
| USB 3.3 V domain | `PWR_CR3.USB33DEN` **only** | WeAct's working examples call `HAL_PWREx_EnableUSBVoltageDetector()` (= `USB33DEN`) and never `EnableUSBReg` (= `USBREGEN`) |
| Clock tree | HSE 25 MHz → PLL1 `M=5 N=96 P=2` → **SYSCLK 240 MHz**, HCLK 120 MHz, VOS2 | WeAct `04-SD_Test` `SystemClock_Config()` |
| USB 48 MHz | **PLL1Q**, `Q=10` → 480/10 | `UsbClockSelection = RCC_USBCLKSOURCE_PLL` |
| OTG_FS FIFO RAM | **320 words (1.25 kB)** total | WeAct allocates `RX 0x80`, `TX0 0x40`, `TX1 0x80` = 320 words exactly |
| Toolchain | `arm-none-eabi-gcc` 10.2.1; newlib-nano and `thumb/v7e-m+dp/hard` multilib present | on the build host |

## Goals / Non-Goals

**Goals:**

- A second command transport over the USB-C already fitted, carrying the *identical* protocol, with
  `USART1` unchanged and simultaneously live.
- A project a future maintainer can open, recognise, and modify with ordinary ST tooling.
- The waveform provably independent of the scheduler and of every interrupt now in play — a property
  that must get *stronger*, not weaker, for this restructure to be defensible.
- Every failure mode recoverable with the BOOT0 button and no debugger — and, after D16, most
  reflashes reachable without touching it at all.

**Non-Goals:**

- No USB host or OTG role-switching. Device-only; the board cannot sense VBUS or ID anyway.
- No in-application DFU class, MSC, or custom bootloader. The ROM bootloader keeps that job — D16
  only reboots *into* it, and adds no DFU implementation of its own.
- No change to the command protocol's grammar or to any datasheet-derived limit.
  (The `TIM1` channel mapping *was* originally a non-goal and is no longer — see D14.)
- No CubeIDE dependency. `make` and `dfu-util` remain the whole toolchain.
- No USB as a bulk data path. It carries a few bytes a minute.
- No D-cache/I-cache enablement, no DMA. Neither is needed, and both add coherency questions.

## Decisions

### D1. Standard CubeMX-shaped layout, but a `Makefile` build

Adopt the directory shape CubeMX generates — `Core/{Inc,Src}`, `Drivers/CMSIS`,
`Drivers/STM32H7xx_HAL_Driver`, `Middlewares/ST/STM32_USB_Device_Library`,
`Middlewares/Third_Party/FreeRTOS` — because that is what "standard STM32 project" means in practice
and what makes the result legible to anyone who has seen one before.

Keep the build a hand-maintained `Makefile` invoking `arm-none-eabi-gcc`, as CubeMX itself can emit.
The repo's existing workflow is `make` on an x86-64 host and `dfu-util` over the ROM bootloader;
requiring CubeIDE would regress that and add a GUI to a reproducible build.

Commit the `.ioc` alongside. It is not part of the build, but it is what lets a future maintainer
regenerate or extend the peripheral configuration instead of reverse-engineering it from
initialisation code — the main maintainability argument for this whole change, applied to itself.

### D2. One pinned STM32CubeH7 release as the single source

HAL, CMSIS device headers, `STM32_USB_Device_Library` (Core + CDC class) and FreeRTOS all come from
one STM32CubeH7 tag, vendored into the firmware tree with their licences, and the tag recorded in
`firmware/README.md`.

Mixing HAL minor versions against a USB middleware built for a different one is a genuine and
hard-to-diagnose failure class, so version consistency matters more than picking any particular
version. Vendoring rather than fetching at build time preserves what `firmware/README.md` promises:
the firmware builds on a machine with `arm-none-eabi-gcc`, `make`, and no network.

*Considered:* taking HAL from WeAct's SDK, since it is proven on this board. Rejected — WeAct ships
no CDC class, so the CDC files would have to come from a Cube release anyway, reintroducing exactly
the version mismatch above. WeAct's SDK is used as a *reference for board-specific values*
(D3/D4/D5), not as a source of code.

### D3. Adopt WeAct's board-proven clock tree — superseding D5

`add-imx296-hw-trigger` D5 chose HSE 25 MHz straight to SYSCLK with no PLL, to avoid
"PLL/VOS/flash-latency sequencing — the largest source of 'bricked on first boot' risk in bare-metal
H7 bring-up". **That rationale was specifically about hand-written sequencing**, which the HAL now
performs, so the decision no longer earns its cost.

Take the tree WeAct's own examples run on this board: PLL1 `M=5 N=96 P=2` → SYSCLK **240 MHz**,
HCLK 120 MHz, VOS2, and `PLLQ=10` → **PLL1Q = 48 MHz** for USB.

Consequences for the trigger, all favourable and all automatic:

- `TIM1` kernel clock rises from 25 MHz to **120 MHz** (APB2 `DIV1`, so timer clock = PCLK2). Since
  every limit is computed at runtime from `g_timer_hz`, **no arithmetic changes** — the existing code
  simply reads a different clock. This is why D5's "all timing is computed from a runtime
  `g_timer_hz`" was worth having.
- ⚠ **On `TIM1`, the faster clock bought no trigger resolution**, contrary to the obvious assumption:
  `div = ceil(pt / 65536)` means ARR's 16 bits — not the clock — set the step, ≈ `period / 65536`
  regardless (520 ns → 517 ns at 30 fps). The 5× finer tick was absorbed by a 5× larger prescaler.
  **D14 later removed that ceiling** by moving to 32-bit `TIM5`, where `div` is 1 and the step is one
  tick (8.33 ns). The clock increase only pays off in combination with the wider counter.
- The emitted period shifts very slightly (33333.040 µs → 33333.267 µs at the 30 fps default), because
  the prescaler/ARR pair lands on a different rounding. Irrelevant to synchronisation — all four
  cameras share the one counter — but it means the port is not bit-identical in emitted timing, and
  the bench comparison should expect that 227 ns rather than treat it as a regression.
- USB gets 48 MHz from PLL1Q with no second PLL and no `HSI48`/`CRS` machinery at all.
- The HSE-fails fallback that D5 added stays meaningful and must be preserved: if `HSERDY` never
  asserts, fall back to a PLL-off HSI path, record it, and report it in `status`. A cold-solder
  crystal must still degrade accuracy rather than brick the rig.

⚠ One number is **not** to be adopted on faith: WeAct pairs this tree with `FLASH_LATENCY_1` at
VOS2. Flash read latency is exactly the failure D5 warned about, so it must be checked against
RM0433's latency-vs-`VOS`-vs-AXI-clock table during implementation and corrected if the table
disagrees, rather than copied because it appears in shipping code.

### D4. `USB_OTG_FS`, device-only, VBUS sensing forced off

The board decides this, not preference. `PA11`/`PA12` are `OTG_FS` (`USB2`), and the pins that would
carry VBUS sense and ID are already `USART1`. In HAL terms: `PCD_SPEED_FULL`, `PCD_PHY_EMBEDDED`,
`vbus_sensing_enable = DISABLE`, `dma_enable = DISABLE`.

**`vbus_sensing_enable = DISABLE` is the single setting most likely to decide whether anything
enumerates.** On a board that does not wire VBUS to the MCU, leaving it enabled means the core never
sees a valid session, never asserts the D+ pull-up, and the host logs *nothing at all* — no failed
enumeration, no error, silence. This project already met that exact symptom from the other direction
when the firmware simply had no USB stack; it is indistinguishable from the host side, which is why
it is recorded here rather than left to the code.

FIFO allocation from the 320-word pool, following WeAct's proven split with room carved out for the
CDC notification endpoint:

| FIFO | Words | Serves |
|---|---|---|
| RX (shared) | 96 | EP0 OUT + EP1 OUT bulk |
| TX0 | 64 | EP0 IN control |
| TX1 | 96 | EP1 IN bulk |
| TX2 | 16 | EP2 IN interrupt (CDC notification) |
| | **272 / 320** | |

### D5. `PWR_CR3.USB33DEN` only, never `USBREGEN`

The H7's USB transceiver has a dedicated 3.3 V domain that is **off at reset**, and forgetting it
produces the same silent non-enumeration as D4 by a different route. Call
`HAL_PWREx_EnableUSBVoltageDetector()` (= `USB33DEN`) and not `HAL_PWREx_EnableUSBReg()`
(= `USBREGEN`): WeAct's shipping UVC and MSC examples for *this board* call the former and never the
latter, which establishes that `VDD33USB` is externally supplied here.

If enumeration fails on the bench, adding `USBREGEN` is the first thing to try — recorded as an open
question rather than pre-emptively set, since enabling the internal regulator onto an
externally-supplied rail is not obviously harmless.

### D6. FreeRTOS through CMSIS-RTOS v2, with the HAL timebase moved off SysTick

Use the CMSIS-RTOS v2 wrapper, as CubeMX generates. It is the layer a maintainer expects to find, and
it keeps the `.ioc` in D1 meaningful.

**Move the HAL timebase to `TIM6`.** By default `HAL_InitTick()` owns `SysTick`, which FreeRTOS also
requires; leaving both on it is the best-known way to make a CubeMX+FreeRTOS project fail in ways
that look like random hangs. `TIM6` is otherwise unused here and is what CubeMX itself recommends.

### D7. Interrupt priority discipline, stated once and explicitly

With HAL and an interrupt-driven USB stack, interrupt priorities stop being a formality:

- `HAL_Init()` sets `NVIC_PRIORITYGROUP_4` — all 4 bits pre-emption, no sub-priority. Keep it.
- Any ISR calling a FreeRTOS `...FromISR` API must have a numeric priority **≥**
  `configMAX_SYSCALL_INTERRUPT_PRIORITY`. `OTG_FS_IRQn` is such an ISR.
- `configASSERT` stays **enabled in the shipping build**. FreeRTOS's port-level assertions catch
  exactly this misconfiguration at the moment it happens rather than as a hang hours later, and this
  firmware has no debugger attached in normal use, so a loud early failure is worth its code size.

This is written down because it is the one class of bug that adopting HAL+FreeRTOS *introduces* which
the previous bare-metal design was structurally immune to.

### D8. Two tasks; USB handled by ST's stack on its own interrupt

| Task | Prio | Blocks on | Owns |
|---|---|---|---|
| `trig` | AboveNormal | `TIM1` update, via ISR → notification | pulse count, burst termination, LED heartbeat |
| `cli` | Normal | command queue | line assembly from both transports, command execution, replies |

USB is not a task: ST's stack runs from `OTG_FS_IRQHandler`, and `usbd_cdc_if.c`'s receive callback
posts assembled lines to the same queue `USART1` feeds. One consumer, two producers, one protocol.

`trig` sits above `cli` because a missed update miscounts a burst, and because command execution is
the only thing here that can take unbounded time while nothing waits on it.

Shared trigger state (`g_period_ns`, `g_pulse_ns[]`, `g_running`, `g_burst_left`) is guarded by one
mutex with priority inheritance. `g_burst_left` is the genuinely shared item — `cli` sets it, `trig`
decrements it — and is why a mutex exists rather than a convention.

**Nothing above can reach the waveform.** Once `PSC`/`ARR`/`CCRx` are written, `TIM1` emits edges
with no software involvement at all. Every task could stop, every interrupt could be masked, and the
four cameras would keep exposing in lockstep at the last commanded parameters. That is the property
that makes putting an RTOS and an interrupt-driven USB stack on a trigger generator defensible, and
it is why the modified `camera-hw-trigger` requirement now states it explicitly instead of leaving it
implied.

### D9. Hard float, `-mfloat-abi=hard -mfpu=fpv5-d16`

The current Makefile's `-mfloat-abi=soft` was justified by "nothing here uses floating point... the
ABI stays trivially correct". Under HAL that stops being free: the prebuilt HAL/USB middleware and
newlib-nano multilibs are selected by ABI, and the hard-float `thumb/v7e-m+dp/hard` multilib is the
one present and the one CubeMX targets for H743. Matching it avoids linking objects of mixed ABI —
a link-time or, worse, run-time failure. `configENABLE_FPU` follows.

### D10. newlib-nano, with `printf` kept out of the trigger path

Link `--specs=nano.specs --specs=nosys.specs` and provide the minimal `syscalls.c` stubs. Retarget
`_write` to the CLI's output sink so stray library output cannot vanish silently.

The existing hand-rolled `uart_putu()` integer formatting is **kept** rather than replaced by
`printf`. It is correct, allocation-free, stack-cheap, and already produces the exact reply strings
`j106-trigctl.py` parses. Pulling in `printf` would grow the image and put a variadic formatter on
the reply path for no behavioural gain. Float formatting is explicitly not enabled.

### D11. Reply routing via an output sink

Every `uart_puts()` call site in `handle()` becomes `out_puts()`, writing to a per-command sink
resolved from where the line arrived. Unsolicited output — the boot banner, `burst done` — goes to
both transports.

Echoing everything everywhere is fewer lines but corrupts the protocol: two attached hosts would each
see the other's replies interleaved with their own, and `j106-trigctl.py` parses `ok`/`err`
positionally. Since the spec requires concurrent transports, reply routing is not optional.

`CDC_Transmit_FS` returns `USBD_BUSY` while a previous IN transfer is in flight, so the USB sink
needs a small ring buffer drained on the CDC TX-complete callback. Ignoring that return is the
standard way CDC output silently truncates.

### D12. The trigger logic is ported, not rewritten

`pulse_for()`, `check_period()`, `tim1_program()`'s auto-prescaler, the datasheet constants
(`IMX296_1H_NS`, `TTGPD_H`, `TOFFSET_NS`, `MIN_LOW_NS`), the command parser and every reply string
move across with their behaviour and their comments intact. `TIM1` is configured through
`HAL_TIM_PWM` and the compare registers written with `__HAL_TIM_SET_COMPARE`, but the *arithmetic
that decides what to write is unchanged*.

This keeps the change reviewable where it matters: the safety-relevant reasoning — the limits that
refuse rather than clamp, the parking of `CCRx = 0` so a stop is never mid-pulse — is the part a
reviewer must still trust, and it should show up in the diff as a move, not as a rewrite.

### D13. What is knowingly lost

Recorded so this is a decision and not a drift:

- `hw.h`'s ~40 hand-declared registers, and with them the property that every register the firmware
  touches is visible in one file.
- A vector table where every entry but `Reset` traps, and the guarantee that no ISR exists to get
  wrong (`startup.c:4-5`).
- A linker script describing only bank 1 and DTCM, so "a stray symbol lands as a link error rather
  than in memory nothing initialises".
- `small enough to audit in one sitting` — the project's own code roughly doubles, and the vendored
  tree adds tens of thousands of lines that will not be read.

What is bought: a USB stack that is deployed on millions of devices instead of debugged from scratch
on a bench, a project shape any STM32 developer can pick up, and regeneratable peripheral
configuration. The trigger path itself (D12) stays small and auditable inside the larger tree.

### D14. The trigger moves to `TIM5` on `PA0`-`PA3` — superseding `add-imx296-hw-trigger` D6

D6 picked `TIM1_CH1..CH4` on `PE9/PE11/PE13/PE14`, reasoning that port E "is where this board leaves
pins free" and noting the TFT-LCD header as "not fitted". On this board the display **is** fitted, and
those pins are its connector: `PE11` = CS, `PE13` = WR_RS, `PE14` = MOSI, with `PE12` = SCK and
`PE10` = backlight. Confirmed twice — from the V1.2 schematic, and from WeAct's own `03-LCD_Test`
`.ioc`, which assigns exactly those signals.

So the conflict was never "display versus trigger". It was "display versus **TIM1**". `TIM5_CH1..CH4`
map to `PA0`/`PA1`/`PA2`/`PA3` (AF2), which this board leaves entirely unused, and moving there frees
`PE10`-`PE14` completely.

Two things follow, one of them unexpectedly good:

- **Resolution improves ~62x.** `TIM5` is a 32-bit counter; `TIM1` is 16-bit. The auto-prescaler
  `div = ceil(pt / ARR_max)` was pinned at 31-62 by the 16-bit limit, making the step ≈ period/65536
  (~517 ns at 30 fps) *regardless of clock*. With a 32-bit ARR `div` is 1 for every rate in range, so
  the step is one tick — **8.33 ns**. Measured on the board: `psc=0`, `arr=3999998`.
- **Every document naming the trigger pins is now wrong.** That is the real cost, and it is why this
  had to be done *now*: the optocouplers are not yet wired (README §7 — "blocked only on two wires"),
  so the change is free today and four resoldered channels tomorrow.

`TIM1` is not freed for nothing: it becomes the backlight PWM (D15), which is what WeAct's own example
uses it for.

*Rejected:* keeping `TIM1` and giving up the display. The board has an integrated panel; a rig that
must run with no host attached (the firmware's stated goal) benefits from being legible on its own.

### D15. Local display on the integrated ST7735, at the lowest priority

Vendor WeAct's `st7735.c` / `st7735_reg.c` / `font.h` unmodified and adapt only their `lcd.c` — whose
`LCD_Test()` drew a 260 kB logo bitmap and then blocked on the KEY button, neither of which belongs in
firmware whose job is keeping four cameras triggered.

The display task runs at **`osPriorityLow`**, below `cli`, and repaints twice a second, only the rows
that changed. This ordering is the whole point: the panel is a slow serial device, and **nothing in
the rig may wait on it**. The waveform is `TIM5` hardware, the command path sits above it, so an
unplugged, faulty or slow panel costs a stale screen and nothing more. Reads go through the same mutex
as the command path, so a half-applied change can never be displayed.

Backlight is `TIM1_CH2N` on `PE10` — available only because of D14.

### D16. Software entry to the ROM bootloader, via reset rather than a live jump

`dfu` writes a magic word to a `.noinit` RAM section (outside `_sbss.._ebss`, so startup does not
zero it) and calls `NVIC_SystemReset()`. The very top of `main()` — before `HAL_Init()`, before any
clock or peripheral — checks the magic and jumps. Coming through a reset means there is no live
FreeRTOS or USB stack to tear down correctly.

**The bootloader vector is at `0x1FF09800`, established by measurement.** RM0433 Rev 7 Table 9 names
`0x1FF0 0000` as the boot address, and jumping there fails: read from user code it returns all zeros.
Both candidates were logged to `status` (`sysmem_sp` / `sysmem_alt`) and the second returned
`0x240044b0` — a stack pointer in AXI SRAM, which is what a bootloader vector looks like.

Two details that are not optional:

- **The MSP switch and the branch must be one asm block.** `__set_MSP(sp); boot();` looks correct and
  is not: at `-Os` the function pointer is a stack-spilled local, so reading it after the stack has
  moved yields garbage. This cost a board excursion before it was found.
- **The vector is sanity-checked before use** — SP in DTCM or AXI SRAM, PC odd and inside system
  memory. A bad vector then boots the application normally instead of taking the board off the bus,
  which is the difference between a failed convenience feature and a BOOT0 recovery.

## Risks / Trade-offs

- **Interrupt-priority misconfiguration between HAL and FreeRTOS** — the failure class D7 exists to
  prevent, and the one this restructure genuinely introduces. → `configASSERT` enabled in the
  shipping build; priority grouping set once by `HAL_Init` and never changed; `OTG_FS_IRQn` priority
  set explicitly and commented against `configMAX_SYSCALL_INTERRUPT_PRIORITY`.
- **`FLASH_LATENCY_1` at VOS2 copied from WeAct may be wrong** — and wrong flash latency is the
  classic silent brick. → Verify against RM0433's table during implementation (D3); this is a task,
  not an assumption.
- **The USB stack still cannot be exercised until it is on hardware** — much reduced versus a
  hand-rolled stack, but not zero. → `USART1` is untouched and stays live, so a non-enumerating USB
  build leaves the rig exactly as capable as today. Staged bring-up isolates it.
- **Auditability loss (D13).** → Not mitigable, only bounded and recorded. The vendored tree is
  unmodified upstream and separately licensed; the trigger arithmetic is unchanged and reviewable in
  isolation.
- **Stack overflow is a new failure mode**, and HAL/USB frames are far larger than the old code's. →
  `configCHECK_FOR_STACK_OVERFLOW = 2` with a hook that parks the trigger and flashes the LED
  distinctly; high-water marks reported by `status`.
- **CDC output can silently truncate** if `USBD_BUSY` is ignored (D11). → ring buffer + TX-complete
  callback, and a dropped-byte counter in `status` so truncation is visible rather than mysterious.
- **VBUS-powered operation couples the trigger to the host.** If the TX2 hosts the board, a TX2
  reboot or suspend drops VBUS, resets the STM32, and it silently returns to compiled-in defaults —
  losing tuned `pol`/`skew`/`exp`. This board already shows AON-PWM state not surviving SC7, so this
  is not hypothetical. → Documented in `WIRING.md`; `j106-trigctl.py` gains a note to re-apply
  settings on connect; powering from its own 5 V with `SB6` open keeps USB data-only.
- **`0483:df11` DFU and CDC can never be present at once.** → Inherent: they are different programs.
  Documented as a mode distinction rather than a limitation.

## Migration Plan

Staged deliberately, so a failure at any step leaves a working rig and identifies its own cause:

1. **Keep a known-good `camtrig.bin`.** Copy the current `build/camtrig.bin` to
   `hw-trigger/firmware/known-good/` before anything else. This is the rollback, and it is a file,
   not a rebuild.
2. **HAL + clock tree only.** Restructure the project, bring up the D3 clock tree, port `TIM1` and
   `USART1`, no FreeRTOS and no USB. Flash; confirm over `USART1` that the waveform is unchanged,
   `status` reports the new `timer_hz`, and `burst` still terminates.
3. **Add FreeRTOS.** Tasks, mutex, `TIM6` timebase, priorities. Re-confirm the same checks. This
   isolates every RTOS-integration failure from every USB failure.
4. **Add USB.** `PWR` and clock first (D3/D5), then `PCD`/device init (D4), then the CDC class.
   Confirm `lsusb`, then `/dev/ttyACM0`, then the protocol behaving identically on both transports
   including reply routing with two hosts attached.
5. **Verify the invariant that justifies the change:** with the trigger running, confirm on a scope
   or by re-running `tools/j106-sync-check.py` that period and pulse width are unchanged from the
   pre-restructure build, under command load on both transports.

**Rollback** at any point: hold BOOT0, tap NRST, `dfu-util -a 0 -s 0x08000000:leave -D
known-good/camtrig.bin`. The ROM bootloader is silicon and cannot be bricked by anything here, so no
state leaves the board unrecoverable — matching the reversibility discipline this project applies to
`extlinux` LABELs.

## Open Questions

- **`USBREGEN` as a fallback.** If the device does not enumerate with `USB33DEN` alone, whether
  `USBREGEN` is also required on this board. One register write at bring-up; changes no spec, no
  structure, no task.
- **Which host owns the USB-C in the finished rig** — the TX2 (one cable, but couples trigger power
  to TX2 power) or a separate 5 V supply with USB for data only. Affects `WIRING.md` guidance and
  nothing in the firmware.
- **Whether `cli` should emit `status` unprompted on USB connect**, so a host attaching mid-run learns
  the current parameters without asking. Additive; deferred until the coupling question is settled.
