## Context

See `proposal.md` for motivation. Constraints that shape what is and is not attemptable here:

- **The ISP is undocumented.** NVIDIA publishes no reference for `camera_overrides.isp`, so every
  parameter change is empirical. Several plausible knobs were tried and measured to be inert (below).
- **The tuning file is global.** One `camera_overrides.isp` serves every sensor on the board. A mixed
  IMX219 + IMX296 board therefore cannot have both correct; this is a platform limitation, not a
  tuning bug.
- **The remaining colour/optics work needs physical targets.** Flat field, grey card, checkerboard and
  ideally a 24-patch chart. None can be produced over SSH, so those tasks split into "capture" (needs
  a human at the rig) and "process" (can be done from the captures).
- **Sensor selection is a boot-time decision on this platform.** Tegra binds sensors from a static DT;
  there is no firmware-level camera probe as on Raspberry Pi.

## Goals / Non-Goals

**Goals**
- Spec that matches the four-IMX296 hardware.
- Specify the ISP-tuning and bounded-AE behaviour that already shipped.
- Track the remaining image-quality work honestly, including what is blocked and on what.
- Document the IMX219 ⇄ IMX296 integration procedure, including whether DT edits can be avoided.

**Non-Goals**
- Authoring an ISP tuning from scratch, or reverse-engineering NVIDIA's AWB model.
- Any driver or device-tree change — ports C–F already work.

## Decisions

### D1. Keep the vendor tuning; correct its gaps rather than replace it

The installed tuning is INNO-MAKER's IMX296 file (found inside their Jetson Orin binary tarball, not
in any repo tree). Its own header calls it *"experimental candidate v0.2c … not production tuning"* —
derived from RidgeRun's IMX477 tuning, CCM mapped from Raspberry Pi's IMX296 libcamera JSON, IMX477
lens-shading tables removed. It still took the channel imbalance from **97 % → 21 %**, so it is the
right base. The gap it leaves is precisely the stripped lens-shading tables.

### D2. Do not use the full-strength Raspberry Pi CCM

The vendor CCM is provably Pi's 5600 K matrix damped 50 % toward identity (verified on all nine
coefficients). Substituting the undamped matrix was **measured to be worse**: imbalance 21 % → 45 %,
magenta cast, over-saturated blues. A CCM is applied *after* white balance, so it amplifies the
residual AWB error rather than correcting it. The damping stays.

### D3. Fix white balance downstream, not inside the AWB model

Two in-file routes were tried and measured inert:

| Knob | Expected | Measured |
|---|---|---|
| `ae.PerChannelGainAdjustment` | per-channel WB trim | no change to the image |
| `ae.MeanAlg.HigherTarget`/`LowerTarget` 80 → 110 | brighter | no brightening (AE already at the clamp ceiling) |

Correcting `lensShading` geometry, black level and `awb.nightmode` in the *old* IMX219 file also did
not remove the cast, which localises the error to the AWB gray-line/CCT constants. Those are
undocumented and calibrated per sensor, so the practical route is a **measured per-channel correction
applied after Argus**, or a commissioned tuning. Raw V4L2 bypasses the ISP entirely and is already
correct — consumers that use RAW are unaffected.

### D4. Sensor selection: select a device tree, do not merge one

Two sensor types cannot share one CSI port in a single DT, because each sensor node's
`remote-endpoint` binds 1:1 to an NVCSI channel and the lane width is a static property of that
channel. Notably **Raspberry Pi does not merge either** — its firmware probes the CSI I²C bus and
*loads the matching overlay*. The Tegra equivalent on this platform is one pre-built DTB per
population, selected by an `extlinux` LABEL — which this repo already does. Automating that selection
is therefore a boot-time script that probes I²C and picks the LABEL, not a DT change.

## Risks / Trade-offs

- **Global tuning file on a mixed board** → unavoidable; documented, and `ISP_FILE` makes the choice
  explicit and reversible. Raw V4L2 is the escape hatch for whichever family loses.
- **Calibration blocked on physical targets** → tasks are split so the blocked half is visible rather
  than silently skipped.
- **Lens-shading tables are lens-specific** → a calibration done through the current fisheye lenses is
  invalid if the optics change; the capture step must be repeatable and its conditions recorded.
- **Empirical tuning without a colour target is subjective** → the colour-target task is listed
  precisely so the other work can be judged numerically rather than by eye.

## Open Questions

- Which illuminants to calibrate white balance under? Deferrable: it does not change the approach,
  only how many capture sets are taken.
- Whether an automatic boot-time DTB selector is wanted, or whether choosing an `extlinux` LABEL by
  hand is preferable operationally. Deferrable: the documentation task covers both, and the selector
  is additive.
