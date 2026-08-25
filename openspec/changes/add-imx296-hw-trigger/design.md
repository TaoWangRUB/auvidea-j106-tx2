## Context

See `proposal.md` — Why, for the motivation and for the evidence that the TX2 has no usable
hardware-PWM pin on this carrier.

State this design has to work with:

- Four IMX296 on ports C–F (`2-001a`, `2-0018`, `7-001a`, `7-0018`) → `/dev/video0..3`, streaming
  BG10 1456×1088 at ~60 fps, each **self-clocked** from its own on-module oscillator (INCK 54 MHz,
  measured). Nothing on the carrier fans a clock or a sync signal to them.
- The J106 CSI connector is a 22-pin RPi-Zero-pinout FFC. Its pins 8 and 9 are **not connected**,
  and pin 6 is the shared `CAM0_RST`. **There is no spare conductor in the camera cable**, so a
  trigger cannot reach the sensors through the CSI harness — it must go to the breakout's own pads
  (`3V3 / MAS / XVS / XHS / XTR+ / XTR−`).
- Trigger source hardware is fixed by the user: a WeAct MiniSTM32H7xx (STM32H743VIT6). Confirmed
  from its V12 schematic: `PE9` is free on header **P2‑32** (`PE10`–`PE14` are the on-board
  TFT-LCD, `PE0/1/4/5/6` are DCMI, LED is `PE3`, USB is `PA11/PA12`, HSE is a 25 MHz crystal).
- Deployment discipline for this repo: every kernel change ships as a numbered patch and is deployed
  behind a new `extlinux` LABEL with the previous one kept as fallback.

Datasheet constants this design is built on (`IMX296LQR-C_Fulldatasheet_Awin.pdf`):

| Quantity | Value | Source |
|---|---|---|
| 1 H (all-pixel) | `HMAX`/74.25 MHz = 1100/74.25 MHz = **14.8148 µs** | HMAX is in units of an internal 74.25 MHz reference, *independent of INCK* |
| `tTGPD` fast trigger, all-pixel | **1126 H = 16.6815 ms** → max **59.95 fps** | Fast Trigger parameter list |
| `tTGSE` min pulse | 0.05 µs | Fast Trigger parameter list |
| `tOFFSET` | **14.26 µs**, added to the low pulse | `t_exp = t_XTRIG_low + 14.26 µs` |
| XTRIG electrical | OVDD = **1.8 V**; VIH 0.8·OVDD = 1.44 V; VIL 0.2·OVDD = 0.36 V; **abs max input 3.3 V** | DC characteristics |

## Goals / Non-Goals

**Goals:**

- One hardware-timed trigger net driving all four IMX296 so their exposures coincide.
- A wiring definition an installer can follow with a soldering iron and a multimeter — no PCB.
- Runtime switching between free-running and triggered, without touching the DTB.
- A host-side measurement that *proves* synchronisation rather than assuming it.

**Non-Goals:**

- No adapter PCB (explicitly declined; a wiring guide instead).
- No auto-exposure loop over the trigger. Exposure is set explicitly; closing an AE loop through
  the trigger source is future work.
- No per-camera exposure. One shared net means one exposure for all four, by construction.
- No absolute time discipline (GNSS PPS, PTP). The trigger source is a free-running crystal.
- No Argus/ISP integration. Argus cannot control exposure through a wire it does not know about;
  triggered operation targets raw V4L2 capture.
- No change to the device tree, and therefore no change to the deployed DTB.

## Decisions

### D1. Fast Trigger mode (master), not Sequential Trigger mode (slave)

The IMX296 offers two triggered modes and they differ far more than their names suggest.

| | Sequential Trigger | **Fast Trigger** |
|---|---|---|
| `XMASTER` | High → **slave** | Low → **master** |
| External signals required | `XVS` **and** `XHS` **and** `XTRIG` | **`XTRIG` only** |
| `XHS` rate the source must generate | 67.5 kHz | — (sensor generates its own) |
| Exposure granularity | 1 H = 14.8 µs | ~0.05 µs |
| Exposure start | delayed 2–3 H after the edge | immediate on `XTRIG` fall |

Chosen: **Fast Trigger**. It needs a single low-rate net instead of three, one of which would have
been a 67.5 kHz clock fanned over flying leads to four boards. It also keeps `XMASTER` low — which
is how the modules are strapped *today*, proven by the fact that they free-run at 60 fps at all. So
the `MAS` pad is left untouched and there is no strapping change to get wrong.

