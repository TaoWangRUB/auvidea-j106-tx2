# nvcam-settings/ — Argus ISP override for IMX219 on TX2

`camera_overrides.isp` is the community **Arducam** IMX219 ISP tuning
(source: https://www.arducam.com/downloads/Jetson/Camera_overrides.tar.gz,
see https://docs.arducam.com/Nvidia-Jetson-Camera/Application-note/Fix-Red-Tint-with-ISP-Tuning/).

Stock L4T ships **no** IMX219 ISP tuning for the TX2 (tegra186) ISP — IMX219 is a reference
sensor only on tegra210 (Nano/TX1) — so Argus falls back to generic defaults and the images are
washed‑out / magenta‑tinted. This file is loaded by libargus from `/var/nvidia/nvcam/settings/`
and corrects the colour matrix + black level. **Verified on this TX2/tegra186 board** (before/after:
`captures/isp_override_compare.jpg`; R/G/B 101/85/95 → 89/89/81, contrast std 10.7 → 52.9).

Install:
```bash
sudo cp camera_overrides.isp /var/nvidia/nvcam/settings/camera_overrides.isp
sudo chmod 664 /var/nvidia/nvcam/settings/camera_overrides.isp
sudo chown root:root /var/nvidia/nvcam/settings/camera_overrides.isp
sudo rm -f /var/nvidia/nvcam/settings/nvcam_cache_*.bin
sudo systemctl restart nvargus-daemon
```
Non‑fatal `em.preset[*].wbgains` parse warnings appear (attributes from a different L4T version,
ignored); the core tuning still applies. Remove the file + restart the daemon to revert.
