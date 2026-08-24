## 1. Baseline and safety net

- [ ] 1.1 Record the pre-change baseline from the board: bound IMX219 clients (`1-0010`, `1-0012`, `2-0010`, `2-0012`, `7-0010`), `/dev/video0..4`, and a working Argus enumeration — this is the regression reference for ports A–E
- [ ] 1.2 Confirm the current `extlinux.conf` has a known-good fallback LABEL and note which LABEL is `DEFAULT`, so rollback is available before anything is deployed
- [ ] 1.3 Verify `patches/0001-imx219-share-reset-gpio-j106.patch` applies cleanly to a pristine `ksrc` checkout, establishing the clean-tree starting point for patch 0002

## 2. Driver: skeleton and identification

- [ ] 2.1 Create `kernel/nvidia/drivers/media/i2c/imx296.c` as a tegracam driver skeleton modelled on this tree's `imx219.c` — `struct imx296` (s_data, tc_dev, regmap), `of_device_id` with `compatible = "nvidia,imx296"`, `i2c_device_id`, `module_i2c_driver`
- [ ] 2.2 Add a 16-bit-address / 8-bit-data regmap plus read/write helpers, including multi-byte little-endian helpers for the 16- and 24-bit registers (`VMAX`, `HMAX`, `SHS1`, `GAIN`, `SENSOR_INFO`)
- [ ] 2.3 Implement `imx296_board_setup()` identification: exit standby via `CTRL00=0`, settle, read `SENSOR_INFO` (`0x3148`), require `(v>>6)&0x1ff == 296`, record and log the mono/colour variant from `BIT(15)`, and fail probe on mismatch
- [ ] 2.4 Implement power get/put and power on/off that use software standby (`CTRL00`) only — explicitly **no** `reset-gpios` request, no GPIO free, and no driving the shared reset line (design D3)
- [ ] 2.5 Implement `imx296_parse_dt()` reading `inck-frequency`, accepting only 37125000 / 54000000 / 74250000 and defaulting to **74250000** (the sensor's own reset default, and the reference that makes fps a direct INCK measurement), storing the matching `INCKSEL(0..3)` bytes and `CTRL418C` value (design D4)

## 3. Driver: modes, controls, streaming

- [ ] 3.1 Add the upstream init table verbatim (~40 registers from `0x3005` through `0x40c8`) as a static register table, with a comment recording that it is copied unmodified from upstream and must not be tuned
- [ ] 3.2 Define the single 1456×1088 RAW10 mode and its `camera_common_frmfmt` / mode-table entries, with the Bayer order for the IMX296LQR colour part actually fitted; advertise the datasheet's **60.3 fps** for All-pixel scan mode (derived: 1/(1118 × 1100/74.25 MHz) = 60.38)
- [ ] 3.3 Implement `imx296_set_mode()` writing the init table, the selected `INCKSEL(0..3)` bytes and matching `CTRL418C` (74.25→E8h / 54→A8h / 37.125→74h), `GTTABLENUM = 0xc5`, `GAINDLY = NONE`, `BLKLEVEL = 0x03Ch`, `HMAX = 1100` (44Ch) and `VMAX = 1118` (45Eh = 1088 + 30 vblank) — noting `HMAX` is in units of the internal reference fixed at 74.25 MHz, *not* the board INCK
- [ ] 3.4 Implement `set_gain` against `GAIN` (`0x3204`), clamping to the sensor's 0–480 (0–48 dB) range
- [ ] 3.5 Implement `set_exposure` against `SHS1` (`0x308d`) as an offset from the current `VMAX`, clamping to what the active frame length permits rather than to a constant
- [ ] 3.6 Implement `set_frame_rate` by adjusting `VMAX`, and `set_group_hold` via `CTRL08` `REGHOLD`
- [ ] 3.7 Implement `start_streaming` / `stop_streaming` via `CTRL00` standby, and wire `tegracam_ctrl_ops` + `camera_common_sensor_ops` and `imx296_probe()` registration

## 4. Build-system integration

- [ ] 4.1 Add the `VIDEO_IMX296` entry to `kernel/nvidia/drivers/media/i2c/Kconfig`, matching the style of the neighbouring `VIDEO_IMX219` block
- [ ] 4.2 Add `obj-$(CONFIG_VIDEO_IMX296) += imx296.o` to that directory's `Makefile`
- [ ] 4.3 Add `CONFIG_VIDEO_IMX296=y` to `j106build/r3276/board-config`

## 5. Package as patch 0002

- [ ] 5.1 Generate `patches/0002-imx296-tegracam-j106.patch` covering the new `imx296.c` plus the `Kconfig` and `Makefile` edits, in the same format and path convention as patch 0001
- [ ] 5.2 Verify on a pristine `ksrc` that 0001 and 0002 both apply in sequence without conflict or fuzz, and that each reverts cleanly on its own

## 6. Device tree: port F

- [ ] 6.1 Add an `IMX296_HW_RESOURCES` / `IMX296_MODE0` macro pair to `tegra186-camera-j106-imx219.dtsi` mirroring the IMX219 macro style — `compatible = "nvidia,imx296"`, no `reset-gpios`, `inck-frequency`, dummy regulators, and `discontinuous_clk = "no"` (design D5)
- [ ] 6.2 Replace the `imx219_f@12` node with `imx296_f@18` on `i2c@c250000` — using `0x18` (the SLAMODE-independent common address `0x1a` through the shifter), *not* the `0x34` alias (design D7) — carrying the single 1456×1088 mode and the `j106_imx219_out5` → `j106_csi_in5` endpoint link
- [ ] 6.3 Set `bus-width = <1>` on all three hops for port F — the sensor endpoint, `nvcsi` `channel@5` endpoint, and `vi` `port@5` endpoint (design D6)
- [ ] 6.4 Update `tegra-camera-platform`: `num_csi_lanes` 12 → 11, and rewrite `module5` (badge, `position`, `devname` `imx296 7-0018`, `proc-device-tree` path) keeping its identity unique for Argus
- [ ] 6.5 Set `embedded_metadata_height = "2"` for the IMX296 node — the sensor emits two lines of DT 0x12 embedded data per frame (design D2 frame-structure table); the 6 null (DT 0x10) and 4 optical-black (DT 0x37) lines are separate data types and are not counted here

## 7. Build and deploy

- [ ] 7.1 Build the kernel `Image` with both patches applied and assert `strings Image | grep 4.9.337-tegra`
- [ ] 7.2 Build the carrier DTB via Approach A (decompile stock → append camera + USB dtsi → recompile), checking the dtc log for errors beyond the expected decompile warnings
- [ ] 7.3 Deploy `Image` → `/boot/Image.j106` and the DTB → `/boot/tegra186-j106.dtb`, refresh `LABEL j106cam`, keep the prior working LABEL as fallback, and reboot

## 8. Verification

- [ ] 8.1 Confirm the IMX296 binds as `imx296 7-0018` and the probe log reports model 296 / colour variant
- [ ] 8.2 Confirm **no regression**: all five IMX219s still bind and still capture, matching the 1.1 baseline
- [ ] 8.3 Capture raw V4L2 frames from port F at 1456×1088 with `bypass_mode=0` and confirm correct frame size with no CSI/VI errors
- [ ] 8.4 **Measure INCK** with `inck-frequency` left at the 74.25 MHz default: observed ≈60.4 fps ⇒ INCK is 74.25 MHz (already correct), ≈43.9 ⇒ 54 MHz, ≈30.2 ⇒ 37.125 MHz. Set `inck-frequency` to the indicated value and re-verify. If instead the link throws CSI errors (the scaled MIPI rate may fall outside NVCSI's D-PHY settle tolerance), fall back to trying the three values in order (design D4)
- [ ] 8.5 Confirm Argus enumerates six distinct cameras with port F among them, and that exposure and gain controls take effect
- [ ] 8.6 Confirm the deployed DTB left the partition DTB untouched and that the fallback LABEL still boots

## 9. Documentation

- [ ] 9.1 Update `README.md` §5 with the IMX296 bring-up findings (shifted address `0x18`, self-clocked module, single-lane routing, the resolved INCK value) and §7 with the mixed 5×IMX219 + 1×IMX296 status
- [ ] 9.2 Record the confirmed `inck-frequency` in the dtsi comments, along with the frame-structure rationale for `embedded_metadata_height = "2"`, so the reasoning is not lost
