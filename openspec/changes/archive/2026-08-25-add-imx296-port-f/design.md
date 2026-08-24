## Context

See `proposal.md` — Why, for motivation and the confirmed hardware facts.

The constraints that actually shape the approach:

- **Kernel is L4T R32.7.6 / `4.9.337-tegra`.** Upstream's `imx296.c` (Laurent Pinchart, ~Linux 6.4)
  is a plain V4L2 sub-device driver built on regmap, `v4l2_subdev_state`, and control APIs that do
  not exist in 4.9. It cannot be dropped in.
- **Argus is a hard requirement, so the driver must be tegracam.** Only sensors registered through
  `tegracam_device` / `camera_common` get an ISP path; a bare V4L2 sub-device would give raw capture
  at best. `imx219.c` and `ov9281.c` in this tree are the shape to match.
- **Port F must not disturb ports A–E.** All six reset pins are one GPIO (461), all six share one
  24 MHz `extperiph1` MCLK, and the five IMX219s currently work. Any driver that claims the reset
  line exclusively, or drives it low, or changes the MCLK rate, breaks them.
- **The IMX296 is single-lane.** IMX219 ports run 2 lanes; port F must run 1, so lane configuration
  can no longer be uniform across the tree.
- **Datasheet**: the full Sony IMX296LQR-C datasheet is now in the repo
  (`IMX296LQR-C_Fulldatasheet_Awin.pdf`) and is the primary reference for everything below. It
  independently confirms every register value taken from upstream mainline `imx296.c` — all three
  `INCKSEL` triplets, `CTRL418C`, `HMAX = 1100`, `BLKLEVEL = 03Ch` — so mainline can be trusted for
  the parts the datasheet leaves implicit (notably the undocumented init table).
- **The module is self-clocked** (`extperiph1` `enable_count=0` while the sensor answered), which is
  the only reason a 24 MHz-fanout board can host a sensor whose INCK table is 37.125/54/74.25 MHz.

## Goals / Non-Goals

**Goals:**

- One IMX296 mode (1456×1088 RAW10) usable from raw V4L2 *and* Argus on port F.
- Zero regression on ports A–E — same binds, same modes, same capture.
- Driver packaged as `patches/0002-*.patch`, independent of and non-conflicting with `0001`.
- Register behaviour traceable to upstream's register map rather than invented.

**Non-Goals (design-level, beyond the proposal's scope exclusions):**

- No cropping / binning / windowed ROI modes — full array only.
- No runtime mono/colour switching; the variant is detected and logged, and the colour Bayer order
  is fixed for the LQR part actually fitted.
- No attempt to make port F's mode list resemble IMX219's.

## Decisions

### D1. Write a fresh tegracam driver, porting register logic from upstream — rather than adapting an existing Jetson IMX296 driver

Existing Jetson IMX296 work was surveyed before deciding:

| Candidate | Verdict |
|---|---|
| `pahomov-and/imx296_for_nvidia` — tegracam, **kernel 4.9**, patch form | **Rejected as a base.** Despite the perfect kernel/framework match, it drives a *Vision Components* module: it talks to a separate FPGA/ROM i2c client (`reg_write(rom, …)`) and reads a VC ROM table for module identity. Our module is a bare IMX296 with no such FPGA. Its *structure* is still a useful reference for tegracam-on-4.9 shape. |
| Upstream mainline `imx296.c` | **Rejected as a base, adopted as the register authority.** Wrong framework and wrong kernel era, but it is the authoritative, field-proven register map. |
| `clydemcqueen/imx296`, `henryjliu/IMX296-Innomaker`, `aliejabbari/…` | Rejected — all target Orin / JetPack 6 (kernel 5.15) and/or Innomaker VC modules. |

So: take the tegracam skeleton from this tree's own `imx219.c` (probe, `tegracam_ctrl_ops`,
`camera_common_sensor_ops`, power get/put, mode plumbing) and fill it with upstream's IMX296
register semantics. This keeps every kernel API 4.9-correct while the sensor-specific parts come
from a source that demonstrably works on real IMX296 silicon.

