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
      timestamp's error budget is a number rather than an assumption — **method built**:
      `j106-imu-read.py --latency` compares the chardev's kernel `CLOCK_REALTIME` event stamp
      against our own `CLOCK_MONOTONIC` read, with the REALTIME read bracketed between two
      MONOTONIC reads so the clock offset is dated to the midpoint. It also reports the
      `clock_gettime` call cost as its own measurement floor. Unverified: needs the board
- [x] 1.5 Establish the camera-side error budget — **done**: inter-camera skew under hardware
      trigger is **1.0 µs max, 0.00 µs/s drift** (`add-imx296-hw-trigger` §6.5), so the cameras are
      effectively one clock and Δ is a single offset to estimate, not four drifting ones. The
      STM32 still free-runs against the Tegra clock: measured frame interval 33332.5 µs against a
      commanded 33333 µs, ≈ −15 µs/s (~50 ms/hour), so a fixed Δ goes stale on long runs — this is
      what the §3 fit is for

## 2. IMU reader (`tools/j106-imu-read.py`)

- [ ] 2.1 MPU-9250 bring-up over `/dev/spidev1.0`: `WHO_AM_I` = `0x71`, gyro/accel ranges, sample
      rate divider — **written**, unverified (needs the board). Every config write is read back and verified; `I2C_IF_DIS` is
      set (SPI-only); a wrong `WHO_AM_I` names the likely part and points at the other spidev nodes
- [ ] 2.2 Configure `INT_PIN_CFG` for **push-pull** (no pull-up on the carrier) and enable the
      data-ready interrupt; account for the J106 **inverting** the line when choosing edge polarity — **written**, unverified (needs the board): push-pull, unlatched, `INT_ANYRD_2CLEAR`; the edge defaults to **falling** because of the
      inversion, and `--active-low` flips both ends together
- [ ] 2.3 Wait on `gpio-298` via `/dev/gpiochip1` offset 42; **discard the chardev's own timestamp**
      (it is `CLOCK_REALTIME`) and take `clock_gettime(CLOCK_MONOTONIC)` at the edge — **written**, unverified (needs the board). The chip/offset are **resolved at runtime** from the sysfs base + label rather than
      hard-coded, since both are assigned in probe order. `clock_gettime` is called through
      `ctypes` for integer nanoseconds — the board's Python 3.6 has no `clock_gettime_ns`, and
      float seconds quantise to ~240 ns at epoch scale
- [ ] 2.4 Read the sample burst over SPI after the edge; keep the edge time as the sample time — **written**, unverified (needs the board): one 14-byte transfer (accel+temp+gyro) so the sample is internally consistent
- [ ] 2.5 Detect and report dropped samples, so a gap cannot pass as a regular series — **written**, unverified (needs the board), by two independent routes: an interval longer than one period, and >1 event already
      queued when we woke (the data registers hold only the newest sample, so every earlier
      queued edge is a lost sample). The rate-error figure excludes intervals spanning a gap —
      a single drop would otherwise swamp a ppm-scale number
- [ ] 2.6 Verify end to end: sample interval matches the configured rate, and the timestamps are
      monotonic and gap-free under load — **blocked on the board**

## 3. Frame-time estimator (`tools/j106-frametime.py`)

- [ ] 3.1 Collect V4L2 timestamps plus the `sequence` field from one or more `/dev/video*` — **written**, unverified (needs the board), reusing `j106-sync-check.py`'s `Camera` class rather than reimplementing V4L2
- [ ] 3.2 Least-squares fit `t[k] = a·k + b` **indexed by `sequence`**, not arrival order, so drops
      do not corrupt the fit — **written**, unverified (needs the board). Checked against synthetic data (600 frames, 1.5 µs injected jitter, 3 drops): recovers
      the period to **0.002 ppm** and the phase to **25 ns**. The same data fitted by arrival
      order is off by **5776 ppm** — the trap is quantified, not just asserted
- [ ] 3.3 Report `a` (trigger period in Tegra clock units), `b`, residual sd, and the implied
      STM32-to-Tegra rate offset in ppm — **written**, unverified (needs the board), plus the walk in ms/hour, and `--json` for `j106-record-sync.py`
- [ ] 3.4 Refuse to report a fit when the cameras are free-running — detect it the way
      `j106-sync-check.py` does and say so, rather than returning a meaningless line — **written**, unverified (needs the board). **Two checks, not one**: `imx296.trigger_mode` (what the driver was told) and the
      inter-camera skew/drift test reusing `nearest_skew`/`drift_rate` at the same thresholds
      (what the wiring actually did). With one camera only the first is available, and it says
      so. `--force` overrides, with the coefficients marked as session-only
