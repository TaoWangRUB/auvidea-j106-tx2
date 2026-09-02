# Auvidea J106 — 6× CSI IMX219 cameras on Jetson TX2

This document captures the analysis of the existing **working TX1** setup and the
**plan + scaffold** to reproduce the same **6 CSI camera** support on **Jetson TX2**
on the Auvidea **J106 (+ M110)** carrier.

> TL;DR: On TX1 it works because Auvidea patched the kernel (IMX219 driver) and shipped a
> custom device tree wiring all 6 cameras across 3 i2c buses. On TX2 the **IMX219 driver
> already exists** in the L4T R32 kernel, so the bulk of the port is a **device‑tree job** —
> **plus one small driver patch** ([`patches/0001-imx219-share-reset-gpio-j106.patch`](patches/0001-imx219-share-reset-gpio-j106.patch))
> so the 6 sensors can share the J106's single hardware reset line (see §5, Stage 1). The board
> already runs **L4T R32.7.6** — we **stay on it** and build a single carrier DTB that fixes
> **cameras + USB** together (WiFi/Ethernet already work). **USB is already fixed & verified**
> (§7). The starting device tree is in
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

### IMX219 modes supported (as deployed on the J106)

Native Bayer RGGB, 2 lanes. The DT mode index = driver register‑table index
(`imx219_mode_tbls.h` order); `sensor_mode=N` / Argus sensor‑mode‑id select by it.
The **fps shown is the deployed cap** — derated to the J106‑lowered **680 Mbps/lane**
PLL (stock 912 Mbps rates in parentheses; advertising the stock rate truncates
frames → `ChanselFault`).

| mode | resolution | fps (stock) | sensor area | FoV (H) | use |
|------|-----------|-------------|-------------|---------|-----|
| 0 | 3264×2464 | 15 (21) | full, no bin | ~160° | full‑res stills / max detail |
| 1 | 3264×1848 | 20 (28) | full width, H‑crop | ~160° | full **horizontal** FoV, lighter than mode0 |
| 2 | 1920×1080 | 30 | center crop | ~94° | 1080p video (narrow) |
| **3** | **1640×1232** | **22 (30)** | **full, 2×2 binned** | **~160°** | **full‑FoV VIO/fisheye (recommended for the omni rig)** |
| 4 | 1280×720 | 44 (60) | center crop, 2×2 binned | ~94° | fast narrow capture |

The stock `1280x720@120` register table is commented out, so there is **no**
720p high‑rate mode (an earlier DT advertised a phantom 720p@110 — removed).

---

## 3a. Camera pipeline architecture — TX1 vs TX2 (diagrams)

These two diagrams show all six IMX219 pipelines end‑to‑end on each platform,
including the hardware ISP. Cross‑checked against the real device trees:
`bsp_L4T_24_2_1_V2.2/.../tegra210-imx219.dts` (TX1) and the decompiled stock
c03 DTB (TX2).

### TX1 (L4T R24.2.1, tegra210) — VI‑only **2‑hop** graph, **2× ISP**

```mermaid
flowchart LR
    subgraph I2C["I2C control"]
        B0["i2c@7000c000 (adpt 0)"]
        B6["i2c@546c0000 (adpt 6)"]
        B2["i2c@7000c500 (adpt 2)"]
    end
    RST["shared reset<br/>&lt;&amp;gpio 148 1&gt; (all 6)"]

    subgraph SENS["6x IMX219 sensors (port@0 / endpoint, 2-lane MIPI, raw Bayer)"]
        A["imx219_a@10  csi-port=0<br/>ep o0 (bottomleft)"]
        Bb["imx219_b@12  csi-port=1<br/>ep o1 (bottomright)"]
        C["imx219_c@10  csi-port=2<br/>ep o2 (centerleft)"]
        D["imx219_d@12  csi-port=3<br/>ep o3 (centerright)"]
        E["imx219_e@10  csi-port=4<br/>ep o4 (topright)"]
        F["imx219_f@12  csi-port=5<br/>ep o5 (topleft)"]
    end

    subgraph VI["host1x / vi@54080000  (nvidia,tegra210-vi, num-channels=6) — capture + DMA"]
        V0["vi port@0 (CSI A)"]
        V1["vi port@1 (CSI B)"]
        V2["vi port@2 (CSI C)"]
        V3["vi port@3 (CSI D)"]
        V4["vi port@4 (CSI E)"]
        V5["vi port@5 (CSI F)"]
    end

    NVMM["NVMM surfaces<br/>(NvBuffer / dmabuf in unified LPDDR)"]
    N["/dev/video0..5<br/>raw Bayer SRGGB10 (ISP bypassed)"]

    subgraph ISPHW["Hardware ISP (in DTB, status=okay) — engaged by libargus"]
        ISPA["isp@54600000<br/>tegra210-isp (ISP-A)"]
        ISPB["isp@54680000<br/>tegra210-isp (ISP-B)"]
    end
    ARG["Argus / nvarguscamerasrc<br/>-> debayered YUV/NV12 + 3A<br/>(stays in NVMM -> CUDA/NVENC)"]

    B0 -. 0x10 .-> A
    B0 -. 0x12 .-> Bb
    B6 -. 0x10 .-> C
    B6 -. 0x12 .-> D
    B2 -. 0x10 .-> E
    B2 -. 0x12 .-> F
    RST -.-> A & Bb & C & D & E & F

    A == "remote-endpoint" ==> V0
    Bb ==> V1
    C ==> V2
    D ==> V3
    E ==> V4
    F ==> V5

    V0 & V1 & V2 & V3 & V4 & V5 ==> NVMM
    NVMM --> N
    NVMM == "Argus binds isp@ at runtime<br/>(not a DT graph hop)" ==> ISPA
    ISPA -.HW debayer + 3A.-> ARG
    ISPB -.parallel instance.-> ARG
```

### TX2 (L4T R32.7.6, tegra186 / J106) — NVCSI+VI **3‑hop** graph, **1× ISP**

```mermaid
flowchart LR
    subgraph I2C["I2C control"]
        B0["i2c@c240000"]
        B1["i2c@3180000"]
        B2["i2c@c250000"]
    end
    RST["shared reset<br/>J106_CAM_RST (all 6)"]

    subgraph SENS["6x IMX219 sensors (port@0 / endpoint, 2-lane MIPI, raw Bayer)"]
        A["imx219_a@10  port-index=0"]
        Bb["imx219_b@12  port-index=1"]
        C["imx219_c@10  port-index=2"]
        D["imx219_d@12  port-index=3"]
        E["imx219_e@10  port-index=4"]
        F["imx219_f@12  port-index=5"]
    end

    subgraph NVCSI["host1x / nvcsi@150c0000 (nvidia,tegra186-csi) — CSI-2 receiver, 6 channels"]
        CH0["channel@0  p0->p1"]
        CH1["channel@1  p0->p1"]
        CH2["channel@2  p0->p1"]
        CH3["channel@3  p0->p1"]
        CH4["channel@4  p0->p1"]
        CH5["channel@5  p0->p1"]
    end

    subgraph VI["host1x / vi@15700000 (nvidia,tegra186-vi, num-channels=6) — capture + DMA"]
        V0["vi port@0"]
        V1["vi port@1"]
        V2["vi port@2"]
        V3["vi port@3"]
        V4["vi port@4"]
        V5["vi port@5"]
    end

    NVMM["NVMM surfaces<br/>(NvBuffer / dmabuf in unified LPDDR)"]
    N["/dev/video0..5<br/>raw Bayer (ISP bypassed)"]

    subgraph ISPHW["Hardware ISP — ONE instance"]
        ISPA["isp@15600000<br/>nvidia,tegra186-isp (ISP-A)"]
    end
    ARG["Argus / nvarguscamerasrc<br/>-> debayered YUV/NV12 + 3A<br/>(stays in NVMM -> CUDA/NVENC)"]

    B0 -. 0x10 .-> A
    B0 -. 0x12 .-> Bb
    B1 -. 0x10 .-> C
    B1 -. 0x12 .-> D
    B2 -. 0x10 .-> E
    B2 -. 0x12 .-> F
    RST -.-> A & Bb & C & D & E & F

    A == "remote-endpoint" ==> CH0
    Bb ==> CH1
    C ==> CH2
    D ==> CH3
    E ==> CH4
    F ==> CH5

    CH0 ==> V0
    CH1 ==> V1
    CH2 ==> V2
    CH3 ==> V3
    CH4 ==> V4
    CH5 ==> V5

    V0 & V1 & V2 & V3 & V4 & V5 ==> NVMM
    NVMM --> N
    NVMM == "Argus binds isp@ at runtime<br/>(not a DT graph hop)" ==> ISPA
    ISPA -.HW debayer + 3A.-> ARG
```

### TX1 ⟷ TX2 pipeline differences

| Aspect | TX1 (tegra210, K3.10) | TX2 (tegra186, K4.9 / J106) |
|--------|------------------------|------------------------------|
| Capture graph | **2‑hop**: `sensor → vi port@N` | **3‑hop**: `sensor → nvcsi channel@N → vi port@N` |
| CSI‑2 receiver in DT | folded into VI (no separate node) | explicit **`nvcsi@150c0000`** with one `channel@N` per stream |
| Port key on endpoint | `csi-port = N` | `port-index = N` |
| VI node | `vi@54080000` (`nvidia,tegra210-vi`) | `vi@15700000` (`nvidia,tegra186-vi`) |
| **Hardware ISP count** | **2** (`isp@54600000`, `isp@54680000`) | **1** (`isp@15600000`) |
| ISP compatible | `nvidia,tegra210-isp` | `nvidia,tegra186-isp` |
| I²C buses (A/B, C/D, E/F) | `7000c000`, `546c0000`, `7000c500` | `c240000`, `3180000`, `c250000` |
| Sensor clock | `cam_mclk1` | `extperiph1` (`TEGRA186_CLK_EXTPERIPH1`) |
| GPIO ref style | raw `<&gpio 148 1>` | `<&tegra_main_gpio TEGRA_MAIN_GPIO(R,5) ...>` |
| Camera DT framework | old `camera_common` | **tegracam** (`mode_type`/`pixel_phase`/fixed‑point gain) |

**Why NVCSI is broken out on TX2 (the extra hop):** on Tegra186 the CSI‑2
receiver is promoted to a first‑class, per‑channel device‑tree block so the SoC
can do **virtual‑channel (VC) demux**, **deserializer/aggregator** cameras
(GMSL/FPD‑Link: many cameras over one CSI port), flexible **N:M stream→VI
routing**, and per‑stream **test‑pattern/error** handling. For one‑camera‑per‑port
IMX219 (our J106), each `nvcsi channel@N` is just a straight `port@0 → port@1`
passthrough, so it behaves like TX1's 2‑hop — just two more nodes to set
`status="okay"`.

**Common to both:** unified LPDDR (UMA) — VI DMAs into **NVMM** buffers shared by
GPU/ISP/NVENC, so the raw V4L2 path (`/dev/videoN`) bypasses the ISP, while the
Argus path engages the HW ISP and can stay zero‑copy into CUDA/NVENC
(`NvBuffer` → EGLImage → CUDA). The 6 sensors share one reset line; the patched
`imx219` driver (§5, Stage 1 / patch `0001`) never asserts it low so the cameras are independent.

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

  #### 🔧 How to reset the cameras (reboot‑free recovery)

  **On the deployed carrier DTB + patched kernel — use the driver's `j106_reset_recover` sysfs.**
  This is the *only* correct runtime reset. It pulses the one shared reset line (low→high) **and**
  re‑probes any present‑but‑unbound sensor (e.g. a port‑D `2-0012` that lost the boot lottery).

  ```bash
  # 1. List the cameras that DID bind (each has a recover trigger):
  ls /sys/bus/i2c/drivers/imx219/        # e.g. 2-0010  2-0012  7-0010  7-0012

  # 2. Trigger recovery from ANY bound camera. The line is shared, so one write
  #    resets + re-probes ALL 6 sensors:
  echo 1 | sudo tee /sys/bus/i2c/devices/2-0010/j106_reset_recover

  # 3. Confirm (the previously-missing sensor should now be bound + have a /dev/video*):
  ls /sys/bus/i2c/drivers/imx219/ ; ls /dev/video*
  ```

  > **Pick the right `<bound-imx219>`.** A sensor that *failed* its own boot probe has **no**
  > `j106_reset_recover` file — trigger from a **sibling that did bind** (recover port D `2-0012`
  > via port C `2-0010`). Buses: `2-00xx` = CSI‑C/D (`i2c@3180000`), `7-00xx` = CSI‑E/F
  > (`i2c@c250000`); `xx10` = north, `xx12` = south (shifted). ⚠️ Shared line → this briefly
  > resets **all 6** cameras, so it's on‑demand recovery, **not** something to run mid‑capture.

  > ⚠️ **The old `gpio 461` sysfs‑export method is OBSOLETE — do not use it on the carrier DTB.**
  > It only worked during early bring‑up on the **stock** DTB (when nothing owned the pin):
  > ```bash
  > # STOCK DTB ONLY (historical) — fails on the carrier DTB:
  > G=461; sudo sh -c "echo $G > /sys/class/gpio/export"
  > sudo sh -c "echo out > /sys/class/gpio/gpio$G/direction"
  > sudo sh -c "echo 0 > /sys/class/gpio/gpio$G/value"; sleep 0.3
  > sudo sh -c "echo 1 > /sys/class/gpio/gpio$G/value"
  > ```
  > On the deployed carrier DTB gpio 461 is claimed by the imx219 driver as `cam_reset_gpio`
  > (and the `j106-camera-reset-release` hog), so `echo 461 > /sys/class/gpio/export` fails with
  > **`‑EBUSY`** (verified live: exit 1, no `gpio461` node); `gpioset`/libgpiod can't grab a held
  > line either. Only the driver can drive the pin — hence the reset edge lives in patch `0001` as
  > the **boot pulse** (Stage 1.5) and the **`j106_reset_recover` sysfs** (Stage 1.6). The raw
  > toggle would also only make the *edge*; it would **not** re‑probe the sensor, which
  > `j106_reset_recover` does for you.
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

