## Context

See `proposal.md` — Why. This change depends on `add-imx296-hw-trigger` for the periodic frame
clock; the IMU half is independent and can be built first.

**Everything below was verified on the live board**, because most of the plausible designs here turn
out to be unavailable on this hardware and the negative results are the useful part.

| Fact | Evidence |
|---|---|
| V4L2 buffer timestamps are **`CLOCK_MONOTONIC`** | buffer flags `0x2001`; bit `0x2000` is `V4L2_BUF_FLAG_TIMESTAMP_MONOTONIC`. Cross-checked: `mono − ts` is a stable +67 ms (buffer depth), `real − ts` is 1.79e9 s |
| Per-frame timestamp jitter | **1.5 µs sd** over 600 frames, all four cameras (`j106-sync-check.py`) |
| IMU is raw spidev | `/dev/spidev1.0`; the only `iio` devices are two `ina3221x` power monitors — there is no MPU-9250 kernel driver |
| IMU data-ready **is** wired | J106 manual: pin 12 `INT` → `G14` → `GPIO9_MOTION_INT`; live: **`gpio-298`**, AON chip (`gpiochip1`), **unclaimed** |
| MPU-9250 **FSYNC** is **not** brought out | The J106 IMU pin table lists only `AD0/SDO`, `SDA/SDI`, `SCL/SCLK`, `/CS`, `INT`. No FSYNC anywhere in the manual |
| Tegra **GTE** hardware GPIO timestamping is **unavailable** | `gpio-tegra186.c` has the hooks, but the pin table is `tegra194_gte_info[]` only; `CONFIG_TEGRA_HTS_GTE is not set`; neither GPIO node carries `use-timestamp` or a `gte` reg |
| GPIO chardev event timestamps are **`CLOCK_REALTIME`** | `gpiolib.c:730` — `ge.timestamp = ktime_get_real_ns()` |
| `gpio-389` (trigger echo candidate) | M110 `J21` pin 8, unclaimed, **1.8 V unbuffered** |

## Goals / Non-Goals

**Goals:**

- Camera and IMU timestamps on one monotonic clock, with the residual offset reduced to a single
  measured constant.
- Frame timing recovered to well under the per-frame timestamp jitter.
- Work with **no additional wiring**; make the optional wire a calibration convenience, not a
  dependency.

**Non-Goals:**

- No VIO/SLAM implementation, and no calibration solver — this produces input for one.
- No kernel driver for the MPU-9250. Userspace over `spidev` plus the data-ready line is enough for
  the accuracy this can reach, and a driver would not lift the ceiling (see D2).
- No attempt to discipline either clock to an absolute reference (GNSS/PTP).
- No magnetometer handling; the AK8963 is a separate concern at a different rate.

## Decisions

### D1. Exploit the trigger's periodicity instead of trusting individual timestamps

The single highest-value idea here, and it costs nothing.

Individual V4L2 timestamps carry 1.5 µs sd of jitter. But under a hardware trigger the frames are
*not* independent samples — they lie on a line. Fitting

```
t[k] = a·k + b
```

by least squares over N frames reduces the phase uncertainty as √N: 600 frames takes 1.5 µs down to
well under 100 ns. `a` is the trigger's true period **expressed in Tegra clock units**, which is
exactly what absorbs the ±20 ppm rate difference between the STM32 crystal and the Tegra clock —
otherwise that difference accumulates without bound.

This is only valid because the trigger is hardware-periodic. Free-running, the "period" is not
constant and the fit is meaningless — which is another way of saying the trigger is what makes
precise frame timing possible at all.

Guard: dropped frames break the `k` indexing. Use the V4L2 `sequence` field, not the arrival order,
as `k`.

### D2. Timestamp the IMU on data-ready, and take our own monotonic reading

Two independent problems, two parts to the decision.

**Where the timestamp comes from.** Reading `/dev/spidev1.0` in a loop times the *read*, not the
sample: the SPI transfer, the scheduler, and any contention all land in the number. The MPU-9250's
data-ready `INT` is already wired to `gpio-298` and unclaimed, so waiting on that edge times the
sample instead.

