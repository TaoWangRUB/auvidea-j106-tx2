# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## What this repo is

This is **not** an application codebase — it is a **Jetson TX2 hardware bring-up project**. The
goal: get **6× CSI IMX219 cameras** (plus USB) working on a Jetson TX2 module sitting on an
Auvidea **J106 + M110** carrier, by building a custom **device tree (DTB)** and a small **kernel
driver patch** on top of the already-installed **L4T R32.7.6** (kernel `4.9.337-tegra`). No OS
downgrade. The deliverables are a `.dtsi`, a `.patch`, and a build/deploy procedure — there is no
code to "run" locally; everything is cross-built here and deployed to the board over SSH.

**`README.md` is the authoritative project reference.** Read it first — **§5** (camera bring-up:
issues & fixes by pipeline stage), **§6** (build, apply patch & flash), **§7** (status & open issues).
The detailed chronological investigation log lives in this file's git history. Keep README.md updated
as the source of truth when state changes.

## Repo layout (the files that matter)

| Path | What it is |
|------|------------|
| `tx2-j106-6csi/tegra186-camera-j106-imx219.dtsi` | The 6-camera device tree (the core deliverable). Defines sensors, NVCSI channels, VI ports, the shared-reset gpio-hog, and the EXTPERIPH1 MCLK pinmux. |
| `tx2-j106-6csi/override-usb.dtsi` | USB VBUS fix — forces the fixed regulator always-on so xusb_padctl stops deferring on the absent devkit `pca953x` expander. |
| `tx2-j106-6csi/build-dtb.sh` | DTB build helper (Approach B: cpp + dtc from NVIDIA DTS sources). |
| `patches/0001-imx219-share-reset-gpio-j106.patch` | Kernel driver patch so all 6 sensors can share the J106's single hardware reset line. |
| `headless-first-boot-setup.md` | NVIDIA oem-config first-boot over micro-USB serial — required before any DTB work on a freshly-flashed board. |
| `*.pdf` | Auvidea/NVIDIA hardware references (J106, M110, XCB-Lite, TX1 flashing). |
| `j106build/` | **git-ignored** build tree: L4T sources, toolchain, kernel build output, DTBs. Reproducible; do not commit. |

## The two changes being made (and why both are needed)

1. **Device tree** (`.dtsi` files) — the stock devkit DTB describes NVIDIA's p2597 devkit, not the
   J106/M110 carrier. It has no camera nodes, and it routes USB VBUS through devkit-only I²C GPIO
   expanders (`pca953x` @ `0x74`/`0x77`) absent on the carrier, so USB defers forever (`-517`).
   The carrier DTB adds the 6 cameras and fixes VBUS.
2. **Driver patch** (`.patch`) — the modern R32 *tegracam* `imx219` driver makes `reset-gpios`
   mandatory and `gpio_request()`s it **exclusively** (one consumer per pin). The J106 ties **all
   6** camera resets to **one** line, so only the first sensor probes; the rest fail `-EBUSY`. The
   patch makes the driver treat `-EBUSY` as success, never free the line, and **never drive it
   low** (release once, leave high — mirroring how the working TX1 BSP behaves).

So this is **device-tree + one small driver patch**, not a pure DT job.

## Camera pipeline (the "big picture" that spans files)

TX2 (tegra186) uses a **3-hop** capture graph, unlike TX1's 2-hop:

```
sensor (imx219 @0x10/0x12) --remote-endpoint--> nvcsi@150c0000 channel@N --> vi@15700000 port@N --> /dev/videoN
```

- 6 sensors across **3 i2c buses, 2 per bus** (`c240000`→i2c-1, `3180000`→i2c-2, `c250000`→i2c-7).
  A carrier address-shifter (shift=2) puts the "south" sensor of each pair at `0x12` instead of
  `0x10`, allowing 6 identical sensors.
- All 6 share **one reset GPIO** = `TEGRA_MAIN_GPIO(R,5)` = sysfs gpio 461, released high once by a
  gpio-hog named `j106-camera-reset-release` (a *unique* name — a name colliding with the stock
  disabled `camera-control-output-high` hog silently merges and does nothing).
- All 6 share **one 24 MHz MCLK** from `extperiph1`. The MCLK pin must be in **SFIO** (not GPIO)
  mode via pinmux under `pinmux@2430000/common` — a standalone pinmux group is ignored because
  nothing references it in `pinctrl-0`.
- The vi/nvcsi channels inherit `disabled` from the stock tree merge; they must be set
  `status="okay"` or there is no `/dev/video*`.

When editing `tegra186-camera-j106-imx219.dtsi`, these facts (buses, gpio, MCLK, okay-status) are
load-bearing and cross-checked against live hardware in README §3b/§4/§5 — change them only with a
documented reason.

## Build & deploy workflow

Builds happen on an **x86-64 Linux host** (this repo machine); artifacts deploy to the **TX2 board
over SSH**. Deployment is **always reversible** via `extlinux.conf` LABELs — the partition DTB
(`mmcblk0p30`) is never touched.

