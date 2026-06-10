# Auvidea J106 — 6× CSI IMX219 cameras on Jetson TX2

This document captures the analysis of the existing **working TX1** setup and the
**plan + scaffold** to reproduce the same **6 CSI camera** support on **Jetson TX2**
on the Auvidea **J106 (+ M110)** carrier.

> TL;DR: On TX1 it works because Auvidea patched the kernel (IMX219 driver) and shipped a
> custom device tree wiring all 6 cameras across 3 i2c buses. On TX2 the **IMX219 driver
> already exists** in the L4T R32 kernel, so the bulk of the port is a **device‑tree job** —
> **plus one small driver patch** ([`patches/0001-imx219-share-reset-gpio-j106.patch`](patches/0001-imx219-share-reset-gpio-j106.patch))
> so the 6 sensors can share the J106's single hardware reset line (see §7). The board already
> runs **L4T R32.7.6** — we **stay on it** and build a single carrier DTB that fixes
> **cameras + USB** together (WiFi/Ethernet already work). **USB is already fixed & verified**
> (§7.1). The starting device tree is in
> [`tx2-j106-6csi/tegra186-camera-j106-imx219.dtsi`](tx2-j106-6csi/tegra186-camera-j106-imx219.dtsi).

> **New board / just flashed?** First complete NVIDIA's headless first-boot (oem-config) over
> the micro-USB serial — see
> [`headless-first-boot-setup.md`](headless-first-boot-setup.md). You need a login before any
> of the DTB work below.

---

## 1. Inventory of the supplied material

| Item | What it is |
|------|------------|
| `bsp_L4T_24_2_1_V2.2.tar.gz` + `*.patch` | **Working TX1** BSP. L4T **R24.2.1**, kernel **3.10.96**. |
| `Tegra210_Linux_R24.2.1_aarch64.tbz2` | NVIDIA L4T driver package for TX1 (base the TX1 BSP overlays). |
| `J90_v2.2_4.2.2.tar.bz2` | **TX2** BSP from Auvidea. L4T **R32.2.1 / JetPack 4.2.2**, kernel **4.9**. Boots J106 on TX2 but **does not** configure 6 CSI cameras. |
| `J106_technical_reference_1.0.pdf` | J106 carrier hardware. Documents the **i2c‑bus ↔ CSI** routing and the i2c **address shifter**. |
| `Firmware_installation_TX1_2.1.pdf` | TX1 flashing procedure (swap DTB + `flash.sh`). |

### How the TX1 BSP is structured (important)
- `J10x_J120.patch` / `J140.patch` are **trivial** — they only change the `FDT` line in
  `bootloader/.../extlinux.conf.emmc` to a custom DTB
  (`tegra210-jetson-auvidea-120-IMX219.dtb`).
- The real work lives in `bsp_L4T_24_2_1_V2.2.tar.gz → patch_sources.tar.gz`:
  - `0001-add-imx219-subdevice-driver.patch` — adds the IMX219 kernel driver (kernel 3.10).
  - `0002-add-imx219-dtb.patch` — adds `tegra210-imx219.dts` defining **all 6 cameras**.

---

## 2. The J106 6‑camera wiring (carrier hardware — identical for TX1 and TX2)

From the J106 technical reference and the TX1 DTS, the carrier routes the 6 CSI‑2
connectors to **3 i2c buses, 2 cameras per bus**. The carrier contains a unique
**i2c address shifter** (default shift = **2**), so the "south" camera on each bus
moves from `0x10` to `0x10 + 2 = 0x12`. This lets you use 6 identical sensors.

