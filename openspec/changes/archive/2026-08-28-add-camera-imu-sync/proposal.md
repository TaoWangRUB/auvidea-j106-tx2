## Why

The BEV/VIO work needs camera frames and IMU samples on one timebase with a known, stable offset.
Today neither half is in good shape:

- **The cameras drift.** Free-running, the four IMX296 sit up to 2.43 ms apart, at a phase that is
  re-randomised on every stream start, drifting a further 3–8 µs/s
  (`tools/j106-sync-check.py`). Any calibrated camera↔IMU offset goes stale within minutes.
- **The IMU is timestamped in the wrong place.** The MPU-9250 is a raw `/dev/spidev1.0` device with
  no kernel driver, so a sample's time is whenever userspace got round to the SPI read — not when
  the sample was ready. Its data-ready `INT` line is wired to `gpio-298` and sits unused.

`add-imx296-hw-trigger` fixes the first half: once the cameras run off one crystal, the camera↔IMU
offset stops drifting and becomes a **single constant**. This change makes the rest of the chain
worth that improvement, and gives the constant somewhere to live.

Three things are *not* available on this hardware and are documented here so they are not chased
again: the MPU-9250's **FSYNC** pin (the textbook way to have the IMU tag the camera trigger) is not
brought out by the J106; Tegra's **GTE** hardware GPIO timestamping is Xavier-only; and the GPIO
character device's event timestamps are **`CLOCK_REALTIME`**, not the `CLOCK_MONOTONIC` that V4L2
uses.

## What Changes

- Add an **IMU reader that timestamps on data-ready**, not on the SPI read: wait on the MPU-9250's
  `INT` line (`gpio-298`, `/dev/gpiochip1` offset 42) and take `CLOCK_MONOTONIC` at the edge.
  Includes the MPU-9250 configuration the J106 requires — push-pull (“totem pole”) `INT`, because
  the carrier inverts that line and provides no pull-up.
- Add a **frame-time estimator** that exploits the trigger's periodicity: least-squares fit
  `t[k] = a·k + b` over V4L2 monotonic timestamps, recovering exposure timing far better than any
  single timestamp (per-frame jitter is 1.5 µs sd, so phase error falls as √N) and absorbing the
  drift between the trigger's crystal and the Tegra clock.
- Add a **camera↔IMU offset calibration**: an optional hardware trigger echo into a Tegra GPIO to
  measure the one remaining constant directly, plus a documented fallback of estimating it with
  Kalibr and no extra wiring.
- Add a **combined recorder** that writes camera frame times and IMU samples on one clock, in a form
  a VIO/calibration pipeline can consume.
- **Document the dead ends** (FSYNC, GTE, REALTIME chardev timestamps) so they are not re-derived.

## Capabilities

### New Capabilities
- `camera-imu-sync`: putting camera exposures and IMU samples on one timebase with a known,
  stable offset — where each timestamp comes from, what the remaining unknown is, how it is
  measured, and what the recorded output must guarantee.

### Modified Capabilities

*(none — this adds a capability alongside `camera-hw-trigger` and `imx296-camera` without changing
either one's requirements)*

## Impact

- **Depends on** `add-imx296-hw-trigger`: the frame-time estimator assumes a hardware-periodic
  trigger. The IMU half stands alone and can be built and tested before the trigger is wired.
- **New**: `tools/j106-imu-read.py` (data-ready-timestamped IMU reader),
  `tools/j106-frametime.py` (periodicity fit), `tools/j106-record-sync.py` (combined recorder).
- **New**: documentation of the timebase chain, and of the three unavailable mechanisms.
- **Hardware**: none required. Optionally one wire plus a 1 kΩ/1.2 kΩ divider for the trigger echo
  into `gpio-389` — that pin is **1.8 V unbuffered**, so a 3.3 V drive must be divided down.
- **Unchanged**: device tree, kernel. This is entirely userspace; `gpio-298` and `gpio-389` are both
  already unclaimed.