> ⚠️ **Path discrepancy to be aware of:** README §6 uses
> `/home/taowang/workspace/auvidea-j106-tx2/j106build/` (this repo), while `build-dtb.sh` and the
> memory notes reference `/tmp/j106build/` on a separate WSL host. Confirm which build tree exists
> before running anything; prefer the in-repo `j106build/`.

**Build the patched kernel Image** (only change is the imx219 patch; config comes from the board's
own `/proc/config.gz` to keep modules compatible — must yield `4.9.337-tegra`):
```bash
cd j106build/r3276
patch -p1 -d ksrc < patches/0001-imx219-share-reset-gpio-j106.patch
cp board-config kout/.config
export CROSS_COMPILE=$PWD/l4t-gcc/bin/aarch64-buildroot-linux-gnu-   # NVIDIA Bootlin GCC 9.3
make -C ksrc/kernel/kernel-4.9 O=$PWD/kout ARCH=arm64 CROSS_COMPILE=$CROSS_COMPILE LOCALVERSION=-tegra olddefconfig
make -C ksrc/kernel/kernel-4.9 O=$PWD/kout ARCH=arm64 CROSS_COMPILE=$CROSS_COMPILE LOCALVERSION=-tegra -j$(nproc) Image
# Result: kout/arch/arm64/boot/Image  — verify: strings Image | grep 4.9.337-tegra
```

**Build the carrier DTB — Approach A (used here): decompile stock DTB + overlay.** Starts from the
board's *actual* running tree so nothing is lost:
```bash
cd j106build
dtc -I dtb -O dts stock-c03.dtb -o stock-c03.dts
# Add two DTS labels the camera dtsi needs (decompiled tree lacks them):
#   clock@5000000  -> tegra_car: clock@5000000
#   gpio@2200000   -> tegra_main_gpio: gpio@2200000
SRC=../tx2-j106-6csi
cat stock-c03.dts "$SRC/tegra186-camera-j106-imx219.dtsi" "$SRC/override-usb.dtsi" > tegra186-j106.dts
dtc -I dts -O dtb -@ -o tegra186-j106.dtb tegra186-j106.dts 2> tegra186-j106.dtc.log
# dtc phandle/unit-address warnings are expected for decompiled+recompiled trees.
```
(**Approach B** builds from NVIDIA DTS sources via `tx2-j106-6csi/build-dtb.sh in.dts out.dtb` —
see README §6.)

**Deploy reversibly** (copy `Image`→`/boot/Image.j106`, DTB→`/boot/tegra186-j106.dtb`, add an
`extlinux` `LABEL j106cam` pointing at them, keep a prior `LABEL` as fallback, set `DEFAULT
j106cam`, reboot). Full commands and rollback are in README §6.

**Board access — always confirm the active connection interface first.** The TX2 has been reached
several ways across sessions and the address depends on which is in use, so never assume a cached
IP. Check which interface is actually carrying the link before connecting:
- **M110 Ethernet (`eth0`)** — host-shared net puts the board on `10.42.0.x` (rediscover via
  `ip neigh show dev <usb-eth-iface>` or a ping sweep); an older direct LAN gave `192.168.0.168`.
- **Micro-USB device-mode gadget** — `192.168.55.1` (and `/dev/ttyACM*` on the host) after
  first-boot; currently not enumerating (the OTG device-mode issue, README §7 Open).
- **WiFi** — its own DHCP lease on the WiFi subnet.
- **Debug UART** (`/dev/ttyUSB0`, 115200 8N1) — the reliable serial fallback when no IP responds.

Specific addresses and the build-host layout live in the memory files, not here — re-verify the
live IP and interface each session.

## Current status & where the work is stuck

USB host ports **done & verified**. Cameras **work end-to-end** — raw V4L2 and Argus, all distinct.
Key dtsi fixes (README §5): `embedded_metadata_height="2"` (SMMU), **`discontinuous_clk="no"`** (the
streaming fix — t186 NVCSI polices D-PHY LP sequences unless bypassed; TX1 never did), 680 Mbps MIPI
rate (patch `0001`, marginal-link margin, matches Auvidea TX1), all-5-modes in driver order + derated
framerates (Argus ISP), and **unique `position` per module** (the Argus `sensor-id` aliasing fix —
identical EEPROM-less IMX219 all collapsed to `(GUID 0, position 0)` → one camera). Deliverable:
`captures/tx2_grid_5cam.mp4` (5-camera 2×3 grid, TX1 parity).

**Open issues (README §7):** (1) micro-USB device-mode (`/dev/ttyACM0` gadget, *not* `ttyS0`) not
enumerating — staged `override-usb.dtsi` `mode="device"` fix unverified; (2) carrier buttons not
working (uninvestigated `gpio-keys`); (3) UART0 debug console **works** (`/dev/ttyUSB0` @115200).
Intermittent (not bugs): south `0x12` cameras (B/D) per-boot enumeration lottery; Argus 5-session
start race (restart `nvargus-daemon` + retry). Board sudo: `echo nvidia | sudo -S <cmd>`.
