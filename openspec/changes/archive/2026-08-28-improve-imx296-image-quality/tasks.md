## 1. Already shipped — specify and verify (no code change)

- [x] 1.1 Install a sensor-appropriate ISP tuning and record the measured effect (97 % → 21 % channel imbalance)
- [x] 1.2 Make the tuning selectable and reversible (`ISP_FILE` in `deploy-j106.sh`; previous file preserved on the board)
- [x] 1.3 Bound Argus auto-exposure so gain cannot pin at maximum (`ARGUS_EXP`/`ARGUS_GAIN`/`ARGUS_DGAIN`)
- [x] 1.4 Record the negative results so they are not retried: full-strength Pi CCM (21 % → 45 %), `ae.PerChannelGainAdjustment` inert, `ae.MeanAlg.*Target` inert while AE is clamped
- [x] 1.5 Re-run the neutral-scene channel-ratio measurement on **all four** ports — done: C 27 %, D 23 %, E 28 %, F 23 %, with G/R 0.84–0.88 and G/B 1.05–1.09 on every port. The cast is **uniform across all four cameras**, so it is a tuning property, not per-module variation, and one correction serves all four

## 2. Spec brought up to date with the hardware

- [x] 2.1 Generalise "the IMX296 port" requirements to every populated IMX296 port, and specify the two-per-bus addressing (north `0x1a` / south `0x18`)
- [x] 2.2 Restate the lane budget as a rule recomputed per population, instead of a fixed number
- [x] 2.3 Add the shared-reset settle-and-retry requirement discovered when C–F were all populated
- [x] 2.4 Add requirements for sensor-appropriate ISP tuning and for bounded auto-exposure
- [ ] 2.5 Archive this change so the deltas land in `openspec/specs/imx296-camera/spec.md`

## 3. Integration procedure: choosing IMX219 or IMX296 (documentation)

- [x] 3.1 Document the **two integration cases** in README: IMX219 on the currently-empty A/B (no DT change needed — nodes are already present) versus IMX219 replacing an IMX296 port (DT change required), with the exact edits for the second
- [x] 3.2 Document **why one DT cannot serve both sensor types on one port** — each sensor node's `remote-endpoint` binds 1:1 to an NVCSI channel and lane width is static on that channel
- [x] 3.3 Document **how Raspberry Pi OS handles this** (`camera_auto_detect=1`: firmware probes the CSI I²C bus and loads the matching overlay; `dtoverlay=imx296`/`imx219` to force it; libcamera then picks its tuning JSON by sensor name) and contrast it with Tegra, where selection is an `extlinux` LABEL per pre-built DTB
- [x] 3.4 Document the **system-level settings that must change together** when swapping sensor family: DTB/LABEL, `ISP_FILE` tuning, AE clamp, and the fact that `/dev/videoN` indices shift
- [x] 3.5 Provide a helper that **probes the I²C buses and reports which sensor family is populated per port**, so the correct LABEL can be chosen without guesswork
- [x] 3.6 Wrap the whole swap procedure in one command (`j106-camera-config.py`): describe the population → generate dtsi, build DTB, deploy under its own LABEL, install the matching ISP tuning, reboot, verify; reuse an already-built DTB. Verified to reproduce the deployed DTB byte-for-byte

## 4. Vignetting — lens shading calibration

*The vendor tuning ships lens-shading parameters but no tables (24 lines vs the IMX219 file's 1216).*

- [ ] 4.1 **[BLOCKED — needs a physical target]** Capture a flat field: a uniformly lit white/grey surface filling the frame, per port, at a fixed known exposure, saved as RAW
- [ ] 4.2 Compute per-channel radial falloff from the flat-field captures and generate `lensShading.*` tables
- [ ] 4.3 Install, re-measure corner-vs-centre falloff, and keep only if measurably better
- [ ] 4.4 Record the capture conditions, since the tables are invalid if the optics change

## 5. White balance

- [ ] 5.1 **[BLOCKED — needs a physical target]** Capture a grey card under the intended operating illuminant(s), per port
- [ ] 5.2 Compute per-channel gains from the grey-card captures
- [ ] 5.3 Apply them **downstream of Argus** (the in-file AWB knobs were measured inert — design D3) and verify the neutral-scene imbalance drops from ~21 %
- [ ] 5.4 Document the resulting gains next to the measurement, so they can be re-derived rather than trusted blindly

## 6. Fisheye distortion

- [ ] 6.1 **[BLOCKED — needs a physical target]** Capture checkerboard sets per camera
- [ ] 6.2 Compute fisheye intrinsics/distortion (OpenCV fisheye model) per camera
- [ ] 6.3 Apply dewarp downstream (VPI or CUDA) — not an ISP function
- [ ] 6.4 Reuse the same intrinsics for the BEV/VIO rig rather than calibrating twice

## 7. Make it all measurable

- [ ] 7.1 **[BLOCKED — needs a physical target]** Obtain a 24-patch colour chart
- [ ] 7.2 Define the acceptance metric (per-patch ΔE, or the channel-ratio proxy already in use) and record a baseline for the current tuning
- [ ] 7.3 Only then consider tuning the CCM/AWB numerically, since without a target it is guesswork
