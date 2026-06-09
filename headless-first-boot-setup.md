# Headless first-boot (oem-config) on TX2 — over micro-USB, no monitor

How to take a **freshly-flashed** Jetson TX2 (L4T R32.7.6) through NVIDIA's first-boot
**`nv-oem-config`** wizard **with no display and no keyboard** — using only serial links.
Verified end-to-end **2026-06-09** with the module on the **XCB-Lite** carrier. The procedure
is **carrier-independent** (it uses only the micro-USB device-mode gadget + the debug UART), so
use the same steps when you first bring the module up on any carrier.

> **Carrier note:** this setup was done on **XCB-Lite**. The module does **not** currently work
> on the **Auvidea J106** carrier — on J106 the stock devkit DTB leaves the carrier peripherals
> (cameras, USB, etc.) non-functional until you flash the carrier DTB; see [README §5](README.md).
> The first-boot below is a prerequisite either way (you need a login before any DTB work).

> TL;DR: The first-boot wizard runs **only** on the micro-USB **device-mode** port
> (`/dev/ttyACMx`), as an ncurses `dialog` UI — **not** on the debug UART. Do **not** waste
> time trying to bypass it from the U-Boot/kernel command line; on R32 the first-boot runs
> inside a self-contained initramfs that ignores `init=`, `systemd.unit=`, `rdinit=`, etc.
> Drive the wizard over `/dev/ttyACM0`; afterward the board is at **`ssh nvidia@192.168.55.1`**.

---

## 0. The three connections (host → TX2)

| Link | TX2 port | Host device | Purpose |
|------|----------|-------------|---------|
| **USB-TTL** (CH340 etc.) | debug UART (`ttyS0`) | `/dev/ttyUSB0` @ **115200 8N1** | Watch boot, drive U-Boot, SysRq reboot |
| **micro-USB** | device-mode/recovery port | `/dev/ttyACM0` + `192.168.55.x` net | **Run the first-boot wizard**, then SSH |
| **Ethernet** | carrier RJ45 | — | LAN/SSH after setup (had no carrier during this run) |

Enumeration sanity-check on the host:
```bash
ls /dev/ttyUSB* /dev/ttyACM*          # CH340 -> ttyUSB0 ; L4T gadget -> ttyACM0
lsusb | grep -iE '1a86:7523|0955:7020'# CH340 serial ; NVIDIA L4T (Tegra) gadget
ip -brief addr | grep 192.168.55      # host gets 192.168.55.100/24, TX2 is .1
```
Common gotchas:
- **micro-USB "connected but not detected"** is almost always a **charge-only cable** or a
  hub — use a known **data** micro-USB cable straight into the host. The TX2 side is fine if
  the boot log shows `using random self/host ethernet address` (the CDC gadget coming up).
- The debug-UART baud is **115200** (not 921600). Wrong baud = solid garbage at every rate.

---

## 1. Debug UART (`/dev/ttyUSB0`) — what it gives you

The whole boot chain **MB2 → CBoot → U-Boot 2020.04 → kernel** is on this line, so you can:

- **Read the boot log** (confirm L4T version, board = `P2771-0000-500` stock devkit DTB).
- **Interrupt U-Boot**: spam a key during the ~2 s `Hit any key to stop autoboot` window to
  land at the `Tegra186 (P2771-0000-500) #` prompt (host→TX2 TX wire must be connected).
- **Reboot the board over serial with SysRq** (no physical access), via a serial BREAK:
  ```python
  import os, time, termios
  fd = os.open('/dev/ttyUSB0', os.O_RDWR | os.O_NOCTTY)
  for ch in ['s','b']:                       # SysRq: Sync, reBoot
      termios.tcsendbreak(fd, 0); time.sleep(0.25)
      os.write(fd, ch.encode()); time.sleep(0.6)
  ```

### What does NOT work (don't bother)
The first-boot **cannot** be bypassed from the kernel command line. All of these were tried
and produce an identical boot straight to the oem-config message, because the first-boot runs
in a self-contained initramfs that never pivots to the rootfs:
`init=/bin/bash`, `systemd.unit=emergency.target` + `SYSTEMD_SULOGIN_FORCE=1`,
`rdinit=/bin/sh` + `rd.break`, `rd.systemd.unit=emergency.target`.
The boot log states the intent plainly:
> *Please complete system configuration setup on the serial port provided by Jetson's USB
> device mode connection. e.g. /dev/ttyACMx …*