### D2. Register model taken verbatim from upstream

- **Init**: upstream's `imx296_init_table` (~40 undocumented 8-bit registers, e.g. `0x3005=0xf0`,
  `0x31c8=0xf3`) written at stream start. These are unpublished tuning values; they are copied
  exactly, not guessed.
- **Timing**: `VMAX` (24-bit @ `0x3010`) = frame length, `HMAX` (16-bit @ `0x3014`) = line length.
- **Exposure**: `SHS1` (24-bit @ `0x308d`), expressed as a shutter offset from `VMAX` — so exposure
  must be clamped against the current `VMAX`, not against a constant.
- **Gain**: `GAIN` (16-bit @ `0x3204`), range 0–480 in 0.1 dB steps (0–48 dB analog).
- **Standby / group hold**: `CTRL00` (`0x3000`, `STANDBY` bit) and `CTRL08` (`0x3008`, `REGHOLD`)
  — `REGHOLD` backs tegracam's `set_group_hold`.
- **Identification**: `SENSOR_INFO` (`0x3148`), model `(v>>6)&0x1ff == 296`, mono flag `BIT(15)`.
  Must exit standby and settle first, since the register reads zero in standby — this is exactly the
  behaviour observed on the bench (`0x0000` in standby, `0x4a00` after `CTRL00=0`).
- **Also required at mode set** (easily missed, all upstream): `GTTABLENUM = 0xc5`,
  `CTRL418C` = the per-INCK value from the clock table, `GAINDLY = GAINDLY_NONE`,
  `BLKLEVEL = 0x03c`, and `HMAX = 1100`.

**Sensor timing model (the part that is counter-intuitive).** `HMAX` is a line length expressed in
units of an *internal reference fixed at 74.25 MHz regardless of the external INCK*; `VMAX` is a
frame length in lines. So line/frame arithmetic is written against 74.25 MHz, not the board's input
clock. Nominal MIPI rate is 1188 Mbps (the sensor picks 1122–1198 internally and does not report it).

The datasheet's own All-pixel scan figures close the loop exactly:

| Quantity | Datasheet | Derived |
|---|---|---|
| `HMAX` | 44Ch = 1100 | 1H period = 1100 / 74.25 MHz = **14.815 µs** |
| `VMAX` | 45Eh = 1118 lines | = 1088 active + 30 vblank |
| Frame rate | **60.3 fps** | 1 / (1118 × 14.815 µs) = **60.38 fps** ✓ |

Computing 60.38 from `HMAX`/74.25 MHz and landing on the datasheet's stated 60.3 is what proves the
74.25 MHz reference is INCK-independent — and it is the basis of the INCK measurement in D4.

**Vertical frame structure emitted per frame** (upstream, from the datasheet) — this determines the
VI configuration:

| Lines | Content | CSI-2 DT |
|---|---|---|
| 1 | FS packet | — |
| **2** | **embedded data** | **0x12** |
| 6 | null | 0x10 |
| 4 | vertical effective optical black | 0x37 |
| 8…1088 | active image data | 0x2b (RAW10) |
| 1 | FE packet | — |
| ≥16 | vertical blanking | — |

### D3. Do not touch the shared reset GPIO at all — rather than replicating patch 0001's `-EBUSY` dance

Patch `0001` had to fight the reset line because the stock tegracam `imx219` driver makes
`reset-gpios` mandatory and requests it exclusively. For a driver written from scratch there is no
such obligation: the IMX296 node simply **declares no `reset-gpios`**, and the driver never
requests, frees, or drives the line.

This is strictly safer than tolerating `-EBUSY`: there is no code path by which the IMX296 can pull
the shared line low and reset the five IMX219s. The line is already released high at boot by the
`j106-camera-reset-release` gpio-hog, which is all the IMX296 needs. Software standby via `CTRL00`
gives per-sensor control without any GPIO.