- [ ] 3.5 Derive exposure midpoint per frame from the fit, the commanded exposure, the sensor's
      14.26 µs offset and the optocoupler `skew`. **Pipeline constants now measured** on IMX296
      @1456×1088 (`add-imx296-hw-trigger` §11), so this no longer needs estimating:
      `readout = 16.1 ms`, `ISP = 1.9 ms`, `SOF→ISP-done = 18.0 ms` (sd 0.1), raw-V4L2 buffer
      delivery `= 66.7 ms` after end-of-readout (sd 0.03, inherent — not queue depth).
      Argus: `t_mid = t_SOF − exposure/2`. Raw V4L2: `t_mid = t_buffer − 16.1 ms − exposure/2`,
      the buffer stamp being `EndOfFrame` on `CLOCK_MONOTONIC` (flag `0x00002001`).
      Under hardware trigger `exposure` is the exact commanded pulse width, not an AE estimate,
      and all four cameras share one edge — so a single `t_mid` covers all four — **written**, unverified (needs the board): the constants are module-level with provenance, and `--exposure-us`/`--skew-us` feed
      `exposure_model()`, shared with the recorder
- [ ] 3.6 Show the uncertainty improving as √N with frame count, to confirm the fit behaves — **written**, unverified (needs the board): refits over growing prefixes and prints measured vs ideal phase error. The ideal curve is
      anchored on the **full run's** residual sd — anchoring on the smallest prefix bends the
      reference line instead of the data. On synthetic data the two track within a few percent
      from N=100 up

## 4. Camera-to-IMU offset (Δ)

- [ ] 4.1 Document both routes in the wiring guide: measure Δ with a trigger echo, or estimate it
      with a calibration solver and no extra hardware
- [ ] 4.2 Trigger echo wiring — `gpio-389` (M110 `J21` pin 8), ⚠ **1.8 V unbuffered**, so specify
      the 1 kΩ/1.2 kΩ divider from the 3.3 V trigger
- [ ] 4.3 Measure Δ: compare echo edge time to the frame's fitted time; report mean and spread
- [ ] 4.4 Check whether Δ depends on commanded exposure; record the answer

## 5. Combined recorder (`tools/j106-record-sync.py`)

- [ ] 5.1 Record camera frame times and IMU samples on one monotonic clock, one time column — **written**, unverified (needs the board): IMU on its own thread, cameras on theirs, everything `CLOCK_MONOTONIC`. The IMU sample
      loop is a generator shared with `j106-imu-read.py`, so there is one implementation of the
      timing rather than two
- [ ] 5.2 Write provenance: clock name, trigger rate and exposure, Δ applied, and whether the
      cameras were triggered or free-running — **written**, unverified (needs the board) — `meta.json`, including the pipeline constants used, the fit per camera, the IMU's DLPF
      group delays, and `delta_source` (defaults to `"UNMEASURED — assumed zero"`, never a
      silent 0). Each camera row keeps `t_buffer` and `t_fit` next to the corrected timestamp,
      so the correction can be redone if a constant is re-measured
- [ ] 5.3 Mark free-running recordings as **unsynchronised** so they cannot be mistaken for
      triggered data — **written**, unverified (needs the board): free-running is **refused outright** unless `--allow-unsynchronised` is passed, and such
      a recording gets both `"synchronised": false` in `meta.json` and an `UNSYNCHRONISED` file
      naming the failed checks
- [ ] 5.4 Emit a layout a calibration/VIO pipeline can consume without bespoke parsing — **written**, unverified (needs the board): EuRoC/ASL — `imu0.csv` in exactly the EuRoC 7 columns, `camN/data.csv`, `meta.json`.
      Pixel data is deliberately out of scope (four raw streams are ~380 MB/s); images dropped
      into `camN/data/` line up row by row

## 6. Documentation

- [x] 6.1 Add a README section on the timebase chain: where each timestamp comes from, which clock,
      and what the remaining unknown is — **done**: README **§5a** (chain table, the fit and why
      it is indexed by `sequence`, the −15 ppm STM32-vs-Tegra walk, the DLPF group delays and
      the ≈1.0 ms gyro-vs-accel difference, and Δ)
- [x] 6.2 Record the three dead ends prominently (FSYNC absent, GTE Xavier-only, chardev timestamps
      are REALTIME) — each is a plausible design someone will otherwise attempt — **done**:
      README §5a.2, each with the evidence that settles it
- [x] 6.3 Add the tools to `tools/README.md` with the "where this runs" convention — **done**:
      all three, each with its run-location banner
- [ ] 6.4 Update the memory notes with the verified timebase facts — the dead ends and the
      wiring facts are already recorded; the **measured** numbers (wake-up latency, Δ) go in
      once the board is back