**Shifter mis-latch → a camera at the WRONG i2c address (recoverable ONLY by cold power-cycle).**
Observed live (June 2026): after closing a running capture, **port A vanished** — bus-1 `0x10`/`0x12`
both NAK'd (`imx219 1-0010: -121`), bus 1 showed only the shifter at `0x0c`. But the **sensor was
alive** — it had been **address-shifted to `0x3b`**. The shift is a **hardware solder-strap (default
2), not i2c-programmable** (J106 ref p.440), so the shifter chip had simply latched a bad state. Prove
the sensor is alive by reading the IMX219 model-ID register (`0x0000` → `0x02 0x19`) at the stray
address:
```bash
sudo i2cdetect -y -r 1                                   # find any extra ACKing address (e.g. 0x3b)
sudo i2ctransfer -y 1 w2@0x3b 0x00 0x00 r2              # 0x02 0x19 == a live IMX219 at the wrong addr
```
**Neither a reset pulse (`j106_reset_recover`) nor a soft `reboot` clears it** — the reset GPIO resets
the *sensors*, not the shifter chip, and `reboot` leaves the carrier rails up so the shifter keeps its
latch. **Only a true cold power-cycle** (pull the DC barrel jack ~10 s) re-latches the shifter from its
straps and restores the camera to `0x10`. Verified: cold boot → `imx219 1-0010 bound` → 5 cams (`/dev/video0-4`).
(This is distinct from the driver-fixed *port-D boot lottery* in §5 Stage 1.5/1.6, which IS a reset-edge
problem and IS fixed by the boot pulse / `j106_reset_recover`.)

---

## 5. Camera bring‑up: issues & fixes by pipeline stage (TX2 on J106 vs stock L4T R32.7.6)

Stock **L4T R32.7.6** (kernel `4.9.337‑tegra`) targets NVIDIA's **p2597 devkit**, not the Auvidea
J106/M110 carrier. Everything below is what the carrier needs **on top of stock**. All changes are in
two device‑tree files plus **one** driver patch — nothing else is rebuilt:

| Deliverable | What it changes |
|---|---|
| [`tx2-j106-6csi/tegra186-camera-j106-imx219.dtsi`](tx2-j106-6csi/tegra186-camera-j106-imx219.dtsi) | the 6 cameras: sensors, reset hog, MCLK pinmux, NVCSI/VI channels, sensor modes, `tegra-camera-platform` (Argus) |
| [`tx2-j106-6csi/override-usb.dtsi`](tx2-j106-6csi/override-usb.dtsi) | USB VBUS (host) + OTG device‑mode |
| [`patches/0001-imx219-share-reset-gpio-j106.patch`](patches/0001-imx219-share-reset-gpio-j106.patch) | imx219 driver: shared reset GPIO (no assert/free) + **boot-time reset pulse** (port-D shifter fix) + **`j106_reset_recover` sysfs** (reboot-free recovery) + 680 Mbps MIPI rate |
| [`patches/0002-imx296-tegracam-j106.patch`](patches/0002-imx296-tegracam-j106.patch) | **new** imx296 tegracam driver — R32.7.6 ships none. Port F only; independent of `0001` (disjoint files, either order) |

Capture path (3‑hop on tegra186):
`sensor (imx219 @0x10/0x12) → NVCSI@150c0000 channel@N → VI@15700000 port@N → /dev/videoN`, then
`ISP → libargus` for the processed path. The stages below follow that path from the sensor down.

### Stage 1 — Sensor I²C enumeration & shared reset

6 sensors on **3 i²c buses, 2 per bus** (`c240000`→i2c‑1 A/B, `3180000`→i2c‑2 C/D, `c250000`→i2c‑7 E/F);
the carrier address‑shifter (shift = 2) puts the "south" sensor of each pair at **0x12** instead of 0x10.

| # | Issue (vs stock) | Fix |
|---|---|---|
| 1.1 | Stock devkit DTB has **no J106 camera nodes**. | Add `imx219_<a..f>@10/12` under each `i2c@…` with the carrier reset/clocks/regulators (dtsi). |
| 1.2 | The R32 *tegracam* `imx219` driver makes `reset-gpios` **mandatory** and `gpio_request()`s it **exclusively** (one consumer/pin). The J106 ties **all 6** resets to **one** line → only the first sensor probes, the rest fail `‑EBUSY/‑16`; and any sensor open/close/crash resets all six. | **Driver patch `0001`:** treat `‑EBUSY` as success, never free the line, and **never drive it low** (release once, leave high — mirroring the working TX1 BSP). |
| 1.3 | The shared reset must be released once and held high. A gpio‑hog **named like the stock disabled `camera-control-output-high` hog silently merges and does nothing**. | Unique gpio‑hog `j106-camera-reset-release` on `TEGRA_MAIN_GPIO(R,5)` = gpio 461 (dtsi). |
| 1.4 | Stock DTB carries **PCA9546/PCA9548 i²c muxes** at `0x70`/`0x77` on the camera buses. The J106 address‑shifter responds to `0x70`; the mux driver claims it → the shifted **`0x12` south cameras never ACK** (even with the mux `disabled`). | `/delete-node/ tca9546@70; tca9548@77;` (i2c‑2) and `i2cmux@70;` (i2c‑1); also delete colliding devkit sensors (`ov23850_a@10`, `ov5693_c@36`, `ov23850_c@36`). |
| 1.5 | **Port‑D per‑boot lottery (FIXED).** The south `0x12` camera on the bare‑J106 **bus‑2** (CSI‑D / `2-0012`) only enumerates on some boots: raw i²c **`‑121`** (no ACK), and the bus‑2 shifter companion `0x0c` is absent too — the **address‑shifter doesn't latch "shift" mode** without a clean reset **edge**. The gpio‑hog (1.3) only *holds* reset high; it never gives the **low→high pulse** the [J106 Technical Reference p.7](J106_technical_reference_1.0.pdf) explicitly requires (*"toggle it low briefly … so the cameras are reset properly at power up"*, then `i2cdetect -y -r 2`). | **Driver patch `0001`:** on the **first** `power_on` (boot, pre‑stream), pulse the shared reset **low ~12 ms → high** — one clean edge for all shifters (static‑flag guarded, never pulses during streaming). Verified **10/10 reboots** port‑D up (was ~2/6 down before). Plus a **`j106_reset_recover` sysfs** for reboot‑free recovery (1.6). |
| 1.6 | If port‑D ever fails to enumerate **or** wedges mid‑stream, the only prior recovery was a reboot — userspace **can't** pulse gpio 461 (the driver/hog owns it → `/sys/class/gpio` export returns `‑EBUSY`; `imx219` is built‑in so no rmmod). | **Driver patch `0001`:** write‑only sysfs created on every **successfully‑bound** cam — `echo 1 \| sudo tee /sys/bus/i2c/devices/<bound-imx219>/j106_reset_recover` re‑pulses the shared reset (same low→high edge) **and** re‑probes every present‑but‑unbound `imx219` (`bus_for_each_dev`→`device_attach`). **A sensor that failed its own boot probe has no recover file of its own — trigger it from any *sibling* camera** (e.g. recover port D via `2-0010`/port C). Verified live: drop `2-0012` (no driver, no own recover file) → trigger from `2-0010` → `2-0012` re‑binds + streams. *Caveat:* shared line → briefly resets all 6 cams; on‑demand recovery, not mid‑capture. |

> **South `0x12` shifter — now deterministic (was an open per‑boot lottery).** The M110 bus (E/F) has a
> robust translator (`0x43` + EEPROM `0x50`) and always worked; the **bare‑J106 bus‑2** shifter (C/D)
> needed the documented reset **edge** — supplied by 1.5/1.6. It was **not** a cable/reseat issue and
> **not** i²c speed (F runs `0x12` fine at 400 kHz). Camera **B has no physical sensor** on this rig
> regardless (its `1-0012` `‑121` is expected).

### Stage 2 — MCLK (24 MHz)

All 6 sensors share **one** 24 MHz MCLK from `extperiph1`.

| # | Issue | Fix |
|---|---|---|
| 2.1 | The MCLK pin can sit in **GPIO** mode (not SFIO) → no clock at the pin. | Pinmux `extperiph1_clk_po0` → function `extperiph1` placed under **`pinmux@2430000/common`** (a standalone group is ignored — nothing references it in `pinctrl-0`). |

> *Note:* §debug found MCLK was actually live regardless (the pinmux was a red herring for the streaming
> failure — that was Stage 3). The `common`‑group pinmux is kept as the correct, documented config.

### Stage 3 — MIPI CSI‑2 / NVCSI D‑PHY

This was the **real "no frames" blocker.** Symptom: `NVCSI INTR_STATUS 0x8 = PP_FSM_TIMEOUT` +
`tegra-vi4 … PXL_SOF syncpt timeout! err=‑11`, clock lane locks but zero pixel packets arrive.

| # | Issue | Fix |
|---|---|---|
| 3.1 | NVIDIA's RPi‑cam‑v2 reference (which we copied) sets **`discontinuous_clk = "yes"`**. The J106 IMX219 run a **continuous** MIPI clock, and the **tegra186 NVCSI polices the D‑PHY LP escape sequence** unless told to bypass — so it waits forever for an LP sequence the cameras never send. (TX1's older receiver never policed it → "just worked".) | **`discontinuous_clk = "no"`** — sets `T18X_BYPASS_LP_SEQ` (`csi4_fops.c:233`). **The key fix.** |
| 3.2 | Stock PLL = **912 Mbps/lane**, exactly matched to the pixel rate with **zero margin**. On the J106's longer CSI traces this causes intermittent CRC/short‑frame errors and a link that wedges. | **Driver patch `0001`:** lower PLL `0x0307 0x39→0x2B`, `0x030D 0x72→0x55` → **680 Mbps/lane** (−25%) for modes 0–4, **matching the Auvidea TX1 production BSP** exactly. Set DT `pix_clk_hz="136000000"` to match. |
| 3.3 | `cil_settletime` — investigated (manual `17` vs auto `22`). | Left at auto (`0`); a manual value did not survive a soak test. Not load‑bearing once 3.1/3.2 are in. |

### Stage 4 — VI capture & channel routing

| # | Issue | Fix |
|---|---|---|
| 4.1 | `embedded_metadata_height = "0"` → VI4 programs `ATOMP_EMB_SURFACE_OFFSET0 = 0x0` → **SMMU fault** `iova=0x0, fsr=0x402` on stream start. | **`embedded_metadata_height = "2"`** (NVIDIA t186 ref uses ≥1) so a real DMA buffer is allocated. |
| 4.2 | The vi/nvcsi channels inherit **`status="disabled"`** from the stock‑tree merge → no `/dev/video*`. | Set `status = "okay"` on every `nvcsi channel@N` and `vi port@N`. |
| 4.3 | Each sensor must route to its **own** NVCSI channel and VI port. | Distinct `port-index = 0..5`, `remote-endpoint` chains sensor→`csi_in`→`csi_out`→`vi_in`, 6 nvcsi channels (dtsi). Verified: connector CSI‑A..F = bricks 0..5 (J106 reference). |

### Stage 5 — ISP / Argus (`tegra-camera-platform`, userspace)

