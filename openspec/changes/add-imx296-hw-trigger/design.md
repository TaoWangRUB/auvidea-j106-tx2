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
  from its V12 schematic: `TIM1_CH1..CH4` land on `PE9/PE11/PE13/PE14`, header P2‑32/34/36/37
  (`PE0/1/4/5/6` are DCMI, `PE10/PE12/PE15` and `PE11/13/14` are the TFT-LCD header — not fitted —
  LED is `PE3`, USB is `PA11/PA12`, HSE is a 25 MHz crystal).
- **The camera trigger pads are an optocoupler LED, isolated from module ground** — measured, see
  D9. This was not known when the topology was first chosen, and it changed it.
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

- One hardware-timed trigger driving all four IMX296 from a single counter, so their exposures coincide.
- A wiring definition an installer can follow with a soldering iron and a multimeter — no PCB.
- Runtime switching between free-running and triggered, without touching the DTB.
- A host-side measurement that *proves* synchronisation rather than assuming it.

**Non-Goals:**

- No adapter PCB (explicitly declined; a wiring guide instead).
- No auto-exposure loop over the trigger. Exposure is set explicitly; closing an AE loop through
  the trigger source is future work.
- No auto-exposure. Per-camera exposure is now *available* (D6), but nothing drives it automatically.
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

### D3. A module parameter, not a DT property

All four sensors are triggered together and must be in the same mode at the same time, so there is
no meaningful per-sensor *mode* to express — even though, after D6, each may carry its own exposure.

The mode selector is therefore a **driver module parameter** (`imx296.trigger_mode`), read at
`set_mode`, not a per-node device-tree property:

- It matches the topology — one global switch for one global decision.
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

### D6. `TIM1_CH1..CH4` on `PE9/PE11/PE13/PE14`; auto-prescaler

`TIM1` in PWM mode 1 produces the pulse entirely in hardware: `ARR` sets the period, `CCRx` the
exposure, and `CCER.CCxP` selects which way round the pin drives it.

**Four channels, not one.** D3 originally chose a single shared net, on the assumption of a CMOS
input that one pin could fan out to four. The measurement in D9 removed that assumption: the input
is an optocoupler LED, so it takes ~10 mA of *current* each, and four in parallel is 40 mA from one
pin — over the STM32's 25 mA per-pin limit. Series is worse still, needing 4 × 1.2 V = 4.8 V of LED
drop from a 3.3 V pin. So it becomes one channel per camera, which costs three resistors and three
wires and gives back per-camera exposure. All four channels share the one counter, so the frame
start remains identical across cameras by construction — the property that actually matters.

`TIM1`'s `ARR` is 16-bit, so the prescaler is chosen at runtime:
`div = ceil(period_ticks / 65536)`, `PSC = div − 1`. This keeps the finest resolution the requested
rate allows and extends the reachable range down past 1 fps.

Rejected: `TIM2` (32-bit, no prescaler juggling). Only `TIM1` has four channels landing on pins this
board leaves free — `PE9/PE11/PE13/PE14`, all on header P2 near its `3V3`/`GND` pins, which matters
when the deliverable is hand-wiring. (`PE11/PE13/PE14` are shared with the on-board TFT-LCD header,
which is not fitted here.)

Stopping cleanly (spec: "Trigger stops cleanly") is `CCRx = 0` on every channel then disabling the
timer, so the pins park in their idle state rather than freezing mid-pulse.

The GPIOs are configured **pull-down**, not pull-up: idle must mean LED-off, so that a board in
reset or unplugged leaves the optos dark rather than holding four sensors inside an exposure.

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

### D9. The trigger input is an optocoupler — measured, and it changed the design

The original plan treated `XTR+` as a voltage input and made its level domain a documented gate,
because `XTRIG` is a 1.8 V pin with a 3.3 V absolute maximum and the pad could have been either that
raw pin or something buffered. The measurement settled it as neither:

| Measurement | Result |
|---|---|
| `XTR+` ↔ `XTR−`, diode mode | **1.2 V** forward one way, **OL** reversed |
| `3V3` ↔ module GND | **3.3 V** powered, **1.9 kΩ** unpowered (so probe contact is sound) |
| `XTR−` ↔ GND, `XTR+` ↔ GND, powered | **both OL** |

A 1.2 V junction is too high for silicon (~0.7 V) or Schottky (~0.3 V) and right for a GaAs emitter;
both legs float against a *verified* ground. It is an **optocoupler LED**. (The first attempt read OL
everywhere and looked like isolation immediately — but the ground reference was unproven at that
point, and a bad ground would have produced exactly the same three readings. Verifying the ground
first is what made the conclusion safe.)

Three consequences:

1. **The level-translation gate disappears entirely.** We never touch `XTRIG`; the opto's own output
   drives it inside the module. No divider, no translator, and no way to over-volt the sensor from
   outside. The most dangerous part of the original design is simply gone.
2. **No shared ground with the cameras.** The LED cathode returns to the MCU's ground. Better than
   the star-ground scheme it replaces.
3. **Current, not voltage** — which forces the four-channel topology in D6, since one pin cannot
   supply four LEDs.

**What the isolation hides**, and why two things must be runtime-adjustable (D10):

- Whether driving the LED *asserts* `XTRIG` or releases it is on the far side of the barrier and
  cannot be probed from here.
- The opto's turn-on and turn-off delays differ, and since pulse width is exposure, that asymmetry
  becomes a systematic exposure error. It is identical across cameras driven through identical
  interfaces, so **synchronisation is unaffected** — only absolute exposure. It also raises the
  minimum usable exposure from the sensor's 14 µs to perhaps 100 µs.

### D10. Pulse sense and transport delay are runtime settings, not build-time constants

`pol` inverts the drive; `skew` removes the opto's delay asymmetry from the pulse alongside the
sensor's own `tOFFSET`. Both are serial commands.

They are runtime settings because neither is knowable from the driving side, and the alternative —
guess, reflash, guess again — turns a two-second experiment into a build cycle during exactly the
bring-up step where iteration matters most. The default for `pol` is idle-LED-off, chosen because
that is the state that cannot leave a sensor held inside an exposure.

## Risks / Trade-offs

- **Exposure will not match the commanded value until `skew` is calibrated** → documented, and the
  bring-up order calibrates it before the four-camera run. Synchronisation is unaffected either way,
  which is the property the acceptance test measures.
- **The opto may be a slow phototransistor type** (`817`-class, tens of µs) rather than a fast logic
  opto → raises the minimum usable exposure. Recorded as a bring-up measurement; the part number is
  worth reading off the module if legible.
- **LED polarity is opposite to what the pad labels suggest** on the module measured → the wiring
  doc says to confirm per module, and notes that backwards is harmless (3.3 V reverse is inside a
  typical 5 V LED reverse rating), so it costs a swap, not a part.
- **Argus exposure control silently stops working in trigger mode** → the driver logs the mode and
  the exposure ownership once per stream, and the spec requires the active mode to be discoverable.
  Triggered operation is scoped to raw V4L2.
- **Firmware cannot be tested until the board is wired** → the risk is contained by keeping the
  clock tree trivial (D5), computing every limit from datasheet constants at runtime, and making
  `status` report clock source, period, pulse width and pulse count, so a scope is not needed to
  tell whether the firmware is doing what it claims.
- **HSI fallback changes the frame rate by up to 1%** → tolerable (sync is unaffected; only absolute
  rate moves) and reported by `status` so it cannot go unnoticed.
- **Four channels means four resistors and five wires** rather than the two originally planned →
  unavoidable once the input turned out to be current-driven, and it buys back per-camera exposure.
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
