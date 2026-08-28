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
- [x] 1.4 Measure the userspace wake-up latency and jitter on a `gpiochip` edge, so the IMU
      timestamp's error budget is a number rather than an assumption — **method built**:
      `j106-imu-read.py --latency` compares the chardev's kernel `CLOCK_REALTIME` event stamp
      against our own `CLOCK_MONOTONIC` read, with the REALTIME read bracketed between two
      MONOTONIC reads so the clock offset is dated to the midpoint. It also reports the
      `clock_gettime` call cost as its own measurement floor.
      **MEASURED** (3000 samples @200 Hz, `chrt -f 80`): median **50.4 µs**, **MAD 2.8 µs**,
      p95 55.7, p99 75.4, rare ~3.4 ms tail about 1 in 3000. At normal priority: median 56.4,
      p95 100.0, p99 122.7. The median is bias that Δ absorbs; the MAD is the real limit, and it
      is the same order as the camera side's 1.5 µs frame jitter. Note the `clock_gettime` call
      itself costs **5.3 µs** here — larger than the jitter measured — which is why the bracketing
      matters
- [x] 1.5 Establish the camera-side error budget — **done**: inter-camera skew under hardware
      trigger is **1.0 µs max, 0.00 µs/s drift** (`add-imx296-hw-trigger` §6.5), so the cameras are
      effectively one clock and Δ is a single offset to estimate, not four drifting ones. The
      STM32 still free-runs against the Tegra clock: measured frame interval 33332.5 µs against a
      commanded 33333 µs, ≈ −15 µs/s (~50 ms/hour), so a fixed Δ goes stale on long runs — this is
      what the §3 fit is for

## 2. IMU reader (`tools/j106-imu-read.py`)

- [x] 2.1 MPU-9250 bring-up over `/dev/spidev1.0`: `WHO_AM_I` = `0x71`, gyro/accel ranges, sample
      rate divider — **VERIFIED on the board 2026-08-28**. Every config write is read back and verified; `I2C_IF_DIS` is
      set (SPI-only); a wrong `WHO_AM_I` names the likely part and points at the other spidev nodes
- [x] 2.2 Configure `INT_PIN_CFG` for **push-pull** (no pull-up on the carrier) and enable the
      data-ready interrupt; account for the J106 **inverting** the line when choosing edge polarity — **VERIFIED on the board 2026-08-28**: push-pull, unlatched, `INT_ANYRD_2CLEAR`; the edge defaults to **falling** because of the
      inversion, and `--active-low` flips both ends together
- [x] 2.3 Wait on `gpio-298` via `/dev/gpiochip1` offset 42; **discard the chardev's own timestamp**
      (it is `CLOCK_REALTIME`) and take `clock_gettime(CLOCK_MONOTONIC)` at the edge — **VERIFIED on the board 2026-08-28**. The chip/offset are **resolved at runtime** from the sysfs base + label rather than
      hard-coded, since both are assigned in probe order. `clock_gettime` is called through
      `ctypes` for integer nanoseconds — the board's Python 3.6 has no `clock_gettime_ns`, and
      float seconds quantise to ~240 ns at epoch scale
- [x] 2.4 Read the sample burst over SPI after the edge; keep the edge time as the sample time — **VERIFIED on the board 2026-08-28**: one 14-byte transfer (accel+temp+gyro) so the sample is internally consistent
- [x] 2.5 Detect and report dropped samples, so a gap cannot pass as a regular series — **VERIFIED on the board 2026-08-28**, by two independent routes: an interval longer than one period, and >1 event already
      queued when we woke (the data registers hold only the newest sample, so every earlier
      queued edge is a lost sample). The rate-error figure excludes intervals spanning a gap —
      a single drop would otherwise swamp a ppm-scale number