*Alternative considered:* mirror `0001`'s tolerate-`-EBUSY`-never-free logic. Rejected as pure
downside — more code, more risk, no benefit, since nothing requires the IMX296 to assert reset.

### D4. INCK frequency is a device-tree property, defaulting to 54 MHz

`INCKSEL(0..3)` at `0x3089` must match the actual input clock; upstream tabulates only three:

| INCK | `INCKSEL(0..3)` bytes | `CTRL418C` |
|---|---|---|
| 37.125 MHz | `80 0b 80 08` | 116 |
| 54 MHz | `b0 0f b0 0c` | 168 |
| 74.25 MHz | `80 0f 80 0c` | 232 |

Sony's flyer independently confirms these are the *only* three legal input frequencies, so the
property is a closed three-way choice, not an open parameter.

The module is self-clocked and its oscillator frequency is not yet known (the on-board EEPROM at
`0x50` reports P/N `699-83310-1000-D02`, which resolves to no public part). Rather than hard-code a
guess, the driver reads an `inck-frequency` property, accepts only the three tabulated values, and
defaults to **74.25 MHz** — which is both the sensor's own power-on reset default for all four
`INCKSEL` registers and `CTRL418C`, *and* the setting that makes the first capture a direct readout
of the true INCK (below).

**Measuring INCK instead of guessing it.** `INCKSEL` tells the sensor what it is being fed; if the
value is wrong, the internal reference — and with it both frame timing and the MIPI bit rate — scales
by (actual INCK / assumed INCK). Because the frame rate at the correct setting is a known 60.38 fps,
leaving `INCKSEL` at the 74.25 MHz default turns observed frame rate into a direct measurement:

| Observed fps at `INCKSEL` = 74.25 | Actual INCK | Then set `inck-frequency` to |
|---|---|---|
| ≈ 60.4 | 74.25 MHz | 74250000 (already correct) |
| ≈ 43.9 | 54 MHz | 54000000 |
| ≈ 30.2 | 37.125 MHz | 37125000 |

The three candidates are far enough apart to be unambiguous. Caveat: the MIPI rate scales too
(1188 → 864 or 594 Mbps), so if NVCSI's D-PHY settle timings do not tolerate the deviation the
symptom may be CSI errors rather than clean slow frames — in which case fall back to trying the
three values in order. Either way one boot resolves it, and it is a DT change, not a driver rebuild.

### D5. Carry over the two J106-specific DT lessons from the IMX219 bring-up

Both were hard-won (README §5) and both are properties of the *platform*, not of IMX219:

- **`discontinuous_clk = "no"`** — t186 NVCSI polices D-PHY LP sequences and stalls unless bypassed.
  This is the single fix that made IMX219 streaming work at all here, so port F gets it from the
  start rather than rediscovering it.
- **Unique `position` / badge for `module5`** — identical EEPROM-less sensors otherwise collapse to
  one Argus camera. The IMX296 entry keeps a distinct identity.

### D6. Single-lane routing is expressed consistently at all three hops

`bus-width = <1>` must be set on the sensor endpoint, the NVCSI `channel@5` endpoint, *and* the VI
`port@5` endpoint — the TX2 3-hop graph validates lane width at each hop, and a mismatch surfaces
as a confusing capture-time failure rather than a probe error. `num_csi_lanes` becomes
`5×2 + 1×1 = 11`.

### D7. Declare the sensor at `0x18` (the common address), not `0x34`

Both work today, but they are not equivalent. `0x34` derives from the `SLAMODE`-strapped address
`0x36`, so it moves to `0x35` on any module strapped SLAMODE=1. `0x18` derives from the common
address `0x1a`, which the datasheet defines as valid in *both* SLAMODE polarities — and `0x1a` is
also the address every other IMX296 driver and module vendor uses. Declaring `0x18` therefore keeps
the DT node correct if the module is ever swapped for a differently-strapped one.

## Risks / Trade-offs

- **INCK frequency is a guess until measured** → D4 makes it a DT property with a falsifiable
  signature; first capture measures fps and confirms or corrects it. Worst case is wrong frame
  timing, not a non-booting board.