| Cam | J106 conn. | CSI port | serial iface | i2c addr | TX1 i2c bus | i2c "marked" |
|-----|-----------|----------|--------------|----------|-------------|--------------|
| A | CSI‑A | 0 | serial_a | 0x10 | `I2C0`  `i2c@7000c000` | 0‑N |
| B | CSI‑B | 1 | serial_b | 0x12 | `I2C0`  `i2c@7000c000` | 0‑S (shifted) |
| C | CSI‑C | 2 | serial_c | 0x10 | `I2C6`  `i2c@546c0000` | 6‑N |
| D | CSI‑D | 3 | serial_d | 0x12 | `I2C6`  `i2c@546c0000` | 6‑S (shifted) |
| E | CSI‑E | 4 | serial_e | 0x10 | `I2C2`  `i2c@7000c500` | 2‑N |
| F | CSI‑F | 5 | serial_f | 0x12 | `I2C2`  `i2c@7000c500` | 2‑S (shifted) |

Other carrier facts:
- All 6 connectors' **reset pin (pin 6) are tied together** and driven by **one GPIO**.
- `VI` uses `num-channels = <6>`, ports 0–5.
- Each link is **2‑lane** (`bus-width = 2`).
- The TC358743/B101 HDMI‑to‑CSI bridge is a separate device (0x0F) — out of scope here.

---

## 3. Why the TX1 device tree cannot be copied verbatim to TX2

| Aspect | TX1 (L4T R24, kernel 3.10) | TX2 (L4T R32.2.1, kernel 4.9) |
|--------|----------------------------|-------------------------------|
| IMX219 driver | **Added by Auvidea patch** | **Already present**: `drivers/media/i2c/imx219.c`, `imx219_mode_tbls.h`, compatible `nvidia,imx219` |
| Camera DT binding | `camera_common` (old) | **tegracam** framework (new) — different node/property layout |
| host1x video nodes | `vi { ... }` | `vi@15700000` + `nvcsi@150c0000` (channels w/ `port-index`) |
| i2c controllers | `7000cxxx` / `546c0000` | `316/318/c24…0000` (see table below) |
| Sensor clock | `cam_mclk1` | `extperiph1` (`TEGRA186_CLK_EXTPERIPH1`) |
| GPIO refs | raw `<&gpio 148 1>` | `<&tegra_main_gpio TEGRA_MAIN_GPIO(x,y) ...>` |
| Mode props | `pixel_t`, … | `mode_type`, `pixel_phase`, `csi_pixel_bit_depth`, fixed‑point gain/exp/fps |

### i2c bus translation (from `tegra186-soc-i2c.dtsi`)

| Role on J106 | TX1 node | **TX2 node (use this)** | Linux bus | Live status |
|--------------|----------|--------------------------|-----------|-------------|
| CSI‑A/B (GEN i2c) | `i2c@7000c000` | `gen2_i2c:` **`i2c@c240000`** | `i2c-1` | **CONFIRMED: 0x10 ACK (north)** |
| CSI‑C/D (CAM i2c) | `i2c@546c0000` | `cam_i2c:` **`i2c@3180000`** | `i2c-2` | **CONFIRMED: 0x10 + 0x12 ACK** |
| CSI‑E/F (GEN i2c) | `i2c@7000c500` | `gen8_i2c:` **`i2c@c250000`** | `i2c-7` | **CONFIRMED: 0x10 + 0x12 ACK** |

Available TX2 i2c controllers: `gen1_i2c@3160000`, `gen2_i2c@c240000`,
`cam_i2c@3180000`, `dp_aux_ch1_i2c@3190000`, `dp_aux_ch0_i2c@31b0000`,
`gen7_i2c@31c0000`, `gen8_i2c@c250000`, `gen9_i2c@31e0000`.

### IMX219 modes supported by the R32 driver
`3264x2464@21`, `3264x1848@28`, `1920x1080@30`, `1280x720@60`, `1280x720@120`
(native Bayer RGGB, 2 lanes).

---

## 3b. LIVE hardware verification (2026‑06‑08, on the actual TX2)

Probed the running board (`nvidia-desktop`, `192.168.0.168`) directly:

- **L4T = R32.7.6** / kernel **4.9.337-tegra** (JetPack 4.6.x) — note this is NEWER than the
  J90 BSP's R32.2.1. The `tegracam` DT binding is identical across R32.x so the `.dtsi`
  is valid; build the DTB against the **R32.7.x** kernel sources to match the running kernel.