- [x] 2.6 Verify end to end: sample interval matches the configured rate, and the timestamps are
      monotonic and gap-free under load — **VERIFIED on the board 2026-08-28**: `WHO_AM_I=0x71` first try; the
      chip/offset lookup resolved `gpio-298` to `/dev/gpiochip1` offset 42 at runtime, matching
      the hand-recorded value. 200.24 Hz measured, and in the 30 s combined recording **6062
      samples, 0 dropped, 0 late reads** while all four cameras streamed. Gravity check
      |a| = 10.46 m/s² = 66 mg from 1 g, i.e. the part's uncalibrated zero-g offset (±60 mg typ),
      now reported by the tool as a standing sanity check

## 3. Frame-time estimator (`tools/j106-frametime.py`)

- [x] 3.1 Collect V4L2 timestamps plus the `sequence` field from one or more `/dev/video*` — **VERIFIED on the board 2026-08-28**, reusing `j106-sync-check.py`'s `Camera` class rather than reimplementing V4L2
- [x] 3.2 Least-squares fit `t[k] = a·k + b` **indexed by `sequence`**, not arrival order, so drops
      do not corrupt the fit — **VERIFIED on the board 2026-08-28**. Checked against synthetic data (600 frames, 1.5 µs injected jitter, 3 drops): recovers
      the period to **0.002 ppm** and the phase to **25 ns**. The same data fitted by arrival
      order is off by **5776 ppm** — the trap is quantified, not just asserted
- [x] 3.3 Report `a` (trigger period in Tegra clock units), `b`, residual sd, and the implied
      STM32-to-Tegra rate offset in ppm — **VERIFIED on the board 2026-08-28**, plus the walk in ms/hour, and `--json` for `j106-record-sync.py`
- [x] 3.4 Refuse to report a fit when the cameras are free-running — detect it the way
      `j106-sync-check.py` does and say so, rather than returning a meaningless line — **VERIFIED on the board 2026-08-28**. **Two checks, not one**: `imx296.trigger_mode` (what the driver was told) and the
      inter-camera skew/drift test reusing `nearest_skew`/`drift_rate` at the same thresholds
      (what the wiring actually did). With one camera only the first is available, and it says
      so. `--force` overrides, with the coefficients marked as session-only
- [x] 3.5 Derive exposure midpoint per frame from the fit, the commanded exposure, the sensor's
      14.26 µs offset and the optocoupler `skew`. **Pipeline constants now measured** on IMX296
      @1456×1088 (`add-imx296-hw-trigger` §11), so this no longer needs estimating:
      `readout = 16.1 ms`, `ISP = 1.9 ms`, `SOF→ISP-done = 18.0 ms` (sd 0.1), raw-V4L2 buffer
      delivery `= 66.7 ms` after end-of-readout (sd 0.03, inherent — not queue depth).
      Argus: `t_mid = t_SOF − exposure/2`. Raw V4L2: `t_mid = t_buffer − 16.1 ms − exposure/2`,
      the buffer stamp being `EndOfFrame` on `CLOCK_MONOTONIC` (flag `0x00002001`).
      Under hardware trigger `exposure` is the exact commanded pulse width, not an AE estimate,
      and all four cameras share one edge — so a single `t_mid` covers all four — **VERIFIED on the board 2026-08-28**: the constants are module-level with provenance, and `--exposure-us`/`--skew-us` feed
      `exposure_model()`, shared with the recorder
- [x] 3.6 Show the uncertainty improving as √N with frame count, to confirm the fit behaves — **VERIFIED on the board 2026-08-28**: refits over growing prefixes and prints measured vs ideal phase error. The ideal curve is
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

- [x] 5.1 Record camera frame times and IMU samples on one monotonic clock, one time column — **VERIFIED on the board 2026-08-28**: IMU on its own thread, cameras on theirs, everything `CLOCK_MONOTONIC`. The IMU sample
      loop is a generator shared with `j106-imu-read.py`, so there is one implementation of the
      timing rather than two
- [x] 5.2 Write provenance: clock name, trigger rate and exposure, Δ applied, and whether the
      cameras were triggered or free-running — **VERIFIED on the board 2026-08-28** — `meta.json`, including the pipeline constants used, the fit per camera, the IMU's DLPF
      group delays, and `delta_source` (defaults to `"UNMEASURED — assumed zero"`, never a
      silent 0). Each camera row keeps `t_buffer` and `t_fit` next to the corrected timestamp,
      so the correction can be redone if a constant is re-measured
