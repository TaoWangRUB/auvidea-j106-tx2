## Why

Two gaps opened after `add-imx296-port-f` was archived, and neither is tracked anywhere.

**1. The living spec has drifted from the hardware.** That change brought up *one* IMX296 on port F
alongside five IMX219s. The carrier has since been repopulated: **ports C, D, E and F all carry
IMX296**, and **A/B are unpopulated**. `openspec/specs/imx296-camera/spec.md` still describes the
old world — "the port carrying the IMX296", `0x18` on `i2c@c250000` as *the* address, and a lane
budget counting "the IMX296 port as one lane and each IMX219 port as two". The requirements are
still individually true of port F but no longer describe the system.

**2. Image quality was worked on but never specified.** A real IMX296 ISP tuning was found and
installed, and Argus AE had to be clamped — both landed as code and README prose with no
requirements behind them, and the remaining work exists only as a prose roadmap.

Measured state today (port F, neutral scene, identical exposure):

| | R | G | B | max channel imbalance |
|---|---|---|---|---|
| Arducam **IMX219** tuning (was) | 43.4 | 85.3 | 55.2 | **97 %** |
| INNO‑MAKER **IMX296** tuning (now) | 50.0 | 44.0 | 41.4 | **21 %** |

Good enough to work with, not good enough to call correct: ~21 % residual imbalance (slightly warm),
visible vignetting, and uncorrected fisheye distortion.

## What Changes

- **Spec brought up to date with the hardware** — requirements generalised from "the IMX296 port" to
  "each populated IMX296 port", with the two‑per‑bus addressing (north `0x1a` / south `0x18`) and the
  lane budget expressed as a rule rather than a fixed number.
- **New requirements for what already shipped but was never specified**: the ISP tuning must be
  IMX296‑specific, and Argus AE must be constrained so it cannot pin gain to maximum.
- **A tracked, honest task list for what is left**: lens‑shading (vignetting) calibration, white
  balance, fisheye dewarp, and colour‑target validation — with the ones that are *blocked on physical
  calibration targets* marked as such rather than silently omitted.
- **No driver or device‑tree change is proposed here.** Ports C–F already work; this change is about
  image quality and about the spec matching reality.

## Capabilities

### New Capabilities
<!-- None. This extends and corrects the existing imx296-camera capability. -->

### Modified Capabilities
- `imx296-camera`: generalise the single‑port requirements to multiple populated IMX296 ports and the
  two‑addresses‑per‑bus layout; correct the lane‑budget requirement; add requirements for
  sensor‑appropriate ISP tuning and for bounded auto‑exposure.

## Impact

**Affected**
- `openspec/specs/imx296-camera/spec.md` — via the delta in this change.
- `tools/nvcam-settings/camera_overrides.imx296.isp`, `tools/deploy-j106.sh`,
  `tools/grid-stream-host.sh` — already carry the shipped behaviour; this change specifies it.
- `README.md` §7 — the roadmap prose becomes the tracked task list here.

**Explicitly blocked, not deferred silently**
Lens‑shading, white‑balance and dewarp calibration all need **physical targets** in front of the
cameras (a flat evenly‑lit white field, a grey card, a checkerboard, ideally a 24‑patch colour
chart). None can be done over SSH. Those tasks are written so the calibration capture is a separate,
clearly‑marked step from the processing that follows it.

**Non-goals**
- No change to `patches/0002-*`, the sensor driver, or the device tree.
- No attempt to author a tuning from scratch — the goal is to correct the vendor tuning's known gaps
  (it ships lens‑shading *parameters but no tables*), not to replace it.