**Which clock.** The obvious route — the GPIO character device's `GPIOEVENT` interface — reports its
own timestamp, but on this kernel that is `ktime_get_real_ns()`, i.e. **`CLOCK_REALTIME`**. V4L2 is
`CLOCK_MONOTONIC`. Mixing them silently would be a correctness bug that NTP makes intermittent.

So: **use the chardev to wait for the edge, but discard its timestamp** and take
`clock_gettime(CLOCK_MONOTONIC)` in the handler. That costs the userspace wake-up latency (tens of
µs) but is on the right clock, and it is honest about what it measures.

Rejected: converting REALTIME→MONOTONIC by sampling both. It works, but the conversion has to be
re-measured continuously (they drift apart under NTP discipline), for no accuracy gain over just
reading the right clock.

Rejected: a kernel driver to timestamp in the IRQ handler. It would remove the wake-up latency, but
`gpio-298` is on the **AON** chip and its interrupt path is not fast enough for that to be the
dominant term; more importantly the residual is a *constant* offset, which D3 absorbs anyway.

**Carrier specifics that must be programmed**, from the J106 manual: the INT output has **no
pull-up** and must be set to push-pull (“totem pole”) in `INT_PIN_CFG`; and the carrier **inverts**
the line, so the edge polarity to request is the opposite of the MPU-9250's own setting.

### D3. The remaining unknown is one constant, and it does not need hardware to find

After D1 and D2 the only unknown is Δ: where the V4L2 timestamp sits relative to the start of
exposure. It is a property of the VI capture path, so it is *constant* — and constants can be
estimated from data.

Two routes, both supported:

- **Measure it**: echo the trigger into `gpio-389` and compare edge time to frame time. Direct, and
  turns Δ into a number you can write down. ⚠ that pin is **1.8 V unbuffered**, so the 3.3 V trigger
  must be divided (1 kΩ / 1.2 kΩ → 1.80 V) — the one place in this project where the level problem
  genuinely applies, since the optocoupler removed it everywhere else.
- **Estimate it**: let a calibration tool (Kalibr and equivalents already solve for a camera↔IMU
  time offset `td`) absorb it. No wiring at all.

The echo is therefore a *calibration aid*, not a runtime dependency — once Δ is known the wire can
come off. This is the reason the capability is specified in terms of "the offset is a measured
constant" rather than "there shall be an echo wire".

### D4. Userspace, no kernel or device-tree change

`gpio-298` and `gpio-389` are both unclaimed, `spidev` is already bound, and every timestamp needed
is available to userspace. Given this repo's history — the device tree is the one artifact whose
breakage costs a UART boot-menu recovery — keeping this entirely in userspace is worth more than the
tens of µs a kernel module would save.

### D5. Record provenance, because a recording outlives the session that made it

The output carries the clock name, the trigger rate and exposure in force, the Δ applied, and
whether the cameras were triggered or free-running. A recording that does not say whether it was
synchronised is indistinguishable from one that was, and will eventually be fed to a solver as if it
were — producing a confidently wrong calibration.

## Risks / Trade-offs

- **Userspace IMU wake-up latency (tens of µs) sits in every sample** → it is largely a constant
  offset, so D3 absorbs it; the varying part is small next to a 1 kHz sample interval. If it ever
  matters, the fix is a kernel module, not a redesign.
- **The periodicity fit is invalid when the trigger is off** → the tool must detect free-running
  operation and refuse to report a fit, rather than returning a meaningless line. `j106-sync-check`
  already distinguishes the two cases and the same test applies.
- **Dropped frames corrupt the fit if arrival order is used as `k`** → use the V4L2 `sequence`
  field, and report drops.
- **Δ may not be perfectly constant across modes** (resolution, binning, exposure) → measure it per
  configuration; record it in the output so a mismatch is visible rather than silent.
- **The magnetometer is on a separate internal path at a lower rate** → out of scope here; treating
  it as synchronous with the gyro would be wrong.

## Open Questions

- Whether Δ varies with commanded exposure. Deferrable: it is measured per configuration either way,
  and the recorded provenance makes a mismatch detectable.
- What IMU rate the VIO front-end actually wants (200 Hz vs 1 kHz). Does not change any interface
  here — it is a configuration value.