- [x] 5.3 Mark free-running recordings as **unsynchronised** so they cannot be mistaken for
      triggered data — **VERIFIED on the board 2026-08-28**: free-running is **refused outright** unless `--allow-unsynchronised` is passed, and such
      a recording gets both `"synchronised": false` in `meta.json` and an `UNSYNCHRONISED` file
      naming the failed checks
- [x] 5.4 Emit a layout a calibration/VIO pipeline can consume without bespoke parsing — **VERIFIED on the board 2026-08-28**: EuRoC/ASL — `imu0.csv` in exactly the EuRoC 7 columns, `camN/data.csv`, `meta.json`.
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
- [x] 6.4 Update the memory notes with the verified timebase facts — **done**: wake-up latency,
      the NTP slew and its two distinct effects, the triggered-vs-free-running residual, the
      gravity check, and the `nearest_skew` artefact. Also **corrected** the stored "−15 ppm"
      STM32-vs-Tegra figure, which was measuring the crystal difference through an uncontrolled
      amount of NTP slew, and recorded that the trigger generator is on the board's
      `/dev/ttyTHS1`, not a USB CDC port. Δ goes in once measured

## 7. Findings from the live bring-up (not anticipated by the plan)

- [x] 7.1 **NTP slews `CLOCK_MONOTONIC`, and it is the largest term in the rate.** Only
      `CLOCK_MONOTONIC_RAW` is free of NTP; V4L2 stamps with `CLOCK_MONOTONIC`. Measured on this
      board: **+48.45 ppm** slew relative to `_RAW` — about 4× the crystal difference the fit is
      meant to report. `j106-frametime.py` now measures the slew during every run and reports the
      de-slewed crystal figure. Validation: two runs whose raw figures were +24.57 and +20.90 ppm
      gave de-slewed **−12.34** and **−12.27 ppm** — agreement to 0.07 ppm across a 3.7 ppm spread
      in the raw number
- [x] 7.2 **The timesyncd servo is what makes the fit residual wander.** Over 900 frames the
      residual is **30.9 µs** with the daemon running, tracing a smooth ±60 µs arc (frame-to-frame
      change only 3.25 µs — a wander, not jitter); with it stopped, **8.4 µs**. Stopping it does
      **not** remove the static slew: the kernel keeps the last `adjtimex` frequency correction,
      still +33 ppm afterwards. **Δ is unaffected either way** — both series are on the same slewed
      clock, so it is common mode and cancels in the difference. It corrupts *rate*, not *offset*
- [x] 7.3 **Bug found in the acceptance test `j106-sync-check.py`** (pre-existing): `nearest_skew`
      clamped at the ends of the series, so when two cameras did not start and stop on the same
      frame it reported a whole **33.3 ms** frame period as "worst skew" and failed a rig that was
      in fact synchronised to 1 µs. That is not a skew measurement, it is the absence of a partner.
      Pairs beyond half a frame period are now dropped, and the function returns aligned
      `(times, skews)` so the drift regression cannot be silently misaligned by the filtering.
      After the fix the same rig reports **SYNCHRONISED, 1.0 µs skew, 0.02 µs/s drift**
- [x] 7.4 **The √N check needs a degeneracy guard.** V4L2 stamps are quantised to 1 µs, so a short
      prefix can land exactly on the line and fit "perfectly" — which produced a
      "34335104× better than independent jitter allows" note. Prefixes fitting below 0.01 µs no
      longer vote, and the note needs two deviating prefixes rather than one
- [x] 7.5 **Triggered fit residual is larger than free-running** (4–8 µs vs 0.63 µs), which is not a
      fault: free-running, the buffer stamps track the sensor's own perfectly periodic readout;
      triggered, they are measured against an external crystal, so VI/DMA completion spread shows up