- Active DTB is the **stock devkit** (`model = quill`, `nvidia,p2597-0000+p3310-1000`) — no
  camera nodes yet, VI ports report `ep ... not enabled`, no `/dev/video*`.
- Live `i2cdetect -l` adapter numbering matches the table above exactly
  (`c240000`→i2c-1, `3180000`→i2c-2, `c250000`→i2c-7).
- **Reset GPIO CONFIRMED**: `TEGRA_MAIN_GPIO(R, 5)` = sysfs **gpio 461** (chip base 320 + 141).
  Exporting it, driving **0 then 1** (active‑low release), made the sensors appear. Pulse:
  ```bash
  G=461
  sudo sh -c "echo $G > /sys/class/gpio/export"
  sudo sh -c "echo out > /sys/class/gpio/gpio$G/direction"
  sudo sh -c "echo 0 > /sys/class/gpio/gpio$G/value"; sleep 0.3
  sudo sh -c "echo 1 > /sys/class/gpio/gpio$G/value"
  ```
  (This is the manual equivalent of the gpio‑hog in the dtsi; it does **not** survive reboot.)
- **Cameras detected after reset release** (initially 4 connected):
  ```
  i2c-2 (3180000):  0x10  0x12     <-- CSI-C/D pair
  i2c-7 (c250000):  0x10  0x12     <-- CSI-E/F pair
  i2c-0 (3160000):  (none)
  i2c-1 (c240000):  (none)         <-- CSI-A/B connectors still empty at this point
  ```

- ALL THREE camera buses now hardware-confirmed (5th camera added to A/B bus):
  ```
  i2c-1 (c240000):  0x10        <-- CSI-A/B pair (north only; +0x12 when 6th added)
  i2c-2 (3180000):  0x10  0x12  <-- CSI-C/D pair
  i2c-7 (c250000):  0x10  0x12  <-- CSI-E/F pair
  i2c-0 (3160000):  (none)      <-- not a camera bus
  ```
  => A/B=`c240000`, C/D=`3180000`, E/F=`c250000` all match the dtsi. The lone `0x0c`
  seen on i2c-1 is the carrier address-shifter companion, not a camera.

---

## 3c. M110 expansion board: Ethernet, USB, PCIe + L4T version decision (2026‑06‑08)

The TX2 is on a **J106 + M110** stack. Checked the M110 peripherals live and decided which
L4T to standardise on.

### Ethernet — WORKS (Tegra EQOS, not a PCIe NIC)

- The M110 RJ45 is wired to the Tegra **built-in EQOS MAC** (`2490000.ether_qos`, driver
  `eqos`, `phy-mode = rgmii`) through an on-carrier Broadcom PHY — it is **not** a USB or
  PCIe network card. So it needs **no Auvidea-specific networking patch** and is not tied to
  any particular L4T version.
- `eth0` links up and passes traffic. It currently negotiates only **10 Mbps**, but `ethtool`
  shows the **TX2 side advertises full 10/100/1000 with zero errors** — the link partner
  ("Link partner advertised link modes: 10baseT/Half/Full") is the bottleneck. Fix is on the
  host/cable side: use a known-good Cat5e/Cat6 cable and set the host NIC to **auto-negotiate**.
  Re-check with:
  ```bash
  sudo ethtool eth0 | grep -E "Speed|Link partner advertised link modes"
  ```

### USB — BROKEN by the stock devkit DTB (same root cause as the cameras)

A Logitech wireless dongle is plugged in but the mouse never moves. USB is **completely dead**:
`/sys/bus/usb/devices/` is empty, no input device appears, and the XUSB host controller never
registers. The persistent boot log shows exactly why:

```
pca953x 0-0074: failed reading register
pca953x: probe of 0-0074 failed with error -121     <-- devkit-only I2C GPIO expander, absent on J106/M110
pca953x: probe of 0-0077 failed with error -121
tegra-xusb-padctl 3520000.xusb_padctl: failed to setup XUSB ports: -517   <-- EPROBE_DEFER (forever)
tegra-usb-cd usb_cd: otg phy is not available yet
```

