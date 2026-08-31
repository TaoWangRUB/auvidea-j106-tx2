# `hw-trigger/scripts` — operating the 4× IMX296 trigger rig

One script per job. **[`../WIRING.md` §6](../WIRING.md) is the reference**; this is the index.

Each script says where it runs, because that is the easiest thing to get wrong.

| Script | Runs on | Job |
|---|---|---|
| `trig-send.sh` | TX2 | drive the STM32: `fps`, `exp`, `pol`, `skew`, `start`/`stop`, `status` |
| `trig-recv.sh` | TX2 | count frames per camera over a fixed window |
| `verify-trigger.sh` | TX2 | acceptance test: free-run control → triggered → polarity sweep |
| `trig-probe.sh` | TX2 | 1 Hz / 500 ms so a multimeter can find where a pulse stops |
| `sender.sh` | TX2 | stream 4 cameras as H.264/RTP/UDP to the host |
| `receiver.sh` | **host** | 2×2 grid, labelled by port |
| `net-tune.sh` | **both** | size the UDP socket buffers for I-frame bursts |

## The short version

```bash
# TX2 — trigger on, wide enough exposure to see anything in a dim room
./trig-send.sh pol 0 && ./trig-send.sh fps 30 && ./trig-send.sh exp 30000 && ./trig-send.sh start
echo 1 | sudo tee /sys/module/imx296/parameters/trigger_mode

# TX2 — prove it
./verify-trigger.sh 30 5
for v in 0 1 2 3; do v4l2-ctl -d /dev/video$v --set-ctrl bypass_mode=0; done
~/j106-sync-check.py -n 200          # expect 30.00 fps, ~1.0 us, SYNCHRONISED

# TX2 + host — look at it
for v in 0 1 2 3; do v4l2-ctl -d /dev/video$v --set-ctrl bypass_mode=1; done
./sender.sh 10.42.0.1                # TX2
./receiver.sh                        # host
```

## Five things that will cost you an hour each

1. **`bypass_mode` is exclusive.** `1` = Argus/ISP (the grid), `0` = raw V4L2 (the sync check).
   Raw capture with `bypass_mode=1` returns **zero bytes with no error** — indistinguishable from a
   dead camera. The grid and the sync check can never run together.

2. **Never judge sync from the live grid.** Each camera passes a 100 ms `rtpjitterbuffer` — a
   transport error ~50,000× the 1 µs being measured. Sync comes from V4L2 timestamps, on the board.

3. **Backgrounding `receiver.sh` with `< /dev/null` kills it.** `gst-launch-1.0 -e` reads EOF on
   stdin as a shutdown request and sends EOS ~2 ms in. Use `< /dev/zero`.

4. **Never send `dfu` unless the USB-C cable is plugged into a host.** It reboots into a *USB*
   bootloader; with the cable out the board leaves the application, presents nothing on USB, stops
   answering the UART, and needs hands on it.

5. **A dark image is not a trigger failure.** Exposure *is* the XTRIG pulse width, so the 5 ms
   default is ~6× darker than free-running at 30 fps. Set `exp 30000` before judging anything.

## Expected good result

```
video0..3   200 frames   0 dropped   33330.1 us   (30.00 fps)
worst skew 1.0 us over 6.6 s;  worst drift 0.00 us/s
verdict: SYNCHRONISED
```
