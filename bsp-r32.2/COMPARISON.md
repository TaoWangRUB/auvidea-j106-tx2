# Auvidea J106/TX2 BSP vs official NVIDIA L4T — R32.2.1 (JetPack 4.2.2)

Comparison done for **path 2** (flash R32.2 to test whether the older libargus fixes the
Argus multi-camera halving). Artifacts downloaded to this dir (`bsp-r32.2/`):

| File | Size | Source |
|------|------|--------|
| `J90_v2.2_4.2.2.tar.bz2` | 426 MB | `https://auvidea.eu/download/firmware/TX2/v2.2/J90_v2.2_4.2.2.tar.bz2` |
| `Jetson_Linux_R32.2.1_aarch64.tbz2` | 131 MB | `https://developer.download.nvidia.com/embedded/L4T/r32-2-1_Release_v1.0/TX2-AGX/` |
| `Tegra_Linux_Sample-Root-Filesystem_R32.2.1_aarch64.tbz2` | 1.27 GB | same NVIDIA base |

JetPack mapping: **4.2 = R32.1.0**, 4.2.1 = R32.2.0, **4.2.2 = R32.2.1** (this comparison, and the
RidgeRun J106 guide version), 4.2.3 = R32.2.3. The Auvidea J106/TX2 package is in the **"J10x"**
carrier family (`J9x/J10x/J120/J130-NV/J140`). An older 4.2/R32.1 package also exists
(`J120_4.2.zip`); R32.2.1 chosen because it matches the libargus we want to test and the RidgeRun guide.

## What the Auvidea BSP is

An **overlay** (37 files) extracted on top of a full official `Linux_for_Tegra`. Extracts to
`J90_4_2_2/`. The file set *is* the list of things Auvidea changes vs stock.

## Diff vs stock R32.2.1 (md5)

| File | Status | Notes |
|------|--------|-------|
| `kernel/dtb/tegra186-quill-*.dtb` (all) | **CHANGED** | USB-only edits — see below |
| `kernel/dtb/tegra194-*.dtb` (Xavier) | CHANGED | not relevant to TX2 |
| `kernel/Image` | **CHANGED** | Auvidea's own kernel build |
| `kernel/kernel_supplements.tbz2` | **CHANGED** | matching modules |
| `p2771-0000.conf.common` | **CHANGED** | ODMDATA — see below |
| `kernel/kernel_headers.tbz2` | same | |
| `kernel/pinmux/t186/*`, `t19x/*` | same | pinmux sources untouched |

Plus a standalone `kernel_src.tar.bz2` (Auvidea's full patched kernel source).

### 1. Flash config (`p2771-0000.conf.common`) — only one line differs
```
- ODMDATA=0x1090000;   # stock C0X
+ ODMDATA=0x3090000;   # Auvidea
```
ODMDATA encodes the Tegra186 UPHY lane / USB-PCIe mapping. This is the carrier USB routing — the
same domain as our `override-usb.dtsi` VBUS fix.

### 2. c03 carrier DTB (`tegra186-quill-p3310-1000-c03-00-base.dtb`) — USB only
Decompiled both, diff = **310 lines, 100% USB** (`xhci`/`xusb_padctl` port enablement,
`vbus-supply` phandle swaps, `e3325-*` → `usb3-std-A-port*` renames, `status="okay"` on USB2/USB3
ports). **Camera/CSI/VI/I2C content in the diff = 0.** The imx185/219/274/318/390 nodes are
**identical** in both DTBs — they are NVIDIA's stock devkit placeholders, **not** a J106 6-camera config.

### 3. Kernel
- Auvidea: `Linux version 4.9.140` (built by `auvidea@auvidea-T5600`, gcc 6.4 Linaro, Sep 2019),
  **LOCALVERSION empty** → modules in `/lib/modules/4.9.140`.
- Official: `4.9.140-tegra` → `/lib/modules/4.9.140-tegra`.

## Bottom line for path 2

1. **The Auvidea BSP does NOT provide cameras.** It fixes only **USB** (DTB port enablement +
   `ODMDATA=0x3090000`) and ships a custom 4.9.140 kernel. The 6× IMX219 J106 bring-up
   (our `tegra186-camera-j106-imx219.dtsi` + imx219 shared-reset patch) is **still required** on
   R32.2.1 — nothing here does it for us.
2. **The USB work we already did on R32.7.6 (override-usb.dtsi) corresponds to Auvidea's changes.**
3. **The actual reason to flash R32.2.1 is the older libargus** (test the single-ISP multicam-halving
   hypothesis). That lives in the **rootfs NVIDIA userspace** (`apply_binaries.sh` → `nv_tegra`),
   **not** in this BSP file diff — so the diff can't confirm/deny the halving fix; only a flash + run can.

## Two flashing routes (both land on R32.2.1 libargus)

- **A — Auvidea BSP**: USB works out of the box; but their non-`-tegra` 4.9.140 kernel means our
  imx219 patch must be rebuilt against `kernel_src.tar.bz2`, and our camera dtsi ported in.
- **B — Stock NVIDIA R32.2.1** + our carrier DT (port 6-cam dtsi + USB override + `ODMDATA=0x3090000`)
  + rebuild imx219 patch on stock R32.2.1 kernel source. Keeps the known-good `-tegra` kernel and our
  proven DT approach; fewer moving parts for isolating the libargus question. **Recommended.**