| # | Issue | Fix |
|---|---|---|
| 5.1 | **DT mode index ≠ driver register‑table index.** DT defined only 2 modes; the driver table has 5. Argus passes the DT mode index straight to the driver → wrong resolution programmed → `ChanselFault PIXEL_LONG_LINE` → ISP never outputs → `nvbuf_utils: dmabuf_fd ‑1`. | Define **all 5 modes in driver‑table order** (`mode0…mode4`): `3264x2464 / 3264x1848 / 1920x1080 / 1640x1232 / 1280x720`. (An earlier 5‑mode fill listed 1280x720 at index 3 instead of the driver's **1640x1232** and a phantom 720p@110 at index 4 — corrected so the DT mode list matches `imx219_frmfmt` exactly; index 3 now exposes the full‑FoV mode.) |
| 5.2 | DT framerates exceeded the lowered (680 Mbps) PLL → `frame_length` too short → `ChanselShortFrame / PIXEL_INCOMPLETE`. | **Derate framerates** to the 680 Mbps clock: **15 / 20 / 30 / 22 / 44 fps** for modes 0–4. The driver `frmfmt` rates were realigned to match (`imx219_22fps` / `imx219_44fps`) so Argus/V4L2 never request a rate the sensor timing can't deliver. |
| 5.3 | Stock `tegra-camera-platform` modules are `status="disabled"`; the overlay merge inherits that → Argus "**No cameras available**". | `status="okay"` on every module + `drivernode0`; add `drivernode1` (`v4l2_lens`) → a lens node. |
| 5.4 | **Argus aliasing:** all 6 modules had `position = "rear"`. libargus keys each camera by `(GUID, position)`; 6× identical **EEPROM‑less** IMX219 all hash to `(GUID 0, position 0)` → every `sensor-id` resolves to the **last** module → *all show one camera*. | Give each module a **unique `position`** (`topleft/topright/centerleft/centerright/bottomleft/bottomright`) + matching badge. Proven in the PCL log: `Found GUID 0 match at index[0..5]` (before) → distinct GUID 0–5, 1:1 (after). NVIDIA docs require unique position for identical modules; stock TX1 `e3322` (same A815P2 IMX219) DTB does exactly this. |

### Stage 6 — `/dev/videoN` & capture usage (not bugs — gotchas)

- **V4L2 mode‑select quirk:** raw capture defaults to **mode0 full‑res 3264×2464@~15 fps** unless you set
  `v4l2-ctl --set-ctrl sensor_mode=N` (2 = 1080p, **3 = 1640×1232 full‑FoV**, 4 = 720p). Raw V4L2 scales to
  all cameras concurrently.
- **Full‑FoV mode (VIO/fisheye):** mode3 **1640×1232 @ 22 fps** is 2×2‑binned over the *whole* sensor array
  (≈160°). The 1280×720 / 1080p modes are center **crops** (1280×720 ≈ 94° H) — use 1640×1232 (or 3264×1848
  for full width) when the wide angle matters. Verified end‑to‑end on `nvarguscamerasrc` (2026‑06‑19).
- **Argus caps gotcha:** pin a **valid** mode/rate or you get `not-negotiated (-4)`: `1640×1232` is 22 fps,
  `1280×720` 44 fps, `1920×1080` 30 fps. There is no 720p@30 and no 720p@110 (that was a phantom DT entry).
- **Argus 5‑session start race (open):** launching 5 `nvarguscamerasrc` simultaneously, one source
  intermittently fails `Internal data stream error` / `not-negotiated`. 4 are reliable; 5 races. Workaround:
  restart `nvargus-daemon`, launch, and if no error in ~9 s record, else retry (a short loop lands it).

**Result:** all wired cameras stream raw V4L2 and through Argus; deliverable
[`captures/tx2_grid_5cam.mp4`](captures/tx2_grid_5cam.mp4) — 2×3 grid, 30 fps, ~38 s, 5 distinct cameras
(A, C, D, E, F; B has no sensor) — the TX2 equivalent of the TX1 grid.

### Stage 5b — ISP image quality (Argus): the missing IMX219 tuning, and the fix

Out of the box the Argus images are **hazy / washed‑out with a magenta cast**. Root cause (verified against
sources): the ISP colour tuning lives in a **`camera_overrides.isp`** file in `/var/nvidia/nvcam/settings/`
(loaded by libargus) — **not** in the imx219 driver (Auvidea's TX1 driver patch only adds sensor modes, no
colour). IMX219 is a *reference* sensor on **tegra210** (Nano/TX1 = the RPi Camera v2), so its tuning is
built into that camera stack; on **tegra186 (TX2)** IMX219 was never a reference camera, so there's **no
built‑in tuning** → generic defaults → the washed/magenta look. (My earlier note that "Auvidea is tuned" was
wrong: neither the TX1 R24 BSP nor Auvidea's patches ship an `.isp` — confirmed, both have 0.)

**The fix — a drop‑in ISP override (verified working on the TX2/tegra186):**
The community **Arducam `camera_overrides.isp`** loads on tegra186 and corrects both the colour matrix and the
black level. Before/after on the same camera, override the only variable:

- **Port F** (content scene): [`captures/isp_F_compare.jpg`](captures/isp_F_compare.jpg) — washed purple haze →
  natural (black connectors, warm desk, bright LEDs). R/G/B 97/**80**/89 → 64/**72**/57, contrast std 7.8 → **43** (≈5.5×).
- **Camera A** (plain wall): [`captures/isp_override_compare.jpg`](captures/isp_override_compare.jpg) — R/G/B
  101/**85**/95 (magenta) → 89/**89**/81 (neutral), contrast std 10.7 → **52.9** (≈5×).
- **Multi‑cam grid**, override active: [`captures/tx2_grid_override10.mp4`](captures/tx2_grid_override10.mp4)
  (natural colour) vs the default‑ISP grid [`captures/tx2_grid_10s.mp4`](captures/tx2_grid_10s.mp4) (purple haze).

The magenta cast disappears (green deficit gone, neutral whites) and the haze clears (real blacks).
Install (the file is in [`tools/nvcam-settings/camera_overrides.isp`](tools/nvcam-settings/); persists in the
rootfs across reboots):
```bash
sudo cp camera_overrides.isp /var/nvidia/nvcam/settings/camera_overrides.isp
sudo chmod 664 /var/nvidia/nvcam/settings/camera_overrides.isp
sudo chown root:root /var/nvidia/nvcam/settings/camera_overrides.isp
sudo rm -f /var/nvidia/nvcam/settings/nvcam_cache_*.bin    # force re-read
sudo systemctl restart nvargus-daemon
```
> ⚠️ The daemon logs two non‑fatal warnings (`Invalid isp config attribute: em.preset[*].wbgains=…`) — a few
> preset attributes from a different L4T version are ignored, but the core CCM/black‑level/tone tuning applies.
> Arducam notes the tuning isn't lens‑specific (NVIDIA hasn't opened CamTune), so it's a strong baseline, not a
> perfect calibration.

Further options (now optional, not required): `nvarguscamerasrc` runtime params (`saturation`, `tnr`, `ee`,
`exposurecompensation`) on top of the override for extra punch; or raw V4L2 + a custom **CUDA ISP**
(debayer → WB → CCM → gamma) for full control + lowest latency (the "simpler HW + lower latency" route).

---

### Stage 7 — Port F: IMX296 global shutter (mixed 5×IMX219 + 1×IMX296)

Port F was repopulated with a **Sony IMX296LQR** (colour global shutter). R32.7.6 has **no `imx296`
driver at all**, so the sensor was invisible and the board ran 5 cameras. Fixed by
[`patches/0002-imx296-tegracam-j106.patch`](patches/0002-imx296-tegracam-j106.patch) plus port-F
changes in the camera dtsi. Everything below was measured on the board, not assumed.

| Issue | Symptom | Fix |
|---|---|---|
| No driver in R32.7.6 | sensor absent from V4L2/Argus; 5 cameras | new tegracam driver (patch `0002`), modelled on this tree's `imx219.c`, register semantics from the Sony datasheet with mainline `imx296.c` as cross-check |
| Sensor answers at **two** i²c addresses | `0x18` *and* `0x34` both ACK on i2c‑7 | one die: IMX296 has a `SLAMODE`-strapped address (`0x36`, so this module straps SLAMODE=0) **and** a common address (`0x1a`) valid in both polarities; the carrier shifter XORs addr bit 1 → `0x34`/`0x18`. DT declares **`0x18`** (from the common address) so it survives a differently-strapped module |
| `SENSOR_INFO` reads 0 | identification appears to fail | that register only reads while **out of standby** — write `CTRL00 = 0` and settle first, then `(v>>6)&0x1ff` must be `296` |
| Shared reset line | would reset the 5 IMX219s | the IMX296 node declares **no `reset-gpios`** and the driver never requests/frees/drives it — unlike `0001`, a from-scratch driver has no such obligation. Per-sensor control = software standby |
| IMX296 needs 37.125/54/74.25 MHz INCK; J106 fans out **24 MHz** | would be unbridgeable | the module is **self-clocked** — `extperiph1` had `enable_count=0` while the sensor still left standby and answered, and the datasheet's power-on sequence requires INCK *before* register communication. Its node names no `mclk` |
| Which INCK? (no register reports it) | unknown oscillator | **measured**: leave `INCKSEL` at the sensor's 74.25 MHz power-on default and read frame rate back — timing scales by (actual/assumed), so 60.4 / 43.9 / 30.2 fps ⇒ 74.25 / 54 / 37.125 MHz. Got 600 frames in 13.905 s = 43.15 fps vs 13.664 s expected for **54 MHz** (+0.24 s fixed stream-start overhead, identical on a 180-frame run); the others were off by 4.0 s and 6.0 s. → `inck-frequency = <54000000>` |
| Single lane | — | IMX296 is a **1-lane** sensor at 1188 Mbps: `bus-width = <1>` at all three hops (sensor → nvcsi → vi) and `num_csi_lanes` 12 → **11** (5×2 + 1×1) |
| Embedded data | mis-framed capture if wrong | the sensor emits **2 lines of DT 0x12 embedded data** per frame → `embedded_metadata_height = "2"`. Full vertical structure: FS + 2 embedded + 6 null (0x10) + 4 optical black (0x37) + active (0x2b) + FE |
| Bayer order | wrong colours | **BGGR** (`pixel_phase = "bggr"`), not the IMX219s' RGGB |

**Timing model (counter-intuitive).** `HMAX` is a line length in units of an internal reference the
sensor holds at **74.25 MHz regardless of INCK**, so all line/frame arithmetic uses that constant.
Datasheet closes the loop: `HMAX` 1100 / 74.25 MHz = 14.815 µs/line, and
1/(`VMAX` 1118 × 14.815 µs) = **60.38 fps** vs the datasheet's stated 60.3.

**Shared-reset settle race (found when C/D/E/F were all populated).** All six J106 CSI resets are
one line, and the imx219 driver pulses it at boot (patch `0001`), so an IMX296 can be probed within
milliseconds of leaving reset. With only a 1 ms wait in `power_on()` the **first sensor probed on
each i2c bus** failed its very first register write with `-EREMOTEIO` while the second succeeded
(`2-001a` failed / `2-0018` bound; same on bus 7) — and raw `i2ctransfer` to the "dead" address
worked fine seconds later, which is what identified it as a settle race rather than a wiring fault.
Fix: honour the datasheet's power-on wait (t4 + t5 + t9 = 200 µs + 21.2 ms + 1.2 ms) and retry the
first register access. Both are in patch `0002`; the retry still fires occasionally on `7-001a`.

**Port ↔ address map with two IMX296 per bus** (the shifter XORs address bit 1, so the north sensor
sits at its native address and the south one at native⊕2):

| Port | Bus | Sensor addr | Alias (SLAMODE) | Node |
|---|---|---|---|---|
| C | `3180000` (i2c‑2) | `0x1a` | `0x36` | `imx296_c@1a` |
| D | `3180000` (i2c‑2) | `0x18` | `0x34` | `imx296_d@18` |
| E | `c250000` (i2c‑7) | `0x1a` | `0x36` | `imx296_e@1a` |
| F | `c250000` (i2c‑7) | `0x18` | `0x34` | `imx296_f@18` |

**Verified on the board** (2026‑08‑24, `LABEL j106imx296`):
- `imx296 7-0018` binds; probe logs `detected IMX296LQ (colour) (sensor info 0x4a00)`.
- **No regression**: all five IMX219s still bind (`1-0010 1-0012 2-0010 2-0012 7-0010`) and still capture.
- `/dev/video5` = `vi-output, imx296 7-0018`; raw V4L2 `BG10` 1456×1088 @ ~60 fps, 600/600 frames, no drops.
- Argus enumerates **6** cameras (`sensor-id=0..5` all OK); sensor-id 5 advertises
  `1456 x 1088 FR = 60.300000 fps`, gain 0–48 dB, exposure 30 µs–500 ms — exactly the DT ranges.
- Controls verified by register read-back: `gain=400` → `GAIN`(0x3204) = `0x0190` = 400;
  `exposure=16000 µs` → `SHS1` = 39, matching `1118 − (16000 µs × 74.25 MHz − 1059)/1100`.
- Capture: [`captures/imx296_portF_argus.jpg`](captures/imx296_portF_argus.jpg).

⚠️ **Gotcha — `override_enable=1` collapses the sensor to 2 fps.** With `override_enable=1`, tegracam
re-applies the `frame_rate` control at *every* stream start, and that control sits at its DT
**minimum** (`min_framerate = "2000000"` = 2 fps, not its 60.3 fps default). That sets `VMAX` = 33750,
the sensor genuinely streams at 2 fps, and VI then fails with `PXL_SOF syncpt timeout` — and it
*sticks for the rest of the boot*. This is tegracam behaviour (IMX219's DT has the same
`min_framerate`), not a driver bug: with `override_enable` at its default 0 the sensor streams fine.
If raw capture suddenly returns 0 bytes, check `VMAX` and reboot to clear the control.

### Stage 8 — Hardware trigger: synchronising four IMX296 (`XTRIG`)

Each IMX296 free-runs on its own on-module oscillator, so the four frame clocks drift against
each other without bound — exactly the failure a global shutter is supposed to remove.
**Measured baseline** (`tools/j106-sync-check.py -n 600`, no trigger wiring):

```
worst skew 2430.0 us over 20.0 s; worst drift 8.33 us/s
verdict: NOT synchronised — free-running.
```

The cameras sit up to **2.4 ms apart**, at a phase that is **re-randomised on every stream
start** (so it cannot be calibrated out once), drifting a further 3–8 µs/s.

**The trigger cannot come from the TX2.** On tegra186 only a handful of pads can mux to a PWM
function (`gp_pwm6_pl6`, `gp_pwm7_pl7`, `uart5_rx_px5`, a few DIS/SEN); on this carrier the only
one broken out is `FAN_PWM`, already owned by `pwm-fan` through an inverting open-drain MOSFET,
and `uart5_rx_px5` is hogged to `spi3` for the IMU. Live check: **no pin on the board is muxed to
PWM at all**. Free GPIOs exist (`GPIO11_AP_WAKE_BT` = gpio 389 = `GPIO_PQ5_PI5`, unclaimed, on
M110 `J21` pin 8) but only as *software-toggled* outputs — and in Fast Trigger mode **the `XTRIG`
low pulse width *is* the exposure time**, so scheduler jitter would land straight on image
brightness. The trigger is therefore generated by a **WeAct MiniSTM32H7**.

**The modules' `XTR+`/`XTR−` pads turned out to be an optocoupler LED**, galvanically isolated from
module ground — measured, not assumed: 1.2 V forward one way and OL reversed, with both legs open to
a ground reference verified at 3.3 V powered / 1.9 kΩ unpowered. That changed the circuit and
removed its most dangerous part:

- The 1.8 V level-translation problem **disappears**. `XTRIG` is never touched from outside; the
  opto's own output drives it inside the module. No divider, no translator, no way to over-volt the
  sensor.
- **No ground is shared** with the cameras — the LED cathode returns to the MCU's own ground.
- But it needs **current, not voltage**: ~10 mA per LED, and four in parallel would be 40 mA from
  one pin, over the STM32's 25 mA per-pin limit. So it is **one channel per camera** —
  `TIM5_CH1..CH4` on `PA0/PA1/PA2/PA3` — all on **one 32-bit counter**, so the frame start stays
  identical across cameras by construction while each channel can carry its own exposure. **8 conductors
  (4 signals + 4 returns, as four twisted pairs) and no external components**: the vendor manual
  identifies the part as a **TLP281** with its own **`R4 = 200 Ω`** on the module, giving 10.3 mA at
  3.3 V. Adding a resistor only starves it.

Two things the isolation hides, both runtime-settable in the firmware rather than compiled in:
`pol` (whether driving the LED asserts `XTRIG` or releases it — unknowable from this side of the
barrier) and `skew` (the opto's turn-on/turn-off asymmetry, which lands straight on exposure since
pulse width *is* exposure). The skew is identical across cameras, so it affects **absolute exposure
only, never synchronisation** — but it does raise the shortest usable exposure from the sensor's
14 µs to roughly 100 µs.

| Decision | Why |
|---|---|
| **Fast Trigger**, not Sequential Trigger | Fast Trigger is a *master*-mode feature — which is how these modules are already strapped, proven by the fact that they free-run at all. One wire (`XTRIG`). Sequential Trigger is slave-mode only and needs `XVS` **and** a 67.5 kHz `XHS` fanned to four boards. |
| Mode registers written **between** clearing `STANDBY` and releasing `XMSTA` | The sensor forbids changing fast-trigger mode during operation, so the change rides on the standby cycle stream-start already performs. Same ordering as the Raspberry Pi `imx296` driver. |
| `SYNCSEL` written as a **whole byte** (`0xc0`/`0xf0`) | Bits `[7:6]` must stay `11b`. The datasheet documents only the `[5:4]` field; a read-modify-write of just those bits would clear reserved bits. Only the RPi driver records this. |
| `VMAX` **pinned** to the readout length while triggered | Frame period comes from `XTRIG`, so `VMAX` only describes readout. Leaving it under the `frame_rate` control re-triggers the `override_enable` trap in Stage 7 (2 fps → `VMAX` 33750 → `PXL_SOF syncpt timeout`, sticky for the boot). In trigger mode that is now structurally impossible. |
| Module **parameter**, not a DT property | One shared net ⇒ one global mode ⇒ nothing per-sensor to express. And it needs no DTB rebuild and no reboot, which makes the free-running/triggered A/B — the acceptance test — a two-second operation. Name/semantics match the RPi driver: `trigger_mode`, `0`=default, `1`=XTRIG. |

Exposure moves out of the sensor: `t_exp = t_pulse + 14.26 µs`. So `set_exposure()` stops writing
`SHS1` while triggered (it still returns success — failing it would fail the stream — but says once
per stream that the value is ignored). Max rate is **59.95 fps** (`tTGPD` = 1126 H = 16.6815 ms),
not a firmware limit.

Exposure is shared across cameras only if you choose to set it that way — the four channels are
independent. [`hw-trigger/WIRING.md`](hw-trigger/WIRING.md) §3 records the measurements that
identified the input, and §4.1 the resistor sizing and per-camera pin map.

Deliverables: [`patches/0003-imx296-external-trigger-j106.patch`](patches/0003-imx296-external-trigger-j106.patch)
(no DT change), [`hw-trigger/`](hw-trigger/WIRING.md) (wiring + STM32H7 firmware),
`tools/j106-trigctl.py`, `tools/j106-sync-check.py`.

Enable, once wired:
```bash
echo 1 | sudo tee /sys/module/imx296/parameters/trigger_mode   # 0 = free-running (default)
```


#### RESULT — SYNCHRONISED (2026‑08‑27/28)

Wired, triggered, and verified on all four cameras (ports C–F):

```
per camera        frames  dropped   mean interval   jitter (sd)
  video0 (C)         300        0      33332.5 us       1.9 us   (30.00 fps)
  video1 (D)         300        0      33332.5 us       1.9 us   (30.00 fps)
  video2 (E)         300        0      33332.5 us       2.0 us   (30.00 fps)
  video3 (F)         300        0      33332.5 us       1.9 us   (30.00 fps)

skew vs video0     median 0.0 us    max 1.0 us    drift 0.00 us/s
verdict: SYNCHRONISED — skew is bounded and not drifting.
```

| | Free-running baseline | Hardware triggered |
|---|---|---|
| Worst inter-camera skew | 2.43 ms | **1.0 µs** |
| Drift | 8.33 µs/s | **0.00 µs/s** |
| Dropped frames | — | **0 / 300** per camera |

Verified rate range **5–59 Hz**. The floor is tegra VI's hardcoded
`chan->timeout = msecs_to_jiffies(200)` (`vi4_fops.c:1089`) — not a sensor limit.

**Independently confirmed without trusting the timestamps.** `tools/j106-sync-frames.py` grabs raw
frames from every `/dev/videoN` and keeps only the set whose V4L2 timestamps match (spread 0.0 µs).
With a running clock in the scene, all four cameras read the **same digits**, with the same digit
caught mid-transition — see `captures/sync_proof_timer_zoom.png`. This validates the timestamps
rather than assuming them.

⚠ **A live RTP grid cannot be used to judge synchronisation.** `csi_sender.sh` → `csi_receiver.sh`
passes each camera through its own 100 ms `rtpjitterbuffer` before the compositor, so cells sit a
frame or two apart (~66 ms) even with perfect sensors — the transport error is ~50,000× the thing
being measured. Judge sync only from raw same-timestamp frames.

**Three defects had to be fixed before any of this worked:**

1. **`0x30af` was `0x09`, must be `0x0b`** (patch `0002`). One byte that silently disabled XTRIG:
   the sensor armed perfectly (`TRIGEN=1`, `LOWLAGTRG=1` read back live) and produced zero frames,
   only `PXL_SOF syncpt timeout`. Found by diffing the *complete* 41-entry init table against the
   Raspberry Pi driver — the only difference. Register state looked correct throughout.
2. **`discontinuous_clk` must be `"yes"`** for the IMX296 nodes. In trigger mode the clock lane
   drops to LP between frames; declaring it continuous made NVCSI police LP sequences it should
   ignore (`CILA_ERR_INTR_STATUS 0x0f` storm). The IMX219 macro must stay `"no"`.
3. **`jetson_clocks` is required for concurrent multi-camera capture** — and is *not*
   trigger-related. Without it three cameras give 12.8/17.6/11.9 fps triggered and 18.0/15.6/10.4
   free-running; each alone reaches 30.00 fps either way. `nvpmodel` MAXN is not sufficient.
   Installed as `jetson-clocks.service`, verified across a reboot.

**Argus AE must be locked in trigger mode.** Exposure is the pulse width, so AE cannot move its
actuator (`ignoring <n>` in dmesg) and hunts on gain instead: a measured **3.47 Hz limit cycle,
150 luma peak-to-peak (171% of mean)**, seen as heavy flashing. Not a mains beat — that would be a
1–3 frame period, this was ~8.7 frames. `csi_sender.sh` now locks AE when the driver reports
trigger mode: p2p 150.5 → 0.8, same mean brightness.

**Polarity matters and the frame rate will not reveal it.** Both polarities rate-lock; `pol 1`
silently yields *exposure = period − commanded*. A raw-sensor sweep at 30 Hz shows brightness
rising with commanded exposure at `pol 0` (959→1027 for 1–15 ms) and falling at `pol 1`
(1112→1043). Use **`pol 0`**.

#### Latency and where to timestamp (measured, IMX296 @1456×1088)

| Stage | Cost |
|---|---|
| Readout + MIPI → NVCSI → VI | **16.1 ms** |
| ISP | **1.9 ms** |
| `SOF → ISP-done` (Argus consumer has the frame) | **18.0 ms** (sd 0.1) |
| Raw-V4L2 buffer age at `DQBUF` | **66.7 ms** (sd 0.03) |

**Bypassing the ISP makes latency worse, not better.** The ISP costs 1.9 ms; the raw V4L2 delivery
path costs 66.7 ms — flat from the first frame, so inherent, not queue depth. Argus hands over
≈1.9 ms after end-of-readout. Use Argus for the pixels; the raw path is for *proving* sync, not for
running an application.

**Stamp at exposure midpoint** — the one instant a global-shutter frame corresponds to:
- Argus: `t_mid = t_SOF − exposure/2`, SOF from the EVENT queue (`tools/latency/argus_evlat.cpp`).
- Raw V4L2: `t_mid = t_buffer − 16.1 ms − exposure/2`; the buffer stamp is `EndOfFrame` on
  `CLOCK_MONOTONIC` (flag `0x00002001`, verified).

Under hardware trigger `exposure` is the exact commanded pulse width, so both corrections are exact
rather than AE estimates — and since all four share one edge, one `t_mid` serves all four.
⚠ `nvarguscamerasrc` GstBuffer PTS is **output-stamped** (~0.7 ms, transport only), *not* SOF —
it looks like a capture timestamp and is not one. The RTP path discards capture time entirely.

## 5a. Camera ⟷ IMU timebase — one clock, and the three ways you cannot get it

The hardware trigger (Stage 8) put the four cameras on one crystal. This section is about putting
the cameras and the **onboard MPU‑9250** on one *timebase*, which is a different problem: the
sensors now agree with each other, but not yet with the IMU, and not with `CLOCK_MONOTONIC`.

Status: **verified on the live board 2026‑08‑28** — IMU reader, frame‑time fit and the combined
recorder all run end to end under the hardware trigger. The one remaining unknown is **Δ**, which
needs the trigger‑echo wire (§5a.5). OpenSpec change `add-camera-imu-sync`.

### 5a.1 The chain, and which clock each link speaks

| Link | Timestamp | Clock | Error |
|------|-----------|-------|-------|
| Trigger edge → sensor exposure | `t_exp = t_pulse + 14.26 µs` (+ opto `skew`) | STM32 crystal | pulse width *is* the exposure |
| Exposure → V4L2 buffer stamp | `+ readout 16.1 ms` (`EndOfFrame`) | **`CLOCK_MONOTONIC`** | per‑frame jitter 1.5 µs sd |
| Buffer stamp → `DQBUF` returns | `+ 66.7 ms` | — | **delivery latency, not a timestamp correction** |
| IMU sample ready → `INT` edge | data‑ready pulse on `gpio-298` | — | + DLPF group delay (below) |
| `INT` edge → userspace wake | `poll()` return | **`CLOCK_MONOTONIC`**, taken by us | **median 50 µs, MAD 2.8 µs** (SCHED_FIFO); p99 75 µs |
| Camera ⟷ IMU | **Δ** | — | *to measure* — trigger echo, or Kalibr |

Both ends are deliberately `CLOCK_MONOTONIC`, because that is what V4L2 stamps buffers with
(flag `0x00002001`, verified).

**The frame times are fitted, not read.** The trigger is hardware‑periodic, so frame times lie on a
line: fit `t[k] = a·k + b` **indexed by the V4L2 `sequence` field**. With 1.5 µs of independent
jitter the phase error falls as 1/√N — about 0.06 µs over 600 frames. Indexing by arrival order
instead of `sequence` is not a small error: a single dropped frame in 600 shifts the fitted period
by ~5800 ppm. The slope `a` is the trigger period *in Tegra clock units*, which is what absorbs the
next fact.

**The STM32 free‑runs against the Tegra.** Measured over 900 triggered frames, the fitted period is
**33334.03 µs** against a commanded 33333.33 µs. Taken at face value that is +21 ppm — but most of
it is not the crystal at all, it is NTP (§5a.4). De‑slewed, the crystal difference is
**−12.3 ppm**, reproducible to 0.07 ppm across runs whose raw figure differed by 3.7 ppm. Either
way the rig walks against `CLOCK_MONOTONIC`, so a camera↔IMU offset calibrated once as a constant
**goes stale**; the fit is what keeps it valid.

**The MPU‑9250's DLPF adds group delay, and adds a different one to each half.** The data‑ready edge
marks when the *filtered* sample was ready, so the instant it describes is earlier by the group
delay. At every matched bandwidth setting the **gyro lags the accel by ≈1.0 ms**:

| `DLPF_CFG` | gyro BW / delay | `A_DLPF_CFG` | accel BW / delay |
|---|---|---|---|
| 1 | 184 Hz / 2.9 ms | 1 | 218 Hz / 1.88 ms |
| 2 | 92 Hz / 3.9 ms | 2 | 99 Hz / 2.88 ms |
| 3 | 41 Hz / 5.9 ms | 3 | 45 Hz / 4.88 ms |

A front end that treats one timestamp as covering both inherits that 1.0 ms. `j106-imu-read.py`
records both delays in the CSV header rather than silently applying them.

### 5a.2 The three designs this hardware cannot support

Each is the obvious way to do it, and each is a dead end **here** — recorded so they are not
attempted again:

1. **MPU‑9250 `FSYNC`** — the textbook trick (feed the camera trigger to `FSYNC`, let the IMU tag
   its own samples with the frame) — **is not brought out by the J106**. The IMU pin table has only
   `AD0/SDO`, `SDA/SDI`, `SCL/SCLK`, `/CS`, `INT`.
2. **Tegra GTE hardware GPIO timestamping** — `gpio-tegra186.c` has the hooks
   (`tegra_gpio_timestamp_control`, the `use-timestamp` DT property), but the pin table is
   `tegra194_gte_info[]` **only**: GTE is **Xavier‑only**. `CONFIG_TEGRA_HTS_GTE` is off and neither
   GPIO node declares it.
3. **The GPIO chardev's own event timestamp** — it is **`CLOCK_REALTIME`**
   (`gpiolib.c:730`, `ge.timestamp = ktime_get_real_ns()`), while V4L2 is `CLOCK_MONOTONIC`. Mixing
   them misdates everything by the REALTIME↔MONOTONIC offset, and NTP can slew one under you. Use
   the chardev to **wait** for the edge, then take your own `clock_gettime(CLOCK_MONOTONIC)`.
   (`j106-imu-read.py --latency` is the one place the chardev stamp is used at all — comparing it
   against our own read is precisely what measures the wake‑up latency.)

So: wake userspace on the edge and stamp it there. That is the best this board allows, and
`--latency` is what puts a number on the cost.

### 5a.3 What waking on the edge actually costs (measured)

`j106-imu-read.py --latency`, 3000 samples at 200 Hz, comparing the kernel's IRQ‑time
`CLOCK_REALTIME` event stamp against our own `CLOCK_MONOTONIC` read:

| | normal priority | `chrt -f 80` |
|---|---|---|
| median | 56.4 µs | **50.4 µs** |
| MAD | — | **2.8 µs** |
| p95 | 100.0 µs | 55.7 µs |
| p99 | 122.7 µs | 75.4 µs |
| rare tail | — | ~3.4 ms, roughly 1 in 3000 |

**The median is bias, not error** — a constant that Δ absorbs. What limits the IMU timestamp is the
**MAD, 2.8 µs**, which is the same order as the camera side's 1.5 µs frame jitter. Run under
`SCHED_FIFO`: it halves the tail and barely moves the median.

⚠ The `clock_gettime` call itself costs **~5.3 µs** here (ctypes into the vDSO on this CPU) — larger
than the jitter being measured. It does not enter the figures because the `REALTIME` read is
bracketed between two `MONOTONIC` reads and dated to their midpoint, which cancels the call cost to
first order.

Also measured, and reported by the tool rather than corrected: the accelerometer reads
**|a| = 10.46 m/s² at rest**, i.e. 66 mg from 1 g. Orientation cannot change a magnitude, so that is
the part's uncalibrated zero‑g offset (±60 mg typical) — exactly what a VIO/Kalibr calibration
estimates. A wrong full‑scale selection would be out by a factor of two, not 7 %.

### 5a.4 ⚠ NTP slews `CLOCK_MONOTONIC` — and it is the biggest term in the rate

`CLOCK_MONOTONIC` is **not** free of NTP. Only `CLOCK_MONOTONIC_RAW` is. Measured on this board:

```
CLOCK_MONOTONIC is slewed +48.45 ppm relative to CLOCK_MONOTONIC_RAW
```

That is **four times larger** than the crystal difference the fit is trying to report, so a raw
"STM32 vs Tegra" figure is mostly an NTP artefact. Two consequences, which behave differently:

- **The static offset biases any RATE figure.** `j106-frametime.py` now measures the slew during
  every run (`MONOTONIC` vs `MONOTONIC_RAW`) and reports the de‑slewed crystal figure alongside the
  raw one. Validation: two runs whose raw figures were +24.57 and +20.90 ppm gave de‑slewed
  −12.34 and −12.27 ppm.
- **The servo re‑adjusting that slew makes the fit residual wander.** With `systemd-timesyncd`
  running, the residual over 900 frames is **30.9 µs**, tracing a smooth ±60 µs arc (frame‑to‑frame
  change only 3.25 µs — a wander, not jitter). Stop the daemon and the same fit gives **8.4 µs**.
  Note that stopping it does **not** remove the static slew: the kernel keeps the last `adjtimex`
  frequency correction, which still measured +33 ppm afterwards.

**It does not harm Δ.** Both the V4L2 stamps and the IMU stamps are `CLOCK_MONOTONIC`, so the slew
is common mode and cancels in the camera↔IMU difference. It corrupts statements about *rate*, not
about *offset*. For a long calibration recording, stop `systemd-timesyncd` anyway — it cuts the fit
residual by ~4×.

### 5a.5 Δ — the one remaining constant

After the fit, exactly one unknown is left: the offset between the camera timebase and the IMU
timebase. It is a **single** constant, not four, because all four cameras share one trigger edge.
Two routes, both documented in `hw-trigger/WIRING.md`:

- **Measure it** — echo the trigger into `gpio-389` (M110 `J21` pin 8) and compare the echo edge to
  the frame's fitted time, timestamping the echo **the same way the IMU is timestamped** so the
  ~50 µs wake-up latency is common mode and cancels (the derivation is in `WIRING.md` §4.4; taking
  the kernel stamp instead measures the wrong quantity). ⚠ `gpio-389` is **1.8 V unbuffered**: the
  3.3 V trigger needs a divider — any **two equal resistors** (1 kΩ–47 kΩ) give 1.65 V, which is
  fine. This is the one place on this rig where the level problem still applies; the camera trigger
  pads themselves are optocouplers and do not.
- **Estimate it** — let Kalibr solve for `td` from a recording. No extra wiring.

Until one of those happens, `j106-record-sync.py` writes `delta_us = 0` **and** records
`"delta_source": "UNMEASURED — assumed zero"`, so no recording can quietly imply a calibration it
does not have.

### 5a.6 Tools

| Tool | What it does |
|------|--------------|
| `tools/j106-imu-read.py` | MPU‑9250 stamped on the data‑ready edge; `--latency` measures edge→wake |
| `tools/j106-frametime.py` | the `t[k] = a·k + b` fit; refuses to fit free‑running cameras |
| `tools/j106-record-sync.py` | both on one clock, EuRoC/ASL layout, provenance in `meta.json` |

## 6. Build, apply the patch & flash

Builds run on an **x86‑64 Linux host**; artifacts deploy to the TX2 **over SSH**. Deployment is **always
reversible** via `extlinux.conf` LABELs — the partition DTB (`mmcblk0p30`) is never touched. Build tree
lives in `j106build/` (git‑ignored, reproducible).

### 6.1 Prerequisites (one‑time)
- L4T **R32.7.6** `public_sources` (kernel + DT): `kernel_src.tbz2` → `j106build/r3276/ksrc/`.
- Cross toolchain (NVIDIA Bootlin GCC 9.3 **or** Linaro 7.3.1 — both yield `4.9.337-tegra`).
- The board's own kernel config: `scp nvidia@<board>:/proc/config.gz . && zcat config.gz > board-config`
  (keeps modules compatible).

### 6.2 Build the patched kernel `Image` (applies patch `0001`: shared reset + 680 Mbps, and `0002`: imx296 driver)
```bash
cd j106build/r3276
patch -p1 -d ksrc < ../../patches/0001-imx219-share-reset-gpio-j106.patch   # idempotent; skip if applied
patch -p1 -d ksrc < ../../patches/0002-imx296-tegracam-j106.patch          # port-F IMX296; disjoint from 0001
cp board-config kout/.config   # must contain CONFIG_VIDEO_IMX219=y and CONFIG_VIDEO_IMX296=y
export CROSS_COMPILE=$PWD/l4t-gcc/.../bin/aarch64-linux-gnu-          # or aarch64-buildroot-linux-gnu-
make -C ksrc/kernel/kernel-4.9 O=$PWD/kout ARCH=arm64 CROSS_COMPILE=$CROSS_COMPILE LOCALVERSION=-tegra olddefconfig
make -C ksrc/kernel/kernel-4.9 O=$PWD/kout ARCH=arm64 CROSS_COMPILE=$CROSS_COMPILE LOCALVERSION=-tegra -j$(nproc) Image
# Result: kout/arch/arm64/boot/Image   — verify: strings Image | grep 4.9.337-tegra
#         and the 680 rate compiled in: grep '0x0307, 0x2B' ksrc/kernel/nvidia/drivers/media/i2c/imx219_mode_tbls.h
```

### 6.3 Build the carrier DTB (cameras + USB), 680 timing + unique positions
Approach A — decompile the board's stock DTB and append the carrier overlays (keeps the real running tree):
```bash
cd j106build
dtc -I dtb -O dts stock-c03.dtb -o stock-c03.dts          # from board /boot, or BSP tegra186-quill-p3310-1000-c03-00-base.dtb
# add the two labels the camera dtsi needs (decompiled tree lacks them):
#   clock@5000000 -> tegra_car: clock@5000000   ;   gpio@2200000 -> tegra_main_gpio: gpio@2200000
SRC=../tx2-j106-6csi
cat stock-c03.dts "$SRC/tegra186-camera-j106-imx219.dtsi" "$SRC/override-usb.dtsi" > tegra186-j106.dts
dtc -I dts -O dtb -@ -o tegra186-j106.dtb tegra186-j106.dts 2> tegra186-j106.dtc.log
# decompile/recompile phandle + unit-address warnings are expected and harmless.
# verify positions merged: dtc -I dtb -O dts tegra186-j106.dtb | grep -A1 'module[0-5] {' | grep position
```

### 6.4 Deploy reversibly (over SSH) — all fixes in one shot

**One‑command deploy** ([`tools/deploy-j106.sh`](tools/)) — pushes and installs **all** carrier fixes
(patched Image, carrier DTB, extlinux entry, **`camera_overrides.isp`** ISP fix, and the recovery‑button
service) in one go, reversibly:
```bash
tools/deploy-j106.sh  j106build/r3276/kout/arch/arm64/boot/Image  j106build/tegra186-j106.dtb
# then reboot the board to apply. Env: TARGET=, SSHOPTS=-o ProxyJump=…, SUDOPW=, LABEL=
```
It runs [`tools/j106-install.sh`](tools/) on the board, which: installs the Image+DTB, adds an `extlinux`
`LABEL` (sets it `DEFAULT`, backs up the prior conf), drops `camera_overrides.isp` into
`/var/nvidia/nvcam/settings/` (+ clears `nvcam_cache_*.bin`), and enables `j106-recovery-key.service`.

**Manual equivalent** (if you prefer step‑by‑step):
```bash
sudo cp /tmp/Image            /boot/Image.j106-680
sudo cp /tmp/tegra186-j106.dtb /boot/tegra186-j106.dtb
sudo install -m664 -o root -g root tools/nvcam-settings/camera_overrides.isp /var/nvidia/nvcam/settings/
sudo rm -f /var/nvidia/nvcam/settings/nvcam_cache_*.bin           # ISP image-quality fix (§5b)
sudo cp /boot/extlinux/extlinux.conf /boot/extlinux/extlinux.conf.bak     # first time only
# append a new LABEL (keep the previous one as fallback) and set DEFAULT to it:
#   LABEL j106cam
#         LINUX /boot/Image.j106-680
#         INITRD /boot/initrd
#         FDT   /boot/tegra186-j106.dtb
#         APPEND ${cbootargs} quiet
sudo sed -i 's/^DEFAULT .*/DEFAULT j106cam/' /boot/extlinux/extlinux.conf
sudo reboot
```
(Board sudo password is `nvidia`: `echo nvidia | sudo -S <cmd>`.)

**Full reflash instead?** Bake the same fixes into the rootfs *before* `flash.sh`: copy the patched `Image`
+ DTB into `Linux_for_Tegra/kernel/`, and the ISP override into the rootfs at
`Linux_for_Tegra/rootfs/var/nvidia/nvcam/settings/camera_overrides.isp` (plus the recovery service files),
so they ship in the flashed image.

### 6.5 Verify on target
```bash
uname -r                                   # 4.9.337-tegra
ls /dev/video*                             # video0..N (one per probing camera)
dmesg | grep -E 'imx219 .*(bound|failed)'  # which of A/B/C/D/E/F enumerated
v4l2-ctl -d /dev/video0 --set-ctrl bypass_mode=0 --set-ctrl sensor_mode=2 \
         --set-fmt-video=width=1920,height=1080,pixelformat=RG10 \
         --stream-mmap --stream-count=30 --stream-to=/dev/null
# bypass_mode=0 is REQUIRED: without it every camera's raw capture hangs with 0 frames
# (looks exactly like a dead sensor). Also stop nvargus-daemon first and confirm it
# released the node: `systemctl stop nvargus-daemon` then `fuser /dev/videoN` = empty.
# Argus grid (restart daemon first; retry if a source races):
sudo systemctl restart nvargus-daemon
```

> **Live grid on the HDMI screen:** use [`tools/grid-display-x.sh`](tools/grid-display-x.sh) (5 cams +
> placeholder, 2×3). It renders with **`nv3dsink`** (an X window). Do **not** use the older
> `nvoverlaysink`-based grid while the desktop is up: the NVIDIA Xorg driver owns the display controller,
> so the legacy overlay plane can't be acquired and the pipeline never reaches PLAYING (it also throws a
> misleading `nvcompositor` "Impossible to configure latency" clock error). `nvoverlaysink` only works
> from a text console with **X stopped**. A `queue` before each compositor sink pad is required to satisfy
> live-source latency negotiation. Kill it by PID (`kill <gst-launch-pid>`) — a `pkill -f` pattern
> containing `gst-launch`/`nvcompositor` will also match (and kill) your own ssh shell.