Consequence carried into the specs: `TRIGEN=1`, `LOWLAGTRG=1`, and the transition **must** be made
through a standby cycle (the datasheet is explicit that fast-trigger mode changes go via standby).

**Cross-checked against a production implementation.** Raspberry Pi's shipping `imx296.c`
(`rpi-6.6.y`) implements exactly this mode, and its sequence is adopted here verbatim:

```
CTRL00    = 0                   /* leave standby        */   0x3000
usleep(2000..5000)              /* settle               */
CTRL0B    = TRIGEN              /* trigger mode         */   0x300b bit0
LOWLAGTRG = FAST                /* fast trigger         */   0x30ae bit0
CTRL0A    = 0                   /* XMSTA: master start  */   0x300a bit0
```

The trigger registers are written after standby is released but **before** master operation starts —
i.e. the mode change rides on the standby cycle the stream start already performs, rather than
needing a standby round-trip of its own. Stream-off is the mirror image (`XMSTA` stop, then
`STANDBY`).

That same driver also settles the control-surface question: it exposes precisely
`module_param(trigger_mode, int, 0644)` with `0=default, 1=XTRIG`. D3 below independently arrived at
the same design; matching the name and semantics means anyone who has used the Raspberry Pi GS
camera already knows this knob.

### D2. `SYNCSEL` → Hi-Z, written as a whole byte

In master mode `XVS`/`XHS` are **outputs**. Four modules whose `XVS` pads sit next to the trigger
wiring are four push-pull drivers waiting to be shorted together by a wiring mistake. Setting
`SYNCSEL` to Hi-Z makes the failure mode of miswiring `XVS` "no signal" instead of "contention", for
one register write.

**Write the whole byte, never the bit field.** The Raspberry Pi driver's constants are
`SYNCSEL_NORMAL = 0xc0` and `SYNCSEL_HIZ = 0xf0` — bits `[5:4]` carry the documented `0h` normal /
`3h` Hi-Z selection, but bits `[7:6]` must be held at `11`. A read-modify-write of just `[5:4]`, or
a naive `0x30`, would clear reserved bits the datasheet does not describe. This is the one place
where the datasheet alone would have produced a subtly wrong driver.

Note that Raspberry Pi *defines* those constants without writing them — leaving `SYNCSEL` at its
reset default. Setting Hi-Z is therefore a deliberate deviation, and a small one: `3h` is a
documented setting, the write only occurs in trigger mode, and it is reverted (`0xc0`) on the way
back to free-running.

Alternative considered: leave them driving and use one module's `XVS` as a frame-start monitor.
Rejected for now — it is a bring-up convenience that adds a live hazard to the shipped
configuration. The wiring doc notes it as an optional scope probe point instead.

### D3. One shared net, therefore one shared exposure — and a module parameter, not a DT property

The user chose a single fan-out net. Because Fast Trigger makes pulse width *be* exposure, that
decision propagates: all four cameras necessarily share one exposure, and all four must be in the
same mode at the same time. There is no meaningful per-sensor configuration to express.

So the mode selector is a **driver module parameter** (`imx296.trigger_mode`), read at
`start_streaming`, not a per-node device-tree property:

- It matches the topology — one global switch for one global net.
- It needs **no DTB rebuild and no reflash**. Switching is `echo 1 > /sys/module/imx296/parameters/trigger_mode`
  then restarting capture, which makes the free-running/triggered A/B comparison a two-second
  operation instead of a boot cycle. That comparison *is* the acceptance test (see D7).
- The device tree is the one artifact in this repo whose breakage costs a UART boot-menu recovery.
  Not touching it is worth a lot.

Default is `0` (free-running), so a board with no trigger wired behaves exactly as it does today.

### D4. Pin the frame-length register in trigger mode

There is a known, documented trap in this driver family: with `override_enable=1` tegracam
re-applies the `frame_rate` control at every stream start, that control sits at its device-tree
*minimum* (2 fps), `VMAX` becomes 33750, and VI dies with `PXL_SOF syncpt timeout` for the rest of
the boot. In trigger mode frame timing comes from `XTRIG`, so `VMAX` has no business tracking a
frame-rate control at all. The driver holds it at the all-pixel value (1118) whenever trigger mode
is active. This neutralises the trap in exactly the mode where it would otherwise be most confusing
to diagnose.

