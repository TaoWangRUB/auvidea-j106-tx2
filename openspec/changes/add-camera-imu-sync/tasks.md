## 1. Verify the timebase chain

- [x] 1.1 Confirm V4L2 buffer timestamps are `CLOCK_MONOTONIC` from the buffer's own
      timestamp-type flag — **done**: flags `0x2001`, cross-checked against `CLOCK_MONOTONIC` and
      `CLOCK_REALTIME`
- [x] 1.2 Confirm the IMU data-ready line is wired and free — **done**: `gpio-298`
      (`GPIO9_MOTION_INT`, AON `gpiochip1` offset 42), unclaimed
- [x] 1.3 Establish what is *not* available and record it so it is not chased again — **done**:
      MPU-9250 `FSYNC` not brought out by the J106; Tegra GTE hardware GPIO timestamping is
      Xavier-only (`tegra194_gte_info[]`, `CONFIG_TEGRA_HTS_GTE` off); GPIO chardev event
      timestamps are `CLOCK_REALTIME` (`gpiolib.c:730`)
- [ ] 1.4 Measure the userspace wake-up latency and jitter on a `gpiochip` edge, so the IMU
      timestamp's error budget is a number rather than an assumption

## 2. IMU reader (`tools/j106-imu-read.py`)

- [ ] 2.1 MPU-9250 bring-up over `/dev/spidev1.0`: `WHO_AM_I` = `0x71`, gyro/accel ranges, sample
      rate divider
- [ ] 2.2 Configure `INT_PIN_CFG` for **push-pull** (no pull-up on the carrier) and enable the
      data-ready interrupt; account for the J106 **inverting** the line when choosing edge polarity
- [ ] 2.3 Wait on `gpio-298` via `/dev/gpiochip1` offset 42; **discard the chardev's own timestamp**
      (it is `CLOCK_REALTIME`) and take `clock_gettime(CLOCK_MONOTONIC)` at the edge
- [ ] 2.4 Read the sample burst over SPI after the edge; keep the edge time as the sample time
- [ ] 2.5 Detect and report dropped samples, so a gap cannot pass as a regular series
- [ ] 2.6 Verify end to end: sample interval matches the configured rate, and the timestamps are
      monotonic and gap-free under load

## 3. Frame-time estimator (`tools/j106-frametime.py`)

- [ ] 3.1 Collect V4L2 timestamps plus the `sequence` field from one or more `/dev/video*`
- [ ] 3.2 Least-squares fit `t[k] = a·k + b` **indexed by `sequence`**, not arrival order, so drops
      do not corrupt the fit
- [ ] 3.3 Report `a` (trigger period in Tegra clock units), `b`, residual sd, and the implied
      STM32-to-Tegra rate offset in ppm
- [ ] 3.4 Refuse to report a fit when the cameras are free-running — detect it the way
      `j106-sync-check.py` does and say so, rather than returning a meaningless line
- [ ] 3.5 Derive exposure midpoint per frame from the fit, the commanded exposure, the sensor's
      14.26 µs offset and the optocoupler `skew`
- [ ] 3.6 Show the uncertainty improving as √N with frame count, to confirm the fit behaves

## 4. Camera-to-IMU offset (Δ)

- [ ] 4.1 Document both routes in the wiring guide: measure Δ with a trigger echo, or estimate it
      with a calibration solver and no extra hardware
- [ ] 4.2 Trigger echo wiring — `gpio-389` (M110 `J21` pin 8), ⚠ **1.8 V unbuffered**, so specify
      the 1 kΩ/1.2 kΩ divider from the 3.3 V trigger
- [ ] 4.3 Measure Δ: compare echo edge time to the frame's fitted time; report mean and spread
- [ ] 4.4 Check whether Δ depends on commanded exposure; record the answer

## 5. Combined recorder (`tools/j106-record-sync.py`)

- [ ] 5.1 Record camera frame times and IMU samples on one monotonic clock, one time column
- [ ] 5.2 Write provenance: clock name, trigger rate and exposure, Δ applied, and whether the
      cameras were triggered or free-running
- [ ] 5.3 Mark free-running recordings as **unsynchronised** so they cannot be mistaken for
      triggered data
- [ ] 5.4 Emit a layout a calibration/VIO pipeline can consume without bespoke parsing

## 6. Documentation

- [ ] 6.1 Add a README section on the timebase chain: where each timestamp comes from, which clock,
      and what the remaining unknown is
- [ ] 6.2 Record the three dead ends prominently (FSYNC absent, GTE Xavier-only, chardev timestamps
      are REALTIME) — each is a plausible design someone will otherwise attempt
- [ ] 6.3 Add the tools to `tools/README.md` with the "where this runs" convention
- [ ] 6.4 Update the memory notes with the verified timebase facts