So: **use the micro-USB.**

---

## 2. Drive the wizard on `/dev/ttyACM0`

Easiest for a human is an interactive terminal:
```bash
screen /dev/ttyACM0 115200     # or: minicom -D /dev/ttyACM0 -b 115200
```
It's an ncurses `dialog` TUI. Key handling quirks if you script it over raw serial:
- **Ctrl-L** (`\x0c`) forces a redraw (useful to re-read the current screen).
- The license is a **scroll textbox** — `Enter` alone does nothing; **`Tab` then `Enter`**
  activates `<Ok>`. PgDn/arrows scroll.
- Yes/No dialogs default to `<No>`; send **Left-arrow** (`\x1b[D`) then `Enter` to pick
  `<Yes>` (needed for the *weak password* and *continue without default route* prompts).

### Answers used for this board (defaults are fine / changeable later)

| Screen | Value |
|--------|-------|
| NVIDIA license | Accept (`Tab`→`Enter`) |
| Language | English |
| Location / Time zone | United States / Eastern, system clock = **UTC** |
| Full name / **Username** | nvidia / **nvidia** |
| **Password** (+ confirm) | **nvidia** → weak-password warning → **Yes** |
| Network — primary iface | `eth0` → "no default route" → **Yes (continue)** |
| **Hostname** | **jetson-tx2** |
| Nvpmodel (power mode) | **MAXN** (max performance) |

Then it runs **"Configuring hardware…"** for a few minutes and finishes. Change any default
later: `sudo timedatectl set-timezone …`, `sudo dpkg-reconfigure locales`,
`sudo nvpmodel -m <id>`.

---

## 3. Confirm it worked

When oem-config finishes the board reaches `multi-user.target` and SSH comes up on the gadget
net. The host has no `sshpass`, so use the `SSH_ASKPASS` trick for a non-interactive login:

```bash
printf '#!/bin/sh\necho nvidia\n' > /tmp/ap.sh && chmod +x /tmp/ap.sh
DISPLAY=:0 SSH_ASKPASS=/tmp/ap.sh SSH_ASKPASS_REQUIRE=force setsid -w \
  ssh -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null \
      -o PreferredAuthentications=password -o PubkeyAuthentication=no \
      nvidia@192.168.55.1 'whoami; hostname; lsb_release -ds; head -1 /etc/nv_tegra_release'
rm -f /tmp/ap.sh
```
Expected:
```
nvidia
jetson-tx2
Ubuntu 18.04.6 LTS
# R32 (release), REVISION: 7.6, ... BOARD: t186ref, EABI: aarch64, DATE: Tue Nov  5 ... 2024
```
`sudo` works (`echo nvidia | sudo -S whoami` → `root`). You can now `sudo poweroff` cleanly.

---

## 4. After first boot

- **Primary admin link** is the micro-USB net (`192.168.55.1`) — always available regardless of
  carrier, so it's the reliable way in during any bring-up.
- **Ethernet** (`eth0`) had **no carrier** during this run; if you want LAN SSH, check the
  cable/switch. Not a fault introduced by setup.
- **Moving the module to J106:** the stock DTB does not drive the J106 carrier, so cameras/USB
  (and more) are dead until the **carrier DTB** is built and flashed — see [README §5](README.md).
  J106 bring-up is **not working yet**. First-boot (above) only needs to be done once per flash.

---

## 5. Notes / gotchas

- The board reports `P2771-0000-500` / `model = quill` in U-Boot — that's the **stock NVIDIA
  devkit DTB**, expected before the carrier DTB is flashed (matches README §3b).
- The `pca953x … -121` and `ina3221 … 0xffffff87` errors in the boot log are the devkit-only
  I²C devices absent on non-devkit carriers (XCB-Lite, J106) — **not** related to first-boot;
  harmless here.
- A solid stream of high-bit garbage on the UART = wrong baud or a 5V/3.3V level mismatch on
  the TTL adapter; the console is **115200** and TX2 UART is **3.3V**.
- If the micro-USB gadget never enumerates, you cannot complete setup over serial alone — the
  fallback is to **re-flash with a pre-seeded user** (NVIDIA flash tooling sets user/password
  at flash time, skipping oem-config) in recovery mode.