### D5. Clocking: HSE 25 MHz direct, no PLL, with an HSI fallback

The firmware runs `SYSCLK` from the board's 25 MHz crystal **without** the PLL.

- Crystal accuracy (±20 ppm) instead of HSI's ±1%, for free.
- No PLL/VOS/flash-latency sequencing — the largest source of "bricked on first boot" risk in
  bare-metal H7 bring-up. At 25 MHz, VOS3 with 0 wait states is unambiguously safe.
- 25 MHz gives a 40 ns tick; after the auto-prescaler (below) resolution at 60 fps is ~280 ns, four
  orders of magnitude finer than the exposures in use.
- If `HSERDY` does not assert within a timeout the firmware falls back to HSI (64 MHz) and records
  which clock it is on. All timing is computed from a runtime `g_timer_hz`, so both paths are
  correct, and `status` reports the source. A cold-solder crystal degrades accuracy instead of
  bricking the rig.

Rejected: 480 MHz via PLL. Nothing here needs the speed, and it multiplies the ways first power-on
can fail silently.

### D6. `TIM1_CH1` on `PE9`, inverted PWM; auto-prescaler

`TIM1_CH1` in PWM mode 1 with `CCER.CC1P = 1` gives idle-high, active-low — the exact `XTRIG`
shape — with the pulse produced entirely by hardware. `ARR` sets the period, `CCR1` the exposure.

`TIM1`'s `ARR` is 16-bit, so the prescaler is chosen at runtime:
`div = ceil(period_ticks / 65536)`, `PSC = div − 1`. This keeps the finest resolution the requested
rate allows and extends the reachable range down past 1 fps.

Rejected: `TIM2` (32-bit, no prescaler juggling). Its channels are on `PA0/PA1/PA5/PA15`; `PE9` is
the pin that is unambiguously free on this board *and* physically adjacent to the `3V3`/`GND` pins
on header P2, which matters when the deliverable is hand-wiring.

Stopping cleanly (spec: "Trigger stops cleanly") is `CCR1 = 0` then disabling the timer, so the pin
is parked high rather than frozen mid-pulse.

### D7. Verification by timestamp spread across four `/dev/video*`

`j106-sync-check.py` opens all four nodes, streams them concurrently, and compares the V4L2 buffer
timestamps frame-index by frame-index. Free-running, the spread between the earliest and latest
camera walks steadily as the four oscillators drift apart; triggered, it stays bounded by the VI/DMA
completion spread. The check runs identically in both modes, so the two runs are directly comparable
and no separate instrumentation is needed.

This is deliberately a *host-side* measurement using only timestamps the kernel already produces —
no scope, no extra wiring, and it works over SSH.

### D8. Serial link: `USART1` on `PA9`/`PA10`, not USB CDC

The command interface is a plain line-based ASCII protocol on `USART1` at 115200.

Rejected: USB CDC to a TX2 host port. It is the nicer cable, but it requires a full USB device stack
in the firmware — several hundred lines that cannot be tested until the hardware is in front of
someone, for a link that carries a handful of bytes per minute. The ROM bootloader still gives USB
DFU for flashing, so USB is not lost, only unused at runtime.

The serial link is also **optional**: the firmware boots with a compiled-in default rate and
exposure and starts triggering on its own. The rig works with two wires (`XTRIG`, `GND`) and no host
link at all; the link only exists to change parameters at runtime.

Where the TX2 end lands is left to bring-up: the M110 `J22` header carries a 3.3 V TTL UART, but
which `/dev/ttyTHS*` it maps to is not derivable from the running tree (no UART pin is claimed in
`pinmux-pins`), so the wiring doc gives a loopback identification procedure and a USB-serial dongle
as the zero-ambiguity fallback. `j106-trigctl.py` takes `--port`, so either works.

### D9. Level translation is a documented gate, not an assumption

`XTRIG` is a 1.8 V input rated to a 3.3 V absolute maximum. Whether the breakout's `XTR+` pad is
that raw input or something already buffered **cannot be determined from the photograph**, and
guessing wrong in the permissive direction risks the sensors. The design therefore makes the
measurement a required step with two documented outcomes:

What the vendor documentation does settle (Inno-Maker `CAM-IMX296RAW` manual, §4):

- The pads are **single-ended**, not differential: "the external trigger (denoted on the board as
  **XTR（Trig+）, GND(Trig-)**)". So `XTR−` is a ground return, and the `+`/`−` labelling does not
  imply a differential receiver.