- **"Self-clocked" — now datasheet-supported.** The datasheet's power-on sequence requires INCK to
  be running *before* register communication is possible (step 3 starts INCK, step 4 then permits
  register access). We performed successful register communication — including a standby transition
  and a valid `SENSOR_INFO` read — with Tegra's `extperiph1` provably disabled, so the module must
  be supplying its own INCK. Residual risk is low; if it somehow proves otherwise, the change is
  blocked on a clock-tree conflict with the IMX219s and must be escalated, not worked around.
- ~~**`embedded_metadata_height` is not obviously transferable**~~ → **Resolved from the datasheet.**
  The IMX296 *does* emit **2 lines of embedded data (DT 0x12)** per frame, so
  `embedded_metadata_height = "2"` — the same value the IMX219s use, and for the same reason. The
  earlier plan to start at `"0"` was wrong and would have mis-framed every capture by two lines.
  (The 6 null and 4 optical-black lines are separate data types and are not counted here.)
- **Init table is undocumented** → copied verbatim from upstream and not modified. Any deviation
  would be unverifiable, so none is attempted.
- **Adding a built-in driver requires a kernel rebuild** → the `LOCALVERSION=-tegra` /
  `4.9.337-tegra` version string must be preserved or out-of-tree modules stop loading; this is an
  existing, documented build invariant and is asserted before deploy.
- **Port F's `/dev/video*` index may shift** → five nodes exist now (`video0..4`); adding port F
  changes what downstream tooling sees. The repo's capture tooling already places cameras by fixed
  port rather than node order, which limits the blast radius.
- **Bricked boot from a bad DTB/Image** → the reversible `extlinux` LABEL flow plus the documented
  U-Boot menu recovery over `/dev/ttyUSB0` bounds this; the partition DTB is never written.

## Migration Plan

1. Apply `patches/0001-*` then `patches/0002-*` to a clean `ksrc`; both must apply without fuzz.
2. Enable `CONFIG_VIDEO_IMX296=y` in `board-config` (derived from the board's own `/proc/config.gz`).
3. Build `Image`; assert `strings Image | grep 4.9.337-tegra`.
4. Build the carrier DTB via the established Approach A (decompile stock → append the camera and USB
   dtsi → recompile).
5. Deploy `Image` → `/boot/Image.j106`, DTB → `/boot/tegra186-j106.dtb`; add/refresh the
   `LABEL j106cam` entry while **keeping the previous working LABEL** as fallback; reboot.
6. Verify in order: IMX296 binds at `7-0018` → all five IMX219s still bind → raw V4L2 capture from
   port F (`bypass_mode=0`) → measured fps confirms INCK → Argus enumerates 6 distinct cameras.
7. **Rollback:** select the previous LABEL at the U-Boot extlinux menu over `/dev/ttyUSB0` and
   restore `DEFAULT`. No partition DTB was touched, so no reflash is involved.

## Open Questions

- **Which INCK does the module's oscillator actually run at?** Still open, and the datasheet
  *cannot* close it: the input clock is a property of the module's oscillator, and the sensor
  provides no register reporting it. What the datasheet does close is the search space — exactly
  three legal values — so D4 makes it a DT property tried in order 54 → 37.125 → 74.25 during
  bring-up. Deferrable: it changes no spec, no approach, and no task.
- ~~**Does the module need `embedded_metadata_height = "2"`?**~~ **Answered: yes, `"2"`.** The
  sensor emits two lines of DT 0x12 embedded data per frame.
- ~~**What is the `0x34` alias?**~~ **Answered by the datasheet.** The IMX296 answers at *two*
  addresses simultaneously: a `SLAMODE`-strapped address (`0x36` for SLAMODE=0, `0x37` for
  SLAMODE=1) and a common address `0x1a` valid in both polarities. Through the carrier's XOR-bit-1
  shifter these appear as `0x34` and `0x18` — exactly the two we observed, which also tells us this
  module straps **SLAMODE = 0**. See D7 for why the node uses `0x18`.