Root cause: the running **stock p2597 devkit DTB** drives USB **VBUS enable** through two I²C
GPIO expanders (`pca953x` @ `0x74`/`0x77`) that only exist on the NVIDIA devkit. On the
J106/M110 those chips are not present, so the expanders fail to probe (`-121` = no I²C ACK),
the XUSB pad controller can never obtain its VBUS GPIOs, and it defers probe permanently
(`-517`). With no host controller, the dongle is unpowered and never enumerates. (The
`ina3221` power-monitor probe failures at `0x42/0x43` are the same devkit-only story.)

`xhci@3530000` and `xusb_padctl@3520000` are both `status = okay` and `CONFIG_USB_XHCI_TEGRA=y`
is built in — the hardware/driver are fine; only the **board description** is wrong.

**Fix = the correct J106/M110 carrier DTB.** The carrier DTB must define USB VBUS via the
carrier's actual regulators/GPIOs instead of the devkit's `pca953x` expanders. This is exactly
what Auvidea's documented **USB vbus-supply patch** (USB 2.0 `vbus-*-supply` change) and the
separate **USB 3.0 patch** do. **Cameras and USB therefore get fixed together** when we flash
the proper carrier device tree — neither is a hardware fault.

### PCIe — slot present but empty

`lspci` returns nothing; no card is installed in the M110 PCIe slot. Nothing to configure.

### L4T version decision → **STAY on R32.7.6** (do not downgrade to R32.2.1)

The question was whether to downgrade to R32.2.1 (JetPack 4.2.2, matching the J90 BSP) to get
"full" J106 + M110 support. Decision: **keep R32.7.6**, because:

- M110 Ethernet is the **standard Tegra EQOS** path — fully supported on R32.7.6, not
  version-locked, and needs no Auvidea patch.
- The TX1-era Auvidea BSP patch set (`bsp_L4T_24_2_1_V2.2`) contains **only** camera (IMX219),
  CAN, and a build fix — **no PCIe or Ethernet patches** — confirming M110 networking does not
  depend on any special/older BSP.
- USB needs a small **device-tree** VBUS patch, which applies cleanly on R32.7.6; it is **not**
  a kernel/driver problem that an older release would solve.
- R32.7.6 ships an **equal-or-newer** kernel (4.9.337) with broader driver/security coverage.

So: build the carrier DTB (cameras + USB VBUS + verified EQOS PHY) against **R32.7.x** sources
and flash it — no OS downgrade required.

---

## 4. The two open values — RESOLVED from web research

Both previously-unknown values are now confirmed from working **J106 + TX2** reports and
from Auvidea's own firmware device tree. They are already applied in the dtsi.

### 4.1 Shared camera RESET GPIO  →  `TEGRA_MAIN_GPIO(R, 5)` (ACTIVE_LOW + gpio-hog)

All 6 connectors share one reset line. On TX2 it is **`TEGRA_MAIN_GPIO(R, 5)`**, released at
boot by a **gpio-hog (`output-high`)** on `gpio@2200000`, and referenced **ACTIVE_LOW** by each
sensor node:

```dts
/ {
        gpio@2200000 {
                camera-control-output-high {
                        gpio-hog;
                        output-high;
                        gpios = <TEGRA_MAIN_GPIO(R, 5) 0>;
                        label = "cam-rst";
                };
        };
};
```

Sources:
- NVIDIA forum *"Raspberry Pi camera and Jetson TX2"* (J106 + TX2, CSI‑E, marked **SOLVED**) —
  uses `CAM_RST_L = TEGRA_MAIN_GPIO(R, 5)` ACTIVE_LOW + the gpio-hog above.
- Auvidea J106/TX2 firmware workflow: the same physical pin is `sysfs gpio 461` on R28
  (`echo 461 > export; echo out > direction; echo 1 > value`).

### 4.2 CSI‑E/F i2c controller  →  `i2c@c250000` (gen8, `i2c-7`) — CONFIRMED

