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

## j106-trigctl — drive the IMX296 external-trigger generator

**Runs wherever the serial link is** — on the board if the STM32H7 is wired to M110 `J22`
(a `/dev/ttyTHS*`), on the build host if it is on a USB-serial dongle (`/dev/ttyUSB*`).
It never touches the cameras.

The generator owns both the frame rate **and the exposure**: in the IMX296's Fast Trigger mode the
asserted pulse width *is* the exposure time, so the sensor's own exposure control does nothing while
triggered. The camera modules' trigger inputs are optocouplers, so two of the commands (`pol`,
`skew`) exist to correct for what the isolation barrier hides — see `hw-trigger/WIRING.md` §3.

```bash
./j106-trigctl.py --port /dev/ttyUSB0 status
./j106-trigctl.py fps 30 --exposure 5000     # 30 fps, 5 ms exposure
./j106-trigctl.py raw 'exp 2 3000'           # camera 2 only, 3 ms
./j106-trigctl.py raw 'pol 0'                # invert, if no frames arrive
./j106-trigctl.py raw 'skew 8000'            # remove 8 us of optocoupler lag
./j106-trigctl.py start
./j106-trigctl.py burst 300                  # 300 pulses, then stop
./j106-trigctl.py stop
```

Firmware and wiring: [`../hw-trigger/`](../hw-trigger/WIRING.md).

## j106-sync-check — measure inter-camera frame synchronisation

> **RUN THIS ON THE BOARD.** It opens `/dev/video*` directly.

The acceptance test for the hardware trigger. Streams every camera at once and compares
V4L2 buffer timestamps, judging on two independent criteria — the **constant offset**
between cameras, and the **drift rate** of that offset. Either alone can be fooled: a
short run hides drift between same-batch crystals (they can be only a few ppm apart),
while offset alone cannot distinguish a well-phased pair from a triggered one.

```bash
sudo systemctl stop nvargus-daemon      # the nodes must be free
./j106-sync-check.py -n 600             # ~20 s at 30 fps
```

Measured free-running baseline (4× IMX296, ports C–F, 2026-08-25):

```
worst skew 2430.0 us over 20.0 s; worst drift 8.33 us/s
verdict: NOT synchronised — free-running.
```

— i.e. the cameras sit up to **2.4 ms apart**, at a phase that is **re-randomised on every
stream start**, drifting a further 3–8 µs/s. That is the number the hardware trigger has to
beat.
