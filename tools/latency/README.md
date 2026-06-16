# Camera latency measurement — J106 + Jetson TX2 (6× IMX219)

Tools to measure **camera → ISP → consumer latency** for the J106 IMX219 cameras, and the
results measured on this rig. The headline tool is [`argus_evlat.cpp`](argus_evlat.cpp), which
times the **Argus capture + ISP** path; [`v4l2_lat.c`](v4l2_lat.c) is the raw-V4L2 reference.

---

## 1. What "latency" means here

The capture pipeline and where time accrues:

```
 photons ──exposure──> [sensor SOF] ──readout+MIPI──> NVCSI ──> VI ──> ISP ──> Argus consumer ──> display
            ~exp ms          (this is t=0)      └─────────── getFrameReadoutTime ──────────┘
                                                └──────────────── SOF -> ISP-done ─────────────┘
```

- **SOF (Start Of Frame)** = the hardware timestamp when the sensor begins emitting the frame
  (after exposure). It is `CLOCK_MONOTONIC`, captured by the VI hardware. This is our `t = 0`.
- **`SOF → ISP-done`** = SOF until Argus fires `CAPTURE_COMPLETE` (the ISP has finished). This is
  what `argus_evlat` measures — the **capture + ISP** cost.
- Full **glass-to-glass** ≈ `exposure + (SOF→ISP-done) + consumer/display`. Exposure happens
  *before* SOF, so it is reported separately (it's AE-dependent, not a fixed pipeline cost).

---

## 2. Principle — why it's done this way (the non-obvious part)

Getting the per-frame **SOF timestamp** into userspace on **this specific platform**
(L4T R32.7.6 / tegra186 / **EEPROM-less** IMX219) is the whole problem. Several "obvious"
methods **do not work here** — verified the hard way:

| Method | Result on this rig |
|---|---|
| Frame-embedded metadata: `IArgusCaptureMetadata` on the EGLStream `Frame` → `getMetadata()` | **null** — the EGL↔Argus metadata bridge is unreliable on this L4T |
| `EGLStream::IFrame::getTime()` | returns the **consumer acquire time** (≈now), **not** SOF |
| `nvarguscamerasrc` GstBuffer PTS | **output-stamped** (~0.7 ms = transport only), not SOF |
| ftrace `tegra_rtcpu/rtcpu_vinotify_*` | **never fires** — tegra186/Parker has **no camera RTCPU** (that's Xavier+) |
| ftrace `camera_common/tegra_channel_capture_*` | fires for the **v4l2** path only; the Argus path bypasses it (only `open`/`close`/`set_stream` fire) |

**What works — the Argus Event Queue (control path), not the data path.** libargus separates the
**data path** (EGLStream pixels) from the **control/metadata path** (events). The metadata is
reliable on the event path:

```
ICaptureSession → IEventProvider::createEventQueue({EVENT_TYPE_CAPTURE_COMPLETE})
  loop: waitForEvents → IEventQueue::getNextEvent
        → IEventCaptureComplete::getMetadata() → ICaptureMetadata::getSensorTimestamp()  // SOF, CLOCK_MONOTONIC
  latency = clock_gettime(CLOCK_MONOTONIC) at event  −  getSensorTimestamp()
```

This returns valid metadata for **every** frame (`nullmd = 0`). Confirmed the SOF and the consumer
clock are the same `CLOCK_MONOTONIC` domain before trusting the subtraction.

**ISP-cost isolation without the raw path.** `ICaptureMetadata::getFrameReadoutTime()` gives the
sensor readout duration, so `ISP/overhead ≈ (SOF→ISP-done) − readout` — no second pipeline needed
(the raw-V4L2 path does not stream on this rig; see §6).

---

## 3. Code design

### `argus_evlat.cpp` — Argus SOF→ISP-done (primary)
- **Consumer thread** creates an `EGLStream::FrameConsumer` and just **drains** frames in a loop, so
  the producer/ISP keeps running (a stream with no consumer back-pressures and stalls).
  *Order matters:* the consumer is created **before** `repeat()` (a barrier flag), mirroring NVIDIA's
  `yuvJpeg` sample — otherwise frames never flow.
- **Control (main) thread** owns the `EventQueue`, pulls `CAPTURE_COMPLETE`, and for each event
  records `now − getSensorTimestamp()` plus exposure & readout.
- **`EGL_STREAM_MODE_FIFO`, `setFifoLength(2)`** — a deep FIFO would let frames sit buffered and
  **inflate** measured latency.
- Ends with `stopRepeat()` + `waitForIdle()` then **`_exit(0)`** to skip the libargus EGLStream
  destructors, which **segfault on teardown** on this L4T (harmless, but `_exit` avoids it).

### `v4l2_lat.c` — raw V4L2 SOF→app floor (reference)
- Plain V4L2 `mmap` capture on `/dev/videoN` (SRGGB10). For each `DQBUF`,
  `latency = CLOCK_MONOTONIC_now − v4l2_buffer.timestamp` (the buffer timestamp **is** the VI SOF).
- Measures the ISP-**bypassed** capture path. Needs `nvargus-daemon` **stopped** (it holds all
  `/dev/video*`). NOTE: on this rig the raw path currently arms (STREAMON ok) but delivers **no
  frames** — kept as a reference/diagnostic; the ISP split is done via `getFrameReadoutTime` instead.

---

## 4. Build (on the board)

```bash
cd tools/latency
./build.sh          # -> ./argus_evlat  ./v4l2_lat
```
Needs the L4T multimedia API headers (`/usr/src/jetson_multimedia_api/...`) and
`libnvargus` (`/usr/lib/aarch64-linux-gnu/tegra/`), both from JetPack. g++ ≥ 5 (C++11).

---

## 5. Usage

```bash
# Argus capture+ISP latency, sensor-id 0, 150 frames:
./argus_evlat 0 150
# -> RESULT SOF->ISPdone n=135 mean=30.9 median=31.3 ... exposure=33.3 readout=27.4 isp_est=3.5 frames=0.93
```
Output fields (milliseconds): `mean/median/min/max/p95/std` of `SOF→ISP-done`; `exposure`
(AE-selected, pre-SOF); `readout` (`getFrameReadoutTime`); `isp_est = mean − readout`
(ISP+overhead); `frames = mean / 33.3ms`.

```bash
# raw V4L2 floor (ISP bypassed) — must stop the daemon first:
echo nvidia | sudo -S systemctl stop nvargus-daemon
./v4l2_lat /dev/video0 1920 1080 150
echo nvidia | sudo -S systemctl start nvargus-daemon
```

**Operational gotchas**
- `nvargus-daemon` **wedges** after repeated restarts (Argus `CANCELLED` / VI4 capture timeout);
  only a **reboot** clears it. Don't thrash it.
- The Argus **5-session start race**: launching many `nvarguscamerasrc`/sessions at once makes some
  fail. **Stagger** session starts (~1.5 s apart) to avoid it.
- Measure **per-stream** — IMX219 has no hardware fsync, so the 6 sensors' SOFs stagger.

---

## 6. Results (measured 2026-06-16, 1080p30, IMX219 on J106/TX2)

| Config | SOF→ISP-done (mean) | readout | ISP/overhead | std |
|---|---|---|---|---|
| 1 camera | **30.9 ms** | 27.4 ms | 3.5 ms | 0.8 ms |
| 5 cameras (concurrent) | **30.3 ms** | 27.4 ms | 2.9 ms | 0.4 ms |

**Findings**
- **Latency is sensor-readout-bound, not ISP-bound.** Of the ~31 ms, **27.4 ms is sensor readout**
  (clocking the frame out over the 2-lane MIPI link at the J106's 680 Mbps/lane); the **ISP adds
  only ~3 ms**. The intuition that "the ISP is the major cost" does not hold for this sensor/mode.
- **Load-insensitive.** 5 cameras streaming concurrently gives the same per-stream latency as 1
  (30.3 vs 30.9 ms — within noise). Readout is fixed by the sensor; the TX2 ISP has ample headroom.
- **Composition (perceived glass-to-glass):** `exposure (~33 ms, AE) + 31 ms (capture+ISP) +
  ~1 display refresh (16–33 ms) ≈ 80–95 ms`. To cut latency, lower the **exposure** (more light /
  cap AE) and the **readout** (lower resolution / higher MIPI rate) — not the ISP.
</content>
