## Why

CSI port F of the J106 has been physically repopulated: the IMX219 that used to sit at `7-0012`
is gone, and a **Sony IMX296LQR global-shutter module** now occupies the port. The running kernel
has no `imx296` driver at all, so the sensor is invisible to V4L2 and Argus — the board dropped
from 6 working cameras to 5 (`/dev/video0..4`), and the global-shutter capability the module was
fitted for (no rolling-shutter skew on fast motion / vibration) is unusable.

Hardware facts confirmed on the live board (2026-08-24), not assumed:

| Fact | Evidence |
|---|---|
| Sensor is IMX296, **colour** variant (LQR) | `SENSOR_INFO`(0x3148) = `0x4a00` → model `(v>>6)&0x1ff` = **296**, MONO bit clear — matches mainline `IMX296_SENSOR_INFO_IMX296LQ` |
| Lives at **`0x18` on i2c-7** | `i2cdetect -y -r 7`; datasheet common address `0x1a` mapped by the carrier address-shifter (XOR of addr bit 1, same rule that maps IMX219 `0x10`→`0x12`) |
| `0x34` is the **same die**, not a 2nd sensor | writing `CTRL00`=STANDBY at `0x18` changed the value read back at `0x34`. Datasheet explains it: the sensor answers at both a SLAMODE-strapped address (`0x36`, so SLAMODE=0 here) and a common address (`0x1a`), which the shifter maps to `0x34` and `0x18` |
| Module is **self-clocked** | `extperiph1` `enable_count=0` / `prepare_count=0` while the sensor still left standby and answered — it does *not* need the shared 24 MHz MCLK |
| Port F is otherwise wired as before | `vi@15700000` `port@5` ↔ `nvcsi` `channel@5`, ports A–E still IMX219 (`1-0010,1-0012,2-0010,2-0012,7-0010`) |

The self-clocking result is what makes this change tractable: IMX296 accepts only 37.125 / 54 /
74.25 MHz INCK, none of which is the 24 MHz the J106 fans out to all six sensors. Because the
module brings its own oscillator, port F can host an IMX296 **without disturbing the five IMX219s**.

## What Changes

- **New kernel driver** `imx296.c` for L4T R32.7.6 / kernel `4.9.337-tegra`, written against
  NVIDIA's **tegracam** framework (`tegracam_core.h`, `camera_common`) so the sensor is usable by
  both raw V4L2 and Argus/ISP — matching how `imx219.c` and `ov9281.c` are structured in this tree.
  Register-level behaviour (init sequence, exposure/gain/vblank model, `SENSOR_INFO` identification)
  follows the full Sony datasheet now in the repo (`IMX296LQR-C_Fulldatasheet_Awin.pdf`), with
  upstream mainline `imx296.c` as the cross-check — the datasheet independently confirms every
  register value mainline uses.
- **Delivered as `patches/0002-imx296-tegracam-j106.patch`**, consistent with the repo's existing
  deploy flow. The patch adds `imx296.c` and the `Kconfig` / `Makefile` entries
  (`CONFIG_VIDEO_IMX296`); `patches/0001-*` (IMX219 shared reset) stays untouched and independent.
- **Device tree**: port F's node in `tegra186-camera-j106-imx219.dtsi` changes from
  `imx219_f@12` to an `imx296_f@18` node — `compatible = "nvidia,imx296"`, one IMX296 mode
  (1456×1088 RAW10) replacing the five IMX219 modes.
- **BREAKING (port F only)**: port F's resolution/format set changes from IMX219's
  3280×2464 … 1280×720 RAW10 Bayer **RGGB** to IMX296's single 1456×1088 RAW10. Anything pinning
  port F to an IMX219 mode, or to `/dev/video5` ordering, must be updated. Ports A–E are unaffected.
- **CSI lane budget**: port F's endpoint drops from `bus-width = <2>` to `<1>` (IMX296 is a
  single-lane sensor at 1188 Mbps), and `tegra-camera-platform`'s `num_csi_lanes` goes 12 → 11.
- **Shared-reset compatibility**: the J106 ties all six camera resets to one GPIO (461), already
  released high at boot by the `j106-camera-reset-release` hog. The IMX296 node therefore declares
  **no `reset-gpios` at all** and the driver never requests, frees, or drives that line — so there
  is no code path by which port F can reset the five IMX219s out from under them (see design D3).
  Per-sensor control uses the IMX296's software standby instead.
- **Documentation**: README §5/§7 updated to record the mixed 5×IMX219 + 1×IMX296 configuration.

## Capabilities

### New Capabilities
- `imx296-camera`: IMX296 global-shutter sensor support on a J106 CSI port — sensor identification,
  the tegracam driver contract (modes, exposure/gain controls, streaming), single-lane CSI routing,
  coexistence with the IMX219s on the shared reset line and shared MCLK, and the packaging of the
  driver as a reproducible patch.

### Modified Capabilities
<!-- None. openspec/specs/ is currently empty — the existing IMX219 behaviour has never been
     captured as a spec, and this change does not alter it (ports A-E keep working unchanged),
     so there is no existing spec whose requirements change. -->

## Impact

**New files**
- `patches/0002-imx296-tegracam-j106.patch` — the driver, as a patch (adds
  `kernel/nvidia/drivers/media/i2c/imx296.c`, edits that directory's `Kconfig` and `Makefile`).

**Modified files**
- `tx2-j106-6csi/tegra186-camera-j106-imx219.dtsi` — port F sensor node, CSI-F endpoint lane width,
  `num_csi_lanes`, and the `tegra-camera-platform` `module5` badge/position/devname entries.
- `j106build/r3276/board-config` — add `CONFIG_VIDEO_IMX296=y`.
- `README.md` — §5 (issues & fixes), §7 (status).

**Systems affected**
- Kernel `Image` must be rebuilt and redeployed (new built-in driver), using the existing reversible
  `extlinux.conf` LABEL flow — the partition DTB is still never touched.
- Argus: port F becomes a distinct camera with its own resolution set; the `position`-uniqueness fix
  that stopped the IMX219s aliasing to one Argus camera must be preserved for the new module.
- Deployment risk is bounded: a bad DTB or Image is recoverable via the U-Boot extlinux menu on
  `/dev/ttyUSB0` (the documented boot-hang recovery path).

**Explicit non-goals**
- No change to ports A–E, to `patches/0001-*`, or to the IMX219 driver.
- No mono (IMX296LLR) support, no external-trigger / strobe mode, no multi-camera hardware sync —
  the module is colour and free-running; those can follow later.