A working J106 + TX2 CSI‑E camera enumerates as **`imx219 7-0010`**, i.e. Linux bus **7**,
which is SoC controller **`i2c@c250000`** (`gen8_i2c`). The south camera sits at `0x12` on the
same bus. This replaces the earlier `i2c@c240000` guess for E/F.

### 4.3 The authoritative source for ALL six buses

Auvidea distributes the complete vendor device tree as **`tegra186-camera-imx219-rr.dtsi`**
(the `-rr` = RidgeRun), included from `tegra186-quill-p3310-1000-a00-00-base.dts`. Get it from
the **source/** directory inside Auvidea firmware **v1.5** (https://auvidea.com/firmware/,
match the JetPack 4.2.2 / L4T R32.2.1 release). That file is the definitive answer for the
A/B and C/D buses too. In Auvidea's rr.dtsi, CSI‑A/B sit on **`i2c@c240000`** (which is why the
table above uses it); the C/D bus here (`i2c@3180000`) is inferred as the remaining camera bus
and should be cross‑checked against that file or via `i2cdetect`.

### 4.4 Known hardware caveat (address shifter)

Multiple J106 + TX2 reports see the **north** cameras (`0x10` on A/C/E) reliably, but the
**south** cameras (`0x12` on B/D/F, produced by the carrier's address shifter) sometimes fail
to ACK — especially after a cold `shutdown` rather than `reboot`. If B/D/F don't appear:

```bash
i2cdetect -l                 # list adapters and their SoC addresses
for b in $(seq 0 8); do echo "== bus $b =="; i2cdetect -y -r $b; done
```

Properly toggling the shared reset (the gpio-hog above) and a full power cycle generally fixes
it; it is a documented carrier quirk, not a device-tree error.

---

## 5. Build & flash plan (TX2 on J106/M110) — current, validated approach

**Goal:** produce **one custom carrier DTB** that fixes **cameras + USB together** in a single
flash, on top of the **already-installed R32.7.6**. No OS downgrade. WiFi/Ethernet untouched.

### 5.0 Strategy (decided from live hardware, see §3b–3c)
- **Stay on L4T R32.7.6** (kernel 4.9.337). Build the DTB against **R32.7.x** sources so it
  matches the running kernel. Do **not** downgrade to R32.2.1.
- **Do not use the Auvidea J90 package** — it targets R32.2.1 and only carries camera+CAN
  patches. Every carrier fix we actually need (CSI cameras, USB VBUS) is a **device-tree**
  change we apply ourselves on top of stock NVIDIA R32.7.x.
- **DTB scope:** (a) 6× IMX219 cameras, (b) USB VBUS, (c) sanity-check EQOS PHY reset.
  **Leave WiFi (`sdhci@*` / `bcmdhd`) and the working Ethernet driver alone.**

### 5.1 Host setup
- Download NVIDIA **Jetson Linux R32.7.x** for TX2: *Driver Package (BSP)* + *Sample Root
  Filesystem*; unpack `Linux_for_Tegra/`.
- Download the matching **`public_sources` (kernel + DT)** for R32.7.x — needed to build DTBs.

### 5.2 Kernel / driver — ⚠️ a small driver patch IS needed (see §7)
- The IMX219 driver already ships in R32 (`drivers/media/i2c/imx219.c`), `CONFIG_VIDEO_IMX219=y`.
- **However** the modern *tegracam* `imx219` driver makes `reset-gpios` **mandatory** and calls
  `gpio_request()` on it **exclusively** (one consumer per pin). The J106 ties **all 6** camera
  resets to **one** line, so the 6 sensors cannot all request it — only the first probes, the
  rest fail with `-EBUSY/-16`. This was discovered on the live board (see §7) and is the reason
  a tiny **kernel driver patch** ([`patches/0001-imx219-share-reset-gpio-j106.patch`](patches/0001-imx219-share-reset-gpio-j106.patch))
  is required — exactly the kind of IMX219 patch Auvidea/RidgeRun apply for TX2. So this is **not**
  a pure device-tree job after all.

### 5.3 Camera device tree
- Copy
  [`tx2-j106-6csi/tegra186-camera-j106-imx219.dtsi`](tx2-j106-6csi/tegra186-camera-j106-imx219.dtsi)
  into `hardware/nvidia/platform/t18x/common/kernel-dts/t18x-common-platforms/`.
- Include it from the board dts matching this module — confirmed live as **`quill p3310-1000`**
  (`tegra186-quill-p3310-1000-a00-00-base.dts`):
  ```c
  #include "t18x-common-platforms/tegra186-camera-j106-imx219.dtsi"
  ```
  Remove/disable any other `tegra186-quill-camera-*.dtsi` include to avoid host1x clashes.
- The dtsi already encodes the verified facts: buses `c240000`/`3180000`/`c250000`,
  reset gpio-hog `TEGRA_MAIN_GPIO(R,5)`, sensors at `0x10`/`0x12` (address shifter).

### 5.4 USB VBUS fix (NEW — required, see §3c)
- The stock DTB drives USB VBUS through devkit-only `pca953x` expanders (`0x74`/`0x77`) that
  don't exist on the carrier → `xusb_padctl … failed to setup XUSB ports: -517`.
- In the carrier DTB, **replace that VBUS source**: point the `xusb_padctl@3520000` USB2/USB3
  ports' `vbus-*-supply` at the carrier's **fixed-regulator / GPIO VBUS** instead of the
  expander GPIOs (this is what Auvidea's USB2 `vbus-*-supply` patch + USB3 patch do). Remove
  the unused `pca953x`/`ina3221` devkit nodes so they stop probe-failing. This clears the
  `-517` defer and brings up `xhci@3530000`.

### 5.5 Ethernet / WiFi (verify only, no change)
- Keep `2490000.ether_qos` (rgmii) as-is; just confirm the PHY reset GPIO matches the carrier.
- **Do not touch** the WiFi SDIO/`bcmdhd` nodes — they already work (§3c).

### 5.6 Build
- Build the tegra186 DTBs (`make … dtbs`), producing the updated
  `tegra186-quill-p3310-1000-a00-00-base.dtb`.

### 5.7 Flash just the DTB (no full reflash needed)
- Fastest, on the live board: copy the new `.dtb` to `/boot`, point the `FDT` line in
  `/boot/extlinux/extlinux.conf` at it, reboot (the same trick the TX1 patch used). **OR**
  flash the DTB partition from the host:
  `sudo ./flash.sh -r -k kernel-dtb jetson-tx2 mmcblk0p1`.

### 5.8 Verify on target
```bash
# cameras
dmesg | grep -i imx219
ls /dev/video*        # expect video0..video5
v4l2-ctl --list-devices
v4l2-ctl -d /dev/video0 --set-fmt-video=width=3264,height=2464,pixelformat=RG10 \
         --stream-mmap --stream-count=10
# usb (was dead before)
lsusb                 # Logitech dongle should appear; mouse should move
ls /sys/bus/usb/devices/
# ethernet
sudo ethtool eth0 | grep -E "Speed|Link"
```

---

## 6. Notes / gotchas

- **No i2c mux needed.** Unlike the NVIDIA devkit `imx274-dual` example (which uses a
  `tca9546` mux), the J106 has 3 **physically separate** buses plus the address shifter, so
  the two sensors per bus sit directly under each `i2c@...` node at `0x10` and `0x12`.
- The address‑shift jumpers (P1–P3) change the shift from 2→1; if you ever set shift=1 the
  south sensors move to `0x11` — keep DT `reg` in sync.
- `port-index` on tegra186 NVCSI maps CSI bricks A–F → 0–5; for 2‑lane sensors each brick is
  used individually (0,1,2,3,4,5), unlike 4‑lane which uses 0,2,4.
- Mixed tegra186/tegra194 DTBs in J90 confirm it is L4T R32.x (also supports Xavier).

---

## 7. Bring‑up progress & current plan (2026‑06‑09)

Live work on the actual TX2 (over `ssh`, DTBs built on WSL with `dtc`, deployed via a
**reversible** `extlinux` `FDT` line with a backup — partition DTB never touched).

### 7.1 USB — ✅ DONE & VERIFIED
The §3c/§5.4 VBUS fix is implemented and deployed. The carrier DTB deletes the `gpio`
property from the devkit VBUS regulator (`fixed-regulators/regulator@17`) and forces it
`regulator-always-on` / `regulator-boot-on`, replacing the absent `pca953x` expander path.
Result: `-517` defer gone, `xhci@3530000` registers, **two USB root hubs enumerate**. Survives
reboots (incl. crash‑recovery). Deployed as `/boot/tegra186-j106.dtb`, `extlinux` `DEFAULT j106usb`.

### 7.2 Cameras — IN PROGRESS (driver patch built, not yet deployed)
What works now:
- **Reset gpio request solved.** A uniquely‑named gpio‑hog `j106-camera-reset-release`
  (`status="okay"`) claims and releases the shared reset (gpio 461). Verified live:
  `gpio-461 … j106-camera-reset-re out hi`. (A same‑named node silently merges into the stock
  devkit's *disabled* `camera-control-output-high` hog — hence the rename.)
- With per‑sensor **dummy** reset pins + an always‑on **dummy regulator** (`cam_dummy_reg`),
  the driver now loads `tegracam … imx219_v2.0.6` and `i2cdetect` shows **`UU` at 0x10/0x12 on
  all 3 buses** (all 6 sensors bound).

The wall:
- Sensors then read **`invalid sensor model id: 00`** (and `-121` NACK on the shifted `0x12`).
  Root cause: the requestable dummy pins live in the camera‑control region and are **physically
  connected** on the J106, so driving them (the driver's reset assert) **disturbs the cameras**;
  the truly‑unconnected pins are SFIO and fail `gpio_request()`. A genuine catch‑22.
  *(Danger noted: exporting AON gpios 256–275 crash‑reboots the board — do not poke them.)*

### 7.3 Why TX1 works and what fixes TX2 (root‑cause, confirmed)
- The **J106 technical reference** confirms: **pin 6 (CAM‑RST, active‑low) of all 6 CSI
  connectors are tied together** and driven by **one** GPIO (TX1 `H8`=gpio 148; TX2=gpio 461),
  toggled **once** (high→low→high). **Pin 5 (`CAM0_MCLK`) is likewise one shared 24 MHz clock.**
  So the carrier is a **single shared reset + single shared mclk, triggered once** design — **not**
  per‑sensor.
- TX1 worked because the **R24.2.1** IMX219 driver did **not** exclusively request a per‑sensor
  reset (it was toggled externally). The modern **R32 tegracam** driver's mandatory per‑sensor
  exclusive `reset-gpios` is the **only** real mismatch.
- A known NVIDIA‑forum thread (*"Can't detect the camera I2C address using J106 board on tx2"*)
  shows the **identical** symptom (same `echo 461 … value 0 … value 1` toggle; `0x10` on A/C/E,
  NACK on `0x12` B/D/F). RidgeRun's official J106/TX2 guide solves it by **rebuilding the kernel
  with an IMX219 driver patch** — confirming the path below. **No public J106 schematic exists**
  (Auvidea ships only the technical reference; a full schematic is request‑only/NDA).

### 7.4 The fix (chosen) — patch the driver to share the one reset, mirroring TX1
[`patches/0001-imx219-share-reset-gpio-j106.patch`](patches/0001-imx219-share-reset-gpio-j106.patch)
makes the driver do three things so all 6 sensors can share the one real line (461) — **no dummy
pins**, nothing disturbing the cameras:
1. treat `-EBUSY` from `gpio_request()` as success (a sibling/hog may already own the line);
2. **never free** the shared reset (unbinding one sensor must not release reset for the others);
3. **never drive the shared line low** in `power_on`/`power_off` — release it high once (the hog
   does this at boot) and leave it, so opening, stopping or crashing one sensor never resets the
   others. IMX219 re‑init uses its I²C software reset (reg `0x0103`).

Cross‑checked against the **Auvidea TX1 R24.2.1 BSP** (`bsp_L4T_24_2_1_V2.2`): its
`tegra210-imx219.dts` points **all 6** sensors at the **same** `reset-gpios = <&gpio 148 1>`, and
its driver patch **comments out every `gpio_set_value(reset, 0)`** — i.e. the exact "release once,
never assert" behavior items 2–3 reproduce on R32. The only structural difference is the capture
graph: TX1 (k3.10) is **vi‑only (2‑hop)**; TX2 (R32/k4.9) is **nvcsi + vi (3‑hop)**, which the
dtsi already implements.

Build status (on WSL, `/tmp/j106build/r3276` for the kernel, `/tmp/j106build` for the DTB):
- ✅ R32.7.6 `public_sources` downloaded; kernel source = **4.9.337** confirmed.
- ✅ Official **Linaro 7.3.1** toolchain (NVIDIA‑hosted) extracted & working.
- ✅ Config = the **board's own `/proc/config.gz`**; `make kernelrelease` → **`4.9.337-tegra`**
  (matches running kernel, so existing modules stay compatible — `LOCALVERSION=-tegra`,
  `CONFIG_LOCALVERSION_AUTO` off).
- ✅ Driver patch applied to `ksrc/.../imx219.c`.
- ✅ **`Image` built** → `kout/arch/arm64/boot/Image` (34 MB, `4.9.337-tegra`, patched
  `imx219.o` compiled in).
- ✅ **dtsi reworked to the shared‑reset model** + capture pipeline enabled (below).
- ✅ **J106 DTB rebuilt** → `tegra186-j106.dtb` (USB fix + 6× imx219 shared reset + vi/nvcsi
  channels `okay`). Verified by decompile: all 6 sensors `reset-gpios = <&gpio@2200000 141
  ACTIVE_LOW>` (the one shared line, same pin the reset‑release hog drives), 6 nvcsi channels okay.

### 7.5 Next steps (host‑only work DONE; remaining steps need the board)
Host‑only build steps (done on WSL, no hardware needed):
1. ✅ **Built the `Image`:**
   `make -C ksrc/kernel/kernel-4.9 O=kout ARCH=arm64 CROSS_COMPILE=<linaro>/bin/aarch64-linux-gnu- LOCALVERSION=-tegra -j$(nproc) Image`
2. ✅ **Reworked the dtsi to the shared‑reset model:** `reset-gpios = <&tegra_main_gpio
   J106_CAM_RST GPIO_ACTIVE_LOW>` restored into `IMX219_HW_RESOURCES`, all
   `IMX219_DUMMY_RST(...)` per‑sensor pins **deleted**, `j106-camera-reset-release` hog and
   `cam_dummy_reg` kept.
3. ✅ **Enabled the capture pipeline:** added `status="okay"` to the `vi@15700000`
   ports/endpoints and `nvcsi@150c0000` channels/ports/endpoints (they inherit `disabled` from
   the stock devkit tree merge, so there is **no `/dev/video*`** until enabled).
4. ✅ **Rebuilt the J106 DTB** (stock‑c03 base + USB override + reworked camera dtsi).

Remaining (require the TX2 board — blocked until hardware is back):
5. ⏳ **Deploy reversibly:** copy patched `Image`→`/boot/Image.j106` and the new DTB; add a **new**
   `extlinux` `LABEL` using them while keeping the working USB‑only entry as fallback. Reboot.
6. ⏳ **Verify:** `dmesg | grep imx219` (expect valid model id, no `-16`/`model id 00`),
   `ls /dev/video0..5`, then a `v4l2-ctl --stream-mmap` / `nvarguscamerasrc sensor-id=0..5` test.

> Fallback always available: `extlinux` `DEFAULT j106usb` (USB working) and backup
> `/boot/extlinux/extlinux.conf.backup-pre-j106`; partition DTB (`mmcblk0p30`) untouched.
