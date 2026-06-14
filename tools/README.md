# tools/ — board-side helpers

## j106-recovery-key — make the M110 "Recovery" button enter recovery mode

The M110 "Recovery" button is wired to a plain Tegra GPIO (gpio-313, which the stock
devkit DT maps to `KEY_VOLUMEUP`), **not** the bootrom `FORCE_RECOVERY` strap — so out
of the box it just emits a (no-op) volume-up key and never enters recovery. This tiny
service watches the `gpio-keys` input device and, on a **≥1.5 s hold** of that button,
runs `reboot forced-recovery`, putting the board into **USB recovery mode (RCM)** for
flashing over the micro-USB (M110 J17). Verified: the host then sees
`0955:7c18 NVIDIA Corp. T186 [TX2 Tegra Parker] recovery mode`.

Install on the board:
```bash
sudo cp j106-recovery-key.py /opt/j106-recovery-key.py
sudo cp j106-recovery-key.service /etc/systemd/system/j106-recovery-key.service
sudo systemctl daemon-reload
sudo systemctl enable --now j106-recovery-key.service
```
To boot back out of recovery, tap **Reset** (without holding recovery).