### 6.6 Rollback & boot‑hang recovery
**Rollback (board boots):** set `DEFAULT` back to a previous LABEL (e.g. `j106-680-rst` or `primary`) and
reboot. The partition DTB and stock `Image` are never modified.

**Recovery (board hangs — a bad `DEFAULT` DTB):** if a deployed DTB wedges the kernel (no network, silent
if that label's `APPEND` lacks `console=`), recover at the **U‑Boot extlinux menu over the debug UART**
(`/dev/ttyUSB0` @115200) — no reflash:
1. Power‑cycle while capturing the UART. **Do not press a key during "Hit any key to stop autoboot"** (that
   drops to the U‑Boot shell and skips the menu).
2. After `L4T boot options` / `Enter choice:` appears, send the **known‑good label number + Enter** (e.g.
   `6` = `j106-680-rst`). `TIMEOUT 30` = a ~3 s window; a tiny pyserial watcher that types the digit when it
   sees `L4T boot options` is reliable.
3. Once booted, make it durable: `sudo sed -i 's/^DEFAULT .*/DEFAULT j106-680-rst/' /boot/extlinux/extlinux.conf`.

(June 2026: `DEFAULT j106-fan` → `tegra186-j106-imu-fan.dtb` hung at `Starting kernel`; recovered this way.)

---

### 6.7 Changing the camera population (IMX219 ⇄ IMX296)

Tegra binds sensors from a **static device tree**, so each CSI port's sensor family is fixed by the
DTB. Changing which family a port carries therefore means booting a different DTB — see
*"why one device tree cannot serve both"* below. In practice you do not hand-edit anything.

#### The whole procedure, one command

Run on the **build host** (this repo's machine):

```bash
./tools/j106-camera-config.py --detect                # what is fitted right now?
./tools/j106-camera-config.py --imx296 C,D,E,F        # apply: build → deploy → reboot → verify
./tools/j106-camera-config.py --imx296 F --no-deploy  # build the DTB only, touch nothing
./tools/j106-camera-config.py --imx296 C,D --no-reboot # deploy but leave the reboot to you
```

Ports not listed under `--imx296` default to **IMX219**. Each population gets its own DTB and
`extlinux` LABEL (`cam296-cdef`, `cam296-f`, …), and older LABELs stay in `extlinux.conf`, so:

- a population **already built** is reused — just a LABEL selection and a reboot, no rebuild;
- **switching back** is re-running the command with the old port list;
- **rollback** is the usual §6.6 route (pick a previous LABEL at the U-Boot menu).

`--target` selects the board (default `nvidia@10.42.0.157`).

⚠️ **Which machine each script runs on** — the two camera tools are opposites, and they now refuse to
run in the wrong place rather than failing obscurely (the board *does* have `cpp` and `dtc`, so the
error would otherwise be a confusing missing-file one):

| Script | Runs on | Why |
|---|---|---|
| `j106-camera-config.py` | **x86‑64 build host** | cross-builds the DTB (needs `cpp`, `dtc`, the `j106build/` tree) and reaches the board over SSH |
| `j106-detect-cameras.sh` | **the board** | probes the live i²c buses — `j106-camera-config.py --detect` ships it there for you |

#### What that command actually does (and the manual equivalent)

Per port whose family changes, in `tegra186-camera-j106-imx219.dtsi`:

| Step | IMX219 → IMX296 | IMX296 → IMX219 |
|---|---|---|
| 1. Sensor node | `imx219_x@10`/`@12` → `imx296_x@1a`/`@18`, `IMX296_HW_RESOURCES`, **one** `mode0` | reverse, `IMX219_HW_RESOURCES`, **five** `mode0..4` |
| 2. Lane width | `<2>` → `<1>` at **all three hops**: sensor endpoint, `nvcsi channel@N`, `vi port@N` | `<1>` → `<2>` |
| 3. Lane budget | recompute `num_csi_lanes` (IMX219 = 2 lanes, IMX296 = 1) | same |
| 4. Argus module | `badge`, `devname`, `proc-device-tree` **inside that port's `moduleN` block** | same |

then rebuild the **DTB only** (§6.3) and deploy (§6.4). The kernel never needs rebuilding — both
drivers are built in (`CONFIG_VIDEO_IMX219=y`, `CONFIG_VIDEO_IMX296=y`).

Two traps the script handles and a hand-edit easily gets wrong:
- step 4 must be **scoped to `moduleN`** — ports share a bus pairwise (A/B, C/D, E/F), so an
  unscoped `devname` edit silently rewrites the *sibling* port's Argus entry;
- step 2 must hit **all three** hops; missing one surfaces as a confusing capture-time failure rather
  than a probe error.

Commit `2dbf7f9` is this transformation done by hand, as a readable reference.

#### Special case: refitting IMX219 on the currently-empty A/B needs **nothing**

The `imx219_a@10` / `imx219_b@12` nodes were deliberately **left in the tree** — 2-lane routing on all
three hops, their own NVCSI channels 0/1, unique `module0`/`module1` Argus entries, and
`num_csi_lanes` already counting them at 2 lanes each. Plug the modules in and reboot; an unpopulated
node simply fails probe harmlessly, which is what the current boot log shows.

#### Why one device tree cannot serve both families on one port

Each sensor node's `remote-endpoint` binds **1:1** to an NVCSI channel, and lane width is a *static*
property of that channel — two sensor nodes pointing at one channel make the media graph ambiguous.
The i²c addresses do **not** collide (`0x10`/`0x12` vs `0x1a`/`0x18`), so an IMX219 and an IMX296 can
share a **bus**; they cannot share a **port**.

**Raspberry Pi does not merge them either** — it *auto-selects*:

| | Raspberry Pi OS | This board (L4T R32.7.6 / TX2) |
|---|---|---|
| Detection | firmware probes the CSI i²c bus at boot | none — Linux needs a DT before it can probe |
| Selection | `camera_auto_detect=1` loads the matching `imx219.dtbo` / `imx296.dtbo` | one pre-built **DTB per population**, chosen by an `extlinux` **LABEL** |
| Force a sensor | `camera_auto_detect=0` + `dtoverlay=imx296` (`,cam0`/`,cam1`) | `DEFAULT <label>` in `extlinux.conf` |
| ISP tuning | libcamera picks `imx296.json` **by sensor name** | `camera_overrides.isp` is **global** — swapped together with the DTB |

R32.7.6's `extlinux` has no `OVERLAYS` key (that arrived with R35/Orin), and the 43 `.dtbo` files on
the board are NVIDIA's stock overlays for *other* boards, referenced by nothing. So one-DTB-per-
population is not a workaround here — it is the only mechanism. The *selection* could be automated
(a boot service running the detect script and rewriting `DEFAULT`); the *merging* cannot.

#### System settings that move together

Changing sensor family is never only the DT. `j106-camera-config.py` handles 1–3; **4 is on you**:

1. **DTB / extlinux LABEL** — sensor nodes and lane widths (rollback §6.6).
2. **ISP tuning** — `camera_overrides.imx296.isp` or the Arducam `camera_overrides.isp`. The script
   picks by majority family. **Global to every sensor**, so a mixed board cannot have both families
   correct; whichever loses should consume **raw V4L2**, which bypasses the ISP and is correct for both.
3. **AE clamp** — `ARGUS_EXP`/`ARGUS_GAIN`/`ARGUS_DGAIN` (§7). Defaults suit the IMX296; IMX219
   tolerates far longer exposures (its DT allows up to 683 ms).
4. **`/dev/videoN` indices shift.** Adding A/B pushes C–F from `video0..3` to `video2..5`. The grid
   tools resolve cameras by i²c name and are unaffected; anything of yours pinning node numbers is not.
5. **Hardware trigger coverage** (§5 *Stage 8*) — **IMX296 only**. `imx296.trigger_mode=1` drives
   every IMX296 that probes, so a port switched **to** IMX296 joins the triggered set and needs its
   `XTR±` pair wired to a free STM32 `TIM5` channel (`PA0`–`PA3`, `WIRING.md` §4.1) or it will sit
   in trigger mode with no pulses and produce **no frames at all**. A port switched **away** from
   IMX296 leaves its STM32 channel spare and drops out of the synchronised set — so the acceptance
   test's expected camera count changes with it. IMX219 has no trigger support: a mixed population
   can only ever be synchronised across its IMX296 ports.
6. **`jetson-clocks.service`** — not family-specific, but it scales with the *number* of cameras:
   three concurrent streams collapse to ~12–18 fps with syncpt timeouts without it, while each
   camera alone reaches 30 fps either way. `nvpmodel` MAXN is **not** enough. Adding ports to a
   population is exactly when this starts to matter.

## 7. Status & open issues

**Deployed config — default boot `j106imx296`** (reversible via `extlinux` LABELs; fallbacks kept,
incl. `j106fullfov`, `j106-imu`, `j106-680`): `Image.j106imx296` (4.9.337‑tegra #6, patches `0001`
= imx219 shared reset + 680 Mbps, and `0002` = **new imx296 driver**) + `j106imx296.dtb`
(**4×IMX296 on C–F**, IMX219 nodes kept for A/B, USB VBUS fix, onboard MPU‑9250 IMU).
Deployed & verified 2026‑08‑25. Board: `ssh nvidia@10.42.0.157` (pw `nvidia`); board sudo
`echo nvidia | sudo -S …`.

> **Camera population changed again 2026‑08‑25**: ports **C, D, E, F all carry Sony IMX296LQR**
> global-shutter modules (`2-001a`, `2-0018`, `7-001a`, `7-0018` → `/dev/video0..3`), and **A/B are
> currently unpopulated** — their `imx219_a@10` / `imx219_b@12` nodes are deliberately left in the
> tree so refitting IMX219 modules there needs **no DT change**. See §5 *Stage 7*.

### ✅ Working
- **IMX296 global shutter on ports C–F** — `2-001a`, `2-0018`, `7-001a`, `7-0018` →
  `/dev/video0..3`, raw V4L2 `BG10` 1456×1088 @ ~60 fps and all four enumerated by Argus. New
  tegracam driver (`patches/0002`); INCK **measured** at 54 MHz; single-lane routing; no reset-gpio,
  so nothing touches the shared reset line. See §5 *Stage 7* (incl. the shared-reset settle race and
  the `override_enable=1` → 2 fps gotcha).
- **USB host ports** (M110) — VBUS fix in `override-usb.dtsi` (stock routes VBUS through devkit
  `pca953x` expanders absent on the carrier → `‑517` defer; fixed regulator forced always‑on).
- **Fan control** — needs **two independent** fixes (both now in place):
  1. **PWM control** — fixed in `override-usb.dtsi`. Stock gates `vdd-fan` (`regulator@13`) through the
     *same* absent devkit I²C expander, so the regulator never registers → `FAN: couldn't get the regulator`
     → `pwm-fan` won't probe → no PWM control → fan stuck **always on at full**. Same fix (drop the expander
     gpio, force the regulator always‑on): `pwm-fan` probes, registers as a cooling device under
     `thermal-fan-est`, and the kernel ramps PWM with temperature (`target_pwm` 80/120/160/255 at the
     **51/61/71/82 °C** trips; `0` when cool).
  2. **Fan‑enable line** — the J106 fan header (J12) is gated by an **inverting MOSFET** off the module's
     **AUD_RST** signal (`GPIO_J6` = sysfs **gpio 398**, `GPIO1_AUD_RST`): high = disabled, low = enabled.
     The rt5659 codec is absent so AUD_RST floats **high** → the fan never physically spins even though PWM
     is ramping (the "verified `target_pwm=0` at 33 °C" above only proved *PWM* control, not airflow). Fix =
     hold AUD_RST **low** via the **userspace** [`tools/j106-fan-enable.service`](tools/j106-fan-enable.service)
     (oneshot, drives gpio 398 low at boot; the governor still owns speed). Verified end‑to‑end with thermal
     emulation: 55 °C → PWM 80 → ~1950 rpm, 65 °C → PWM 120 → ~2800 rpm; off when cool.
     ⚠️ **Do NOT use the device‑tree gpio‑hog** (`tx2-j106-6csi/fan-enable.dtsi`): the DTB that bundled it
     (`tegra186-j106-imu-fan.dtb`, extlinux `LABEL j106-fan`) **hangs the kernel at boot** — recover per §6.6.
  3. **Resume re‑sync** — fixed by [`tools/j106-fan-resync`](tools/j106-fan-resync) (a `post`‑resume hook in
     `/lib/systemd/system-sleep/`). Fan *speed* is driven by `pwm-fan` on the **AON PWM** (`pwm@c340000` =
     pwmchip3, always‑on/SPE domain). When cool the governor sets `target_pwm=0` and pwm‑fan **disables** the
     channel instead of actively driving 0 % duty. At cold boot the disabled pad leaves the fan off, but across
     **SC7 suspend/resume the AON‑PWM state is lost and not restored** → the pad comes back driving the fan at
     **full**, and because the governor still computes `target_pwm=0` (`target == cur == 0`) pwm‑fan never
     re‑issues `pwm_config`/`pwm_enable`, so the fan **runs at full RPM forever** until a real >51 °C trip forces
     a reprogram. Symptom: `cur_pwm=0` but the fan screams (tach ~5800 rpm). Fix = on each resume force one real
     reprogram (`temp_control=0; target_pwm=160; sleep 1; target_pwm=0; temp_control=1`) to re‑sync the hardware,
     then hand speed back to the governor. Verified live: stuck at `cur_pwm=0`/5795 rpm → after the kick →
     `cur_pwm=0`/**0 rpm**. (The AUD_RST enable gate is a separate always‑on line, not involved.) Install:
     `sudo install -m0755 tools/j106-fan-resync /lib/systemd/system-sleep/`.
- **HDMI 5V regulator** — fixed in `override-usb.dtsi`. `vdd-hdmi` (`regulator@3`), gated by the same absent
  expander, made nvdisplay defer with `couldn't get regulator vdd_hdmi_5v0, -517`. Dropping the expander gpio
  clears it (the residual `tegra_hdmi_tmds_range_read failed` is just the EDID read with no monitor attached).
- **Onboard IMU (MPU‑9250) — WORKING** (`tx2-j106-6csi/imu-mpu9250.dtsi`, built into `tegra186-j106-imu.dtb`,
  extlinux `LABEL j106-imu`). The 9‑axis IMU is optional but **fitted on this board** (verified: `WHO_AM_I`
  reg `0x75` = `0x71`, live accel Z ≈ +1 g gravity). The bus mapping is the trap — it does **not** transfer
  from TX1: on **TX1** the IMU is HW SPI4 = `/dev/spidev3.0`; on **TX2** the same connector pins land on HW
  **SPI3 = `spi@c260000` = `/dev/spidev1.0`** (CS0), pinmux **`spi3` on `uart5_rx/rts/cts`** (3 pads;
  console is ttyS0, unaffected). The dtsi muxes those 3 pads, disables the absent devkit touchscreen on
  CS0 (`spi-touch-sharp19x12@0`), and adds a `tegra-spidev`. `spidev` is the `=m` module — for it to load
  at boot (so `/dev/spidev1.0` exists without `modprobe spidev`), the **only extra deploy step**:
  `echo spidev | sudo tee /etc/modules-load.d/spidev.conf`. No kernel patch/defconfig change is needed.
  `/dev/spidev1.0` is root‑only. (Wrong buses, all 0xFF/0x00: `spi@3210000`/gpio_wan,
  `spi@3230000`/gpio_sen, `spi@3240000`/gpio_cam.)
  **IMU + fan‑tach coexist — the key pin trick:** `uart5_tx_px4` is SPI3 **CS1** (a second chip‑select the
  IMU doesn't use — it's on CS0) **and** the same SoC ball as **`FAN_TACH`** (J12 pin 3 / module B17 →
  `tachometer@39c0000`). Muxing it to spi3 silently zeroes the fan RPM readback. So the dtsi **deliberately
  omits `uart5_tx`** — and then **both work simultaneously** (verified: WHO_AM_I `0x71` + live accel **and**
  fan tach ~2300 rpm under load). (Fan *speed* control is independent regardless: `FAN_PWM` = module C16 =
  AON `pwm@c340000`; fan‑enable = AUD_RST service. Note `uart5_cts`=gpio 479 is also the USB‑OTG VBUS‑detect
  extcon, taken for SPI3 — host ports unaffected, OTG device‑mode never worked on J17 anyway.) So `LABEL
  j106-imu` now gives **IMU + fan RPM + cameras + USB** all together.

> **Benign boot messages** (all expected on this carrier — absent devkit chips, not faults):
> `pca953x 0-0074/0-0077 -121` (the routed‑around I²C GPIO expanders), `ina3221x 0-0042/0-0043 -121`
> (devkit power monitors), `imx219 1-0012 -121` (camera B, no sensor), `eqos: failed to read
> eqos_auto_cal_config` (Ethernet works), `tegra_hdmi_tmds_range_read failed` (no monitor).
- **Ethernet** (M110, Tegra EQOS) and **WiFi** — stock, untouched.
- **Cameras** — all wired sensors stream raw V4L2 and through Argus; aliasing fixed (Stage 5.4);
  **5‑camera Argus grid** delivered ([`captures/tx2_grid_5cam.mp4`](captures/tx2_grid_5cam.mp4)).
- **Port D (CSI‑D `2-0012`) reliability — FIXED** (Stage 1.5/1.6). Boot‑time shared‑reset **pulse**
  (matching the J106 manual's documented power‑up reset) makes the bus‑2 shifter latch deterministically:
  **10/10 reboots port‑D up** (was a per‑boot lottery, ~2/6 down). Reboot‑free recovery via
  `echo 1 | sudo tee /sys/bus/i2c/devices/<bound-imx219>/j106_reset_recover` — trigger from **any bound
  sibling** (a sensor that failed its boot probe has no recover file of its own); verified: drop port D →
  recover via port C → re‑binds + streams. Deployed kernel: `Image.j106-680-rst` (extlinux `LABEL
  j106-680-rst`, the new default).
- **ISP image quality** — fixed with the Arducam `camera_overrides.isp` (Stage 5b); installed in
  `/var/nvidia/nvcam/settings/`. Washed/magenta → natural colour + real contrast
  ([`captures/isp_F_compare.jpg`](captures/isp_F_compare.jpg)).
- **UART0 debug console** — works: `/dev/ttyUSB0` @ 115200 8N1 on the host (FTDI on the debug header).
- **Micro‑USB recovery / flashing (M110 J17)** — works. `sudo reboot forced-recovery` (or the M110 recovery
  button via `tools/j106-recovery-key`) puts the board into RCM and the host enumerates it as
  `0955:7c18 NVIDIA Corp. T186 [TX2 Tegra Parker] recovery mode` (verified). The M110 PDF calls J17 the
  *"USB 2.0 port for firmware upgrades"* — flashing is its actual purpose.
- **Carrier buttons** — all functional; verified map below. The M110 "Recovery" button now triggers software
  recovery via [`tools/j106-recovery-key`](tools/).

### ✅ Hardware trigger — WIRED, WORKING, SYNCHRONISED (2026‑08‑28)

All four IMX296 (ports C–F) run off one STM32 `TIM5` counter: **30.00 fps each, 0 dropped of 300,
worst inter-camera skew 1.0 µs, drift 0.00 µs/s** — against a free-running baseline of 2.43 ms skew
and 8.33 µs/s drift. Verified 5–59 Hz, and confirmed visually with a clock in the scene
(`captures/sync_proof_timer_zoom.png`). Full detail, the three defects that had to be fixed, the AE
lock, and the latency/timestamp budget are in §5 *Stage 8*.

Deployed as `LABEL j106fix` (kernel `#8`, patches 0001+0002+0003) with `j106trig` and `j106disco`
kept as fallbacks; DTB carries `discontinuous_clk="yes"` for the IMX296 nodes.
`jetson-clocks.service` is installed and required for concurrent multi-camera capture.

The historical pre-wiring notes follow.

- **Hardware trigger for the four IMX296** (§5 *Stage 8*). Everything on the software side was done
  and verified before the wiring existed:
  - `patches/0003-imx296-external-trigger-j106.patch` — compiles clean, `Image` rebuilt and
    verified `4.9.337-tegra`; the patch re-applies onto a pristine tree byte-for-byte. **No DT
    change.**
  - `hw-trigger/firmware/` — **rebuilt as a standard STM32 project and verified on the board**
    (2026‑08‑26). HAL + FreeRTOS + ST's USB Device Library; ~51 kB. Confirmed live over its own USB
    CDC console: `clock=hse25-pll240`, `timer_hz=120000000`, `psc=0 arr=3999998 ccr=598288`, all
    limits refusing correctly, `burst` terminating, and the waveform registers unchanged under ~400
    commands in 9.1 s while pulses advanced at 29.97 fps.
    - **Trigger pins moved** to `TIM5_CH1..4` on `PA0`–`PA3`. `PE9/PE11/PE13/PE14` are the on‑board
      TFT‑LCD connector, so the old pinout made the integrated display unusable. `TIM5` is 32‑bit,
      so pulse resolution went from ~517 ns to **8.33 ns**.
    - **Control link over USB‑C** (`0483:5740` → `/dev/ttyACM*`), alongside `USART1`; both live at
      once, replies routed to the originating transport.
    - **Status on the integrated 0.96" TFT** — rate, exposure, pulse count, `pol`/`skew`, clock and
      link — so the rig is legible with no host attached.
    - **`make flash` needs no BOOT0 press**: the firmware reboots itself into the ROM bootloader
      (vector at `0x1FF09800`, established by measurement — `0x1FF00000` reads as zeros).
  - `tools/j106-sync-check.py` — **run on the live board**, and it is what produced the
    free-running baseline above (worst skew 2.43 ms, drift 8.33 µs/s).
  - **Trigger input characterised**: optocoupler, isolated (README §5 Stage 8). The level-shifting
    risk is gone; the circuit is 4 signal wires plus a common return, no external components.
  - ~~**Missing: the wiring itself**~~ — **done**: 8 conductors between the STM32H7
    (`PA0/PA1/PA2/PA3`, header P2-17..20) and the four modules' trigger pads. All four trigger.
  - **Deployed 2026‑08‑26**: `Image.j106trig` (`4.9.337-tegra #7`, patches 0001+0002+**0003**) is
    installed behind `LABEL j106trig` and is now `DEFAULT`, reusing the existing `j106imx296.dtb`
    (patch 0003 needs **no DT change**). `sha256` verified against the local build;
    `LABEL j106imx296` kept as the fallback and `extlinux.conf.bak-pre-trig` alongside it. Booted and
    verified: all four sensors probe (`2-0018 2-001a 7-0018 7-001a`), all four capture 10/10 frames
    free-running, and `/sys/module/imx296/parameters/trigger_mode` exists and is writable
    (currently `0`). So the kernel side is no longer on the critical path — enabling triggered mode
    is one `echo 1` once the wires are on.
  - ⚠ **If you wired anything from an earlier revision of `WIRING.md`, re-check it.** The trigger
    pins changed from `PE9/PE11/PE13/PE14` to `PA0`–`PA3`; the old pins now drive the display.

### ◑ Intermittent (hardware/Argus quirks, not config bugs)
- ~~**South cameras B/D (`0x12`) per‑boot enumeration lottery**~~ — **FIXED** by the boot‑time reset
  pulse + `j106_reset_recover` sysfs (Stage 1.5/1.6, see Working). Port D now 10/10 reboots. The south
  MIPI links (D, F) still log an occasional **non‑fatal** CIL `0x89` (D‑PHY) error at stream start —
  recovered, does not wedge on the deployed 680 Mbps kernel. B has no physical sensor.
- **Argus 5‑session start race** — Stage 6; use the restart‑daemon + retry workaround.

### ❌ Open / hardware‑limited
- **USB 3.0 on J2/J3 (2026‑09‑02)** — **Every software configuration is exhausted, including
  Auvidea's exact shipping config. The SS link does not train. The fault is physical, below the
  SoC.** Stock is wired for the devkit, so *both*
  J106 connectors are USB‑2‑only: on the J106 the SS lanes pair with the *other* USB2 ports —
  `usb3-0`↔`usb2-2` (**J3**, silkscreen "USB2") and `usb3-1`↔`usb2-1` (**J2**, silkscreen "USB1")
  — whereas stock points `usb3-0` at `usb2-1` and disables `usb3-1`. Fixed in
  [`override-usb.dtsi`](tx2-j106-6csi/override-usb.dtsi), applied **twice** (static padctl node
  **and** `/plugin-manager/fragment-500-xusb-config`, which rewrites the ports unconditionally for
  this SKU and silently undoes a static‑only patch).

  Confirmed independently from Auvidea's own **J120 4.2** package
  ([`bsp-r32.2/j120-v2.0/`](bsp-r32.2/j120-v2.0/)): a subtree diff of their shipped c03 DTB against
  stock shows **exactly three** padctl changes, plus the 5‑phy xhci list —

  ```
  usb2-2:  + nvidia,oc-pin = <0x01>
  usb3-0:    companion 1 → 2        (J3)
  usb3-1:    disabled → okay        (J2)
  ```

  and their `J120_rev9_second_USB.txt` encodes the same pairing in the legacy binding by phy
  **order** (`utmi-1, usb3-1` then `utmi-2, usb3-0`).

  **ODMDATA: leave at stock `0x1090000`.** Auvidea's release notes give the **J10x family (our
  J106)** *"1x USB 3.0, config 2 (default port muxing)"*; *config 6* is the 2× USB3 arrangement for
  J121/J142/J143. Their shipped `p2771-0000.conf.common` carries `ODMDATA=0x1090000`, i.e. stock.
  An earlier attempt here flashed `0x3090000` (from the v2.2 J90 BSP) — it does move UPHY lane 1
  from PCIe to XUSB (verified: `odm-data` loses `enable-pcie-on-uphy-lane1`, gains
  `enable-xusb-on-uphy-lane1`) but it is the **wrong carrier's muxing** and did not help. Revert with
  `sudo ./flash.sh -o 0x1090000 -r -k BCT jetson-tx2 mmcblk0p1` from RCM (BCT‑only; `flash.sh` skips
  `build_fsimg` when `-k` is given, so rootfs/kernel/DTB partitions are untouched — verified, board
  cold‑booted first time).

  **Evidence that the SoC side is fine** (padctl dynamic debug, 180 callsites, xhci re‑bound):

  ```
  phy init USB3 0 ; USB3 port 0 companion USB2 port 2 mode 1 ; phy power on USB3 0
  phy init USB3 1 ; USB3 port 1 companion USB2 port 1 mode 1 ; phy power on USB3 1
  ```

  Both SS phys initialise and power on with the configured companions, and there is **no** error,
  warning, timeout or PLL failure anywhere in padctl / xhci / dmesg / journalctl / UART0.

  **Evidence that nothing is on the far end** (xHCI PORTSC via `/dev/mem`, device plugged in):

  | port | proto | PP | PLS | CCS |
  |---|---|---|---|---|
  | 1–3 | USB3.0 | 1 | `RxDetect` | 0 |
  | 6 | USB2.0 | 1 | `U0` High | 1 |

  `RxDetect` + `CCS=0` with a device attached = **no far‑end receiver termination sensed**. Training
  never starts, so this is not a signal‑integrity or link‑training problem.

  **The decisive test.** With **stock `ODMDATA=0x1090000` + this DTB** — i.e. byte‑for‑byte what
  Auvidea ships for the J10x — the drive on J3 still enumerates at **480 Mb/s**. The revert
  demonstrably took effect in hardware: xHCI SS port 3 flips from `RxDetect` to `Disabled`
  (one XUSB lane instead of two), and `/chosen/plugin-manager/odm-data/` shows
  `enable-pcie-on-uphy-lane1` restored with `enable-xusb-on-uphy-lane1` gone. Full matrix, all
  with both J2 and J3, a drive **and** a self‑powered USB3 hub:

  | config | result |
  |---|---|
  | stock DTB + stock ODMDATA | 480 |
  | Auvidea DTB + `0x3090000` | 480 |
  | Auvidea DTB + `oc-pin` + `0x3090000` | 480 |
  | + PCIe controller disabled | 480 |
  | **Auvidea DTB + stock `0x1090000`** (their exact config) | **480** |

  **Ruled out by test, not by reasoning:** DT config and companion mapping; the padctl driver;
  power (a self‑powered USB3 hub that shows its `2109:0817` USB3 function on a PC shows only
  `2109:2817` USB2 here); the drive; and **PCIe lane contention** — PCIe holds 4+1 UPHY lanes from
  ~1.02 s until "no end points detected" at ~3.69 s, but a DTB with the PCIe controller fully
  disabled changed nothing.

  **Leading remaining suspect — cable role.** J2/J3 are micro‑USB3 **sockets used as host ports**.
  A normal micro‑B cable is wired for the *peripheral* end, so using one inverted puts TX against TX
  on the SS pairs while USB2 (no role crossover) keeps working perfectly — exactly the signature
  measured. A cable verified at USB3 against a RealSense T265 does **not** verify this orientation.
  The tech ref calls for a **DeLOCK 83469** micro‑B → A‑female **OTG** adapter; try that first.
  If it and the ODMDATA revert both fail, conclude the carrier's SS traces don't work with a TX2.


- **USB bus power is 900 mA shared across J2+J3** (`USB1_EN_OC`, tech ref) — an NVMe enclosure
  (Corsair MP510 + RTL9210) browns out on it: enumerates, then `Read Capacity(10) failed`
  (Sense Key 0x5 / ASC 0x24), `/dev/sda` = 0 bytes, self‑disconnects after 30–200 s. A **self‑powered
  hub** fixes it completely (full 1.9 TB reads, stable); a low‑power 256 GB SATA M.2 in the same
  enclosure works direct. Not a config bug — budget for external power or pick a low‑draw drive.

- **Storage: prefer the M110 SATA M.2 slot (J4) over USB.** SATA is on UPHY lane 5, untouched by the
  ODMDATA change, and probes clean today — the enable GPIO is on the module's own `spmic@3c`
  (`maxim,max77620`), **not** the absent carrier expander, so none of the `-517` grief that hit
  USB/fan/HDMI applies:

  ```
  tegra-ahci 3507000.ahci-sata: AHCI 0001.0301 32 slots 2 ports 3 Gbps
  ata1: SATA link down (SStatus 0 SControl 300)      # configured, waiting for a drive
  vdd-pex-1v00 / dvdd-pex: enabled
  ```

  Fit an **M.2 2242, B‑key, SATA** card (*not* NVMe; M3 flat‑head screw). ~270 MB/s at SATA II vs
  ~35 MB/s on the USB2 path — enough for 4× IMX296 mono8 1456×1088 @ 30 fps (~190 MB/s), which USB
  cannot do at any of the rates measured here.

- ~~**IMX296 ISP colour tuning**~~ — **SOLVED 2026‑08‑25.** A public IMX296 tuning exists after all,
  inside INNO‑MAKER's Jetson Orin binary package
  (`github.com/INNO-MAKER/cam-imx296raw-trigger`, `1-1jetson_orin_nano_driver/5.15.148/…tar.gz`,
  path `isp/camera_overrides.isp`). It is *not* in any repo tree — only inside that tarball, which is
  why a file search misses it. Vendored here as
  [`tools/nvcam-settings/camera_overrides.imx296.isp`](tools/nvcam-settings/camera_overrides.imx296.isp)
  and installed by default by `deploy-j106.sh` (`ISP_FILE=` to override).

  Measured on a neutral scene, port F, identical exposure settings:

  | Tuning | R | G | B | G/R | G/B | max cast |
  |---|---|---|---|---|---|---|
  | Arducam **IMX219** (was) | 43.4 | 85.3 | 55.2 | 1.97 | 1.55 | **97 %** |
  | INNO‑MAKER **IMX296** (now) | 50.0 | 44.0 | 41.4 | 0.88 | 1.06 | **12 %** |

  Measured on **all four ports** with the IMX296 tuning (AE clamp gain 1–8 / dgain 1–2):

  | Port | R | G | B | G/R | G/B | imbalance |
  |---|---|---|---|---|---|---|
  | C `2-001a` | 53.9 | 46.2 | 42.4 | 0.86 | 1.09 | 27 % |
  | D `2-0018` | 69.9 | 61.2 | 56.7 | 0.88 | 1.08 | 23 % |
  | E `7-001a` | 84.7 | 70.8 | 65.9 | 0.84 | 1.07 | 28 % |
  | F `7-0018` | 79.1 | 67.4 | 64.3 | 0.85 | 1.05 | 23 % |

  The residual warm cast is **uniform across all four cameras** (G/R 0.84–0.88, G/B 1.05–1.09), so it
  is a property of the tuning rather than per-module variation — meaning **one** correction serves all
  four. Brightness-preserving gains from the four-port mean (R 71.9, G 61.4, B 57.3): **R ×0.884, G ×1.035, B ×1.108**.

  The old numbers are almost exactly a raw Bayer sensor's native channel ratios — AWB was applying
  unity gains and never converging, because the `awb` gray‑line / CCT constants
  (`awb.GrayLineSlope`, `awb.UtoCCT`, …) were IMX219 colour‑response measurements. That is also why
  `wbmode` had no effect. The IMX296 file carries its own AWB constants and the correct
  `opticalBlack` bias of **60** (the IMX296's `BLKLEVEL`; the IMX219 file says 64).

  **Can the Raspberry Pi tuning be reused directly?** Partly — and it is already the ultimate source.
  Pi's libcamera IMX296 tuning
  (`raspberrypi/libcamera:src/ipa/rpi/{vc4,pisp}/data/imx296.json`) is a real Sony/RPi calibration
  containing **7 CCMs across 2500–7400 K**, a 7-point AWB ct-curve, black level 3840 (= 60 << 6,
  matching the datasheet's `BLKLEVEL`), a noise model and ALSC tables. But it does not transplant
  wholesale:
  - **NVIDIA's format holds exactly ONE CCM** (`colorCorrection.srgbMatrix[0..2]`) with no
    colour-temperature indexing, so 6 of Pi's 7 matrices have nowhere to go.
  - **The vendor CCM is provably Pi's 5600 K matrix damped 50 % toward identity** — verified
    numerically to 3 decimal places on all 9 coefficients. **Tested the full-strength Pi matrix:
    it is worse** (channel imbalance 21 % → 45 %, strong magenta cast, over-saturated blues),
    because a CCM is applied *after* white balance and amplifies the residual AWB error. The
    damping is a deliberate, correct compromise — kept.
  - **ALSC / lens shading cannot transfer at all**: Pi's tables are calibrated for the Pi GS
    camera's lens, while the J106 modules carry a fisheye. This is why the vendor stripped the
    lens-shading tables, and why residual vignetting remains.
  - Pi's AWB ct-curve *could* in principle be remapped onto NVIDIA's gray-line/CCT constants
    (`awb.UtoCCT`, `awb.GrayLineSlope`, `awb.v4.FusionLights`), which is where the remaining ~12–21 %
    imbalance lives — that is the actual remaining work, and it needs a colour target to validate.

  ⚠️ Caveats worth knowing: the file's own header calls it *"experimental candidate v0.2c … **not
  production tuning**"* — it is derived from RidgeRun's **IMX477** tuning with the CCM mapped from
  Raspberry Pi's IMX296 libcamera JSON (daylight, 5600 K) and the IMX477 lens‑shading tables
  removed. So expect residual vignetting and a slight warm tint; it is a large improvement, not a
  calibration. And `camera_overrides.isp` is **global — one tuning for all sensors** — so a mixed
  IMX219 + IMX296 board cannot have both correct at once.
- **Path to a genuinely natural image (what is actually left)** — current state is a ~21 % channel
  imbalance (slightly warm), visible vignetting, and uncorrected fisheye distortion. In priority order:
  1. **White balance (biggest win).** The residual cast lives in the `awb` gray-line / CCT constants.
     **Tried and does NOT work:** `ae.PerChannelGainAdjustment` (measured trim applied, image
     unchanged — the knob appears inert in this ISP build) and raising `ae.MeanAlg.*Target` 80→110
     (no brightening, because AE is already pinned at the clamp ceiling). The reliable route is a
     **grey-card measurement + per-channel correction applied downstream** of Argus, or commissioning
     a real tuning. Raw V4L2 bypasses all of this and is already correct.
  2. **Vignetting.** The IMX296 file has lens-shading *parameters but no tables* (24 lines vs the
     IMX219 file's 1216) — the vendor stripped IMX477 tables and never replaced them. Fixing this is
     a straightforward **flat-field calibration through the actual fisheye lens**: shoot a uniformly
     lit white surface, compute per-channel radial falloff, generate `lensShading.*` tables. Pi's ALSC
     tables cannot be reused (different lens).
  3. **Fisheye distortion.** Not an ISP job at all — needs OpenCV fisheye calibration plus a dewarp
     (VPI/CUDA) downstream. Required for the BEV/VIO rig regardless, so the intrinsics are dual-use.
  4. **Validation.** Everything above is subjective without a **24-patch colour target**; with one,
     CCM and AWB can be optimised numerically instead of by eye.

- **IMX296 Argus auto-exposure is unusable unmarshalled** — left alone, AE pins **both** gain
  (479/480 = 47.9 dB) and exposure (SHS1=0, full-frame 33 ms) to maximum and still exposes for the
  highlights, burying the image in chroma noise. This is the AE half of the same wrong tuning.
  Clamp it: `exposuretimerange="8000000 16500000" gainrange="1 4" ispdigitalgainrange="1 1"` gives a
  clean, sharp image (defaults in `tools/grid-stream-host.sh`, overridable via `ARGUS_EXP` /
  `ARGUS_GAIN` / `ARGUS_DGAIN`).
- **IMX296 lens descriptor is a placeholder** — `module5`'s `drivernode1` still points at
  `j106_lens_imx219@J106` because the IMX296 module's optics have not been characterised. Replace with
  a dedicated `j106_lens_imx296` node once focal length / f-number are measured (affects Argus metadata
  only, not capture).
- **Micro‑USB Linux device‑mode gadget (`/dev/ttyACM0` / `192.168.55.1`) — will NOT work on M110 J17.** This is
  USB0/OTG (J106 → M110 **J17**), *separate* from UART0 (and *separate* from recovery, which **does** work —
  see Working). Per the M110 PDF, J17 is a **host‑leaning port**: `USB0_ID` floats → host, and `USB0_VBUS` is
  tied to the M110's own 5 V output. Forcing `usb2-0 mode="device"` binds the L4T gadget
  (`acm.GS0`+`rndis/ncm`+`mass_storage` on UDC `3550000.xudc`, `/dev/ttyGS0` present) but it **never goes
  online**: dmesg `tegra-xudc 3550000.xudc: vbus state: 0` + `extcon@1: USB_HOST=1` (host VBUS not sensed) →
  xudc stays in ELPG, host sees nothing. Confirmed at register level — **hardware wiring, not a software bug**
  (forcing pure‑device by stripping xudc's `extcon`/`otg-controller` breaks the padctl with `-517`). So we **leave `usb2-0` as stock `mode="otg"`** (forcing device gave no
  benefit and would block J17 from acting as a USB host). It would
  enumerate on a devkit‑wired micro‑USB (e.g. **XCB‑Lite**). *Recovery/flashing over the same J17 works fine
  (Working) — that is what the port is for.*

### Carrier buttons — verified map (live per‑button test, 2026‑06‑14)
| Button | Wiring (measured) | Behaviour |
|---|---|---|
| **J106 power** | Tegra `gpio-312` → `KEY_POWER` (+ module `POWER_BTN`) | ✅ powers the board on; shuts down a running board (`logind HandlePowerKey=poweroff`) |
| **M110 power** | PMIC `POWER_BTN` (B50) — **not on any GPIO**; all‑GPIO scan saw nothing on press, and it never powers on | ❌ electrically dead on this stack — **hardware** (not remappable). Use the J106 power button. |
| **M110 recovery** | Tegra `gpio-313` → `KEY_VOLUMEUP` (a plain GPIO — **not** the bootrom `FORCE_RECOVERY` strap) | by itself a no‑op; with [`tools/j106-recovery-key`](tools/) a **≥1.5 s hold** runs `reboot forced-recovery` → **RCM** (verified) |
| **Reset** (J106 / M110) | `SYS_RESET_N` | ✅ hardware reset (LED blinks) |

`reboot forced-recovery` is supported by the t18x kernel (`reboot-t18x.c`). Because the M110 "Recovery" button
is wired to a GPIO (not `FORCE_RECOVERY`), it cannot enter recovery on its own — the `tools/j106-recovery-key`
systemd service bridges it (hold ≥1.5 s → software RCM). Tap **Reset** (no hold) to leave recovery.

### Notes
- Detailed chronological bring‑up investigation (every dead‑end, symptom, and the reasoning that led to
  each fix above) is preserved in the git history of this file.
- **Cross‑checked against the working TX1 reference** (2026‑06‑14): the camera driver patch `0001` mirrors
  Auvidea's TX1 R24.2.1 imx219 patch exactly (comment out `gpio_set_value(reset,0)`; release the shared
  reset high once) and the 680 Mbps PLL matches Auvidea's TX1 values; the micro‑USB fix mirrors TX1's
  carrier‑independent OTG approach; buttons confirmed devkit‑wired via the XCB‑Lite reference.