- The topology chosen here is the vendor's own: "**Multiple cameras can be connected to the same
  pulse**, allowing for an alternative way to synchronize two cameras."
- Their reference trigger source is a **Raspberry Pi GPIO** (`gpioset gpiochip0 23=…`), which is
  3.3 V push-pull — evidence that a breakout of this family tolerates a 3.3 V drive on `XTR`.
- Their exposure formula is identical to the datasheet's: "the exposure time is equal to the low
  pulse-width time plus an additional 14.26 µs … a PWM frequency of 30 Hz will lead to a framerate
  of 30 frames per second."

That makes the 3.3 V-tolerant outcome the *likely* one, but not the confirmed one: the module in
this rig carries pads (`MAS`, `XVS`, `XHS`) the Inno-Maker board does not, so it is not necessarily
the same board, and a bare `XTRIG` pin sits at its absolute maximum under a 3.3 V drive. The
measurement stays mandatory; it is now expected to take one reading and confirm Case A.

- **3.3 V-tolerant** → direct wire + 33 Ω series at the source.
- **Raw 1.8 V** → one 820 Ω / 1.0 kΩ divider (→ 1.813 V, comfortably above the 1.44 V VIH), shared
  by all four high-impedance inputs. Source impedance 451 Ω; against ~150 pF of stubs and wiring
  that is a ~150 ns edge — symmetric on both edges, so it cancels out of the pulse width to first
  order and is irrelevant beside a ≥50 µs exposure.

## Risks / Trade-offs

- **Driving 3.3 V into a raw 1.8 V `XTRIG` would exceed the sensor's absolute maximum** → the
  measurement gate (D9) is a spec requirement, not advice; the wiring doc leads with it and states
  the failure explicitly.
- **`XTR+`/`XTR−` may be a differential pair rather than signal + return** → the first bring-up task
  is a continuity/voltage identification of all six pads before anything is connected; if they turn
  out to be differential, the single-ended plan needs a receiver and the task list says so.
- **Argus exposure control silently stops working in trigger mode** → the driver logs the mode and
  the exposure ownership once per stream, and the spec requires the active mode to be discoverable.
  Triggered operation is scoped to raw V4L2.
- **Firmware cannot be tested until the board is wired** → the risk is contained by keeping the
  clock tree trivial (D5), computing every limit from datasheet constants at runtime, and making
  `status` report clock source, period, pulse width and pulse count, so a scope is not needed to
  tell whether the firmware is doing what it claims.
- **HSI fallback changes the frame rate by up to 1%** → tolerable (sync is unaffected; only absolute
  rate moves) and reported by `status` so it cannot go unnoticed.
- **All four cameras share one exposure** → accepted consequence of the chosen topology. Moving to
  `TIM1_CH1..CH4` on four nets restores per-camera exposure later with no change to the sensor-side
  design; the firmware's parameter model keeps that door open.
- **A trigger stall leaves the sensors waiting** → the sensors simply stop producing frames; the
  host recovers by clearing `trigger_mode` and restarting capture. Documented, not silent.

## Migration Plan

1. Wire and verify **hardware first** (measure pads → connect → scope or `status` confirms the
   waveform) while the cameras stay free-running. Nothing on the TX2 changes yet.
2. Build `Image` with patches `0001`+`0002`+`0003`, deploy behind a **new `extlinux` LABEL**
   (`j106trig`) with the current IMX296 label kept as fallback. The DTB is reused unchanged.
3. Boot; confirm zero regression with `trigger_mode=0` — the four cameras must still free-run
   exactly as before.
4. Set `trigger_mode=1`, start the trigger, run `j106-sync-check.py`, compare against the
   free-running baseline captured in step 3.

**Rollback**: at any point, `trigger_mode=0` and restart capture returns to today's behaviour
without a reboot. If the kernel itself is at fault, select the previous `extlinux` LABEL — the
established recovery path, including from the U-Boot menu over `/dev/ttyUSB0` if the board will not
boot.

## Open Questions

- Which `/dev/ttyTHS*` the M110 `J22` header maps to. Deferred safely: the serial link is optional
  (D8) and `j106-trigctl.py` takes `--port`, so this changes no spec, no interface and no task
  beyond the identification step already in the list.
- Whether the trigger source should later be disciplined to an external time reference (GNSS PPS on
  M110 `J21` pin 7). Out of scope here and additive when wanted.
