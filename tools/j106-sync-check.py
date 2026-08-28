#!/usr/bin/env python3
"""j106-sync-check.py — measure how well the IMX296 cameras are synchronised.

Streams every populated /dev/video* at once and compares the kernel's V4L2
buffer timestamps across cameras. This is the acceptance test for the hardware
trigger: run it free-running, run it triggered, compare.

  free-running  each sensor runs off its own oscillator, so the skew between
                cameras DRIFTS steadily - that drift is the whole problem.
  triggered     every sensor exposes on the same edge, so the skew is small and
                stays put.

  >>> RUN THIS ON THE BOARD. <<<   It opens /dev/video* directly.

  ./j106-sync-check.py                    # 200 frames from every camera found
  ./j106-sync-check.py -n 600             # longer run shows drift more clearly
  ./j106-sync-check.py -d /dev/video0 -d /dev/video1

Raw V4L2 capture on this platform needs bypass_mode=0 and nothing else holding
the node - the script sets the control and tells you if the node is busy.

Wiring and bring-up order: hw-trigger/WIRING.md
"""
import argparse
import fcntl
import glob
import mmap
import os
import statistics
import struct
import subprocess
import sys
import threading

# ---- ioctl plumbing -------------------------------------------------------
_IOC_WRITE, _IOC_READ = 1, 2


def _IOC(d, t, nr, size):
    return (d << 30) | (size << 16) | (ord(t) << 8) | nr


BUF_FMT = "<IIIII4xqqIIBBBB4sIIQIII4x"
BUF_SIZE = struct.calcsize(BUF_FMT)                       # 88 on arm64
REQBUFS_FMT = "<III8x"
REQBUFS_SIZE = struct.calcsize(REQBUFS_FMT)               # 20

VIDIOC_REQBUFS = _IOC(_IOC_READ | _IOC_WRITE, "V", 8, REQBUFS_SIZE)
VIDIOC_QUERYBUF = _IOC(_IOC_READ | _IOC_WRITE, "V", 9, BUF_SIZE)
VIDIOC_QBUF = _IOC(_IOC_READ | _IOC_WRITE, "V", 15, BUF_SIZE)
VIDIOC_DQBUF = _IOC(_IOC_READ | _IOC_WRITE, "V", 17, BUF_SIZE)
VIDIOC_STREAMON = _IOC(_IOC_WRITE, "V", 18, 4)
VIDIOC_STREAMOFF = _IOC(_IOC_WRITE, "V", 19, 4)

BUF_TYPE_VIDEO_CAPTURE = 1
MEMORY_MMAP = 1
NBUFS = 4


def pack_buf(index=0, memory=MEMORY_MMAP):
    return struct.pack(BUF_FMT, index, BUF_TYPE_VIDEO_CAPTURE, 0, 0, 0,
                       0, 0, 0, 0, 0, 0, 0, 0, b"\0" * 4, 0, memory, 0, 0, 0, 0)


def unpack_buf(raw):
    # Field order: index type bytesused flags field | tv_sec tv_usec |
    # timecode(type flags frames seconds minutes hours userbits) |
    # sequence memory m length reserved2 reserved
    f = struct.unpack(BUF_FMT, raw)
    return {"index": f[0], "flags": f[3], "tv_sec": f[5], "tv_usec": f[6],
            "sequence": f[14], "offset": f[16], "length": f[17]}


class Camera:
    def __init__(self, dev):
        self.dev = dev
        self.name = os.path.basename(dev)
        self.fd = None
        self.maps = []
        self.stamps = []        # seconds, float
        self.seqs = []
        self.error = None

    def open(self):
        subprocess.run(["v4l2-ctl", "-d", self.dev, "--set-ctrl", "bypass_mode=0"],
                       stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
                       check=False)
        try:
            self.fd = os.open(self.dev, os.O_RDWR)
        except OSError as exc:
            raise RuntimeError(
                f"{self.dev}: {exc}. If it is busy, stop the Argus daemon first:\n"
                f"    sudo systemctl stop nvargus-daemon") from exc

        fcntl.ioctl(self.fd, VIDIOC_REQBUFS,
                    struct.pack(REQBUFS_FMT, NBUFS, BUF_TYPE_VIDEO_CAPTURE,
                                MEMORY_MMAP))

        for i in range(NBUFS):
            raw = fcntl.ioctl(self.fd, VIDIOC_QUERYBUF, pack_buf(i))
            b = unpack_buf(raw)
            self.maps.append(mmap.mmap(self.fd, b["length"],
                                       mmap.MAP_SHARED,
                                       mmap.PROT_READ | mmap.PROT_WRITE,
                                       offset=b["offset"]))
            fcntl.ioctl(self.fd, VIDIOC_QBUF, pack_buf(i))

    def run(self, count):
        try:
            fcntl.ioctl(self.fd, VIDIOC_STREAMON,
                        struct.pack("<i", BUF_TYPE_VIDEO_CAPTURE))
            for _ in range(count):
                raw = fcntl.ioctl(self.fd, VIDIOC_DQBUF, pack_buf())
                b = unpack_buf(raw)
                self.stamps.append(b["tv_sec"] + b["tv_usec"] / 1e6)
                self.seqs.append(b["sequence"])
                fcntl.ioctl(self.fd, VIDIOC_QBUF, pack_buf(b["index"]))
            fcntl.ioctl(self.fd, VIDIOC_STREAMOFF,
                        struct.pack("<i", BUF_TYPE_VIDEO_CAPTURE))
        except Exception as exc:                      # noqa: BLE001
            self.error = exc

    def close(self):
        for m in self.maps:
            m.close()
        if self.fd is not None:
            os.close(self.fd)


def us(v):
    return f"{v * 1e6:9.1f} us"


def drift_rate(times, skews):
    """Least-squares slope of skew against time, in seconds per second.

    A regression over every frame rather than differencing the first and last
    windows: same-batch crystals sit only a few ppm apart, so the drift of two
    free-running sensors over a short run is small enough that an endpoint
    estimate is mostly noise.
    """
    n = len(times)
    if n < 3:
        return 0.0
    t0 = times[0]
    xs = [t - t0 for t in times]
    mx = sum(xs) / n
    my = sum(skews) / n
    denom = sum((x - mx) ** 2 for x in xs)
    if denom == 0:
        return 0.0
    return sum((x - mx) * (y - my) for x, y in zip(xs, skews)) / denom


def nearest_skew(ref, other):
    """Signed offset from each ref timestamp to the closest frame in `other`.

    Returns two PARALLEL lists (times, skews) - not just skews - because
    dropping an unmatched pair would otherwise silently misalign the caller's
    zip against ref.stamps and corrupt the drift regression.

    Matching by nearest time rather than by index deliberately: the four
    STREAMONs do not happen on the same edge, so cameras can be a whole frame
    apart in index while being perfectly synchronised in time.

    Pairs further apart than half a frame period are DROPPED, not reported.
    The two series rarely start and stop on the same frame, so at the ends the
    nearest available partner can be a whole period away - which is not a skew
    measurement, it is the absence of a partner. Left in, a single such pair
    reported a whole 33.3 ms period as "worst skew" and failed a rig that was
    in fact synchronised to 1 us.
    """
    if len(ref) < 3 or not other:
        return [], []
    intervals = sorted(b - a for a, b in zip(ref, ref[1:]))
    half_period = intervals[len(intervals) // 2] / 2.0
    times, skews = [], []
    j = 0
    for t in ref:
        while j + 1 < len(other) and abs(other[j + 1] - t) <= abs(other[j] - t):
            j += 1
        skew = other[j] - t
        if abs(skew) <= half_period:
            times.append(t)
            skews.append(skew)
    return times, skews


def main():
    ap = argparse.ArgumentParser(
        description="Measure inter-camera frame synchronisation.",
        epilog="hw-trigger/WIRING.md")
    ap.add_argument("-n", "--frames", type=int, default=200)
    ap.add_argument("-d", "--device", action="append", dest="devices",
                    help="repeatable; default is every /dev/video*")
    args = ap.parse_args()

    devices = args.devices or sorted(glob.glob("/dev/video*"))
    if len(devices) < 2:
        sys.exit("need at least two cameras to measure synchronisation; "
                 f"found {devices or 'none'}")

    cams = [Camera(d) for d in devices]
    opened = []
    try:
        for c in cams:
            try:
                c.open()
                opened.append(c)
            except RuntimeError as exc:
                print(f"skipping {c.dev}: {exc}", file=sys.stderr)

        if len(opened) < 2:
            sys.exit("fewer than two cameras could be opened")

        print(f"streaming {args.frames} frames from "
              f"{', '.join(c.name for c in opened)} ...")

        threads = [threading.Thread(target=c.run, args=(args.frames,))
                   for c in opened]
        for t in threads:
            t.start()
        for t in threads:
            t.join()
    finally:
        for c in opened:
            c.close()

    live = [c for c in opened if len(c.stamps) >= 10]
    for c in opened:
        if c.error:
            print(f"{c.name}: capture error: {c.error}", file=sys.stderr)
    if len(live) < 2:
        sys.exit("not enough cameras delivered frames")

    # ---- per camera -------------------------------------------------------
    print("\nper camera")
    print(f"  {'node':<10}{'frames':>8}{'dropped':>9}"
          f"{'mean interval':>16}{'jitter (sd)':>14}")
    for c in live:
        d = [b - a for a, b in zip(c.stamps, c.stamps[1:])]
        mean = statistics.mean(d)
        sd = statistics.pstdev(d) if len(d) > 1 else 0.0
        expected = c.seqs[-1] - c.seqs[0] + 1
        dropped = expected - len(c.seqs)
        print(f"  {c.name:<10}{len(c.stamps):>8}{dropped:>9}"
              f"{us(mean):>16}{us(sd):>14}   ({1/mean:5.2f} fps)")

    # ---- pairwise ---------------------------------------------------------
    ref = live[0]
    print(f"\nskew relative to {ref.name} (nearest-frame match)")
    print(f"  {'node':<10}{'median':>13}{'max |skew|':>14}{'drift':>14}")

    worst = 0.0
    worst_rate = 0.0
    for c in live[1:]:
        ts, sk = nearest_skew(ref.stamps, c.stamps)
        if not sk:
            print(f"  {c.name:<10}   no frame pairs within half a period")
            continue
        rate = drift_rate(ts, sk)
        mx = max(abs(s) for s in sk)
        worst = max(worst, mx)
        worst_rate = max(worst_rate, abs(rate))
        print(f"  {c.name:<10}{us(statistics.median(sk)):>13}"
              f"{us(mx):>14}{rate * 1e6:11.2f} us/s")

    # ---- verdict ----------------------------------------------------------
    #
    # Two independent tests, because either one alone can be fooled:
    #
    #   offset  Sensors sharing one trigger edge expose together, so the skew
    #           is only the VI/DMA completion spread - tens to hundreds of us.
    #           Free-running sensors have arbitrary phase and sit milliseconds
    #           apart, constantly.
    #   drift   Sensors sharing one clock source cannot drift apart at all.
    #           Free-running ones drift by the difference between their
    #           crystals - which for same-batch parts can be only a few ppm,
    #           so this test needs a tight threshold AND a long enough run to
    #           resolve it.  That is why the offset test carries the short runs.
    MAX_OFFSET = 1000e-6        # 1 ms
    MAX_RATE = 2e-6             # 2 us/s = 2 ppm
    MIN_SPAN = 5.0              # seconds needed to trust the drift number

    span = ref.stamps[-1] - ref.stamps[0]
    print(f"\nworst skew {worst * 1e6:.1f} us over {span:.1f} s; "
          f"worst drift {worst_rate * 1e6:.2f} us/s")

    bad = []
    if worst > MAX_OFFSET:
        bad.append(f"cameras sit {worst * 1e3:.1f} ms apart — a shared trigger "
                   f"edge cannot do that (expect < {MAX_OFFSET * 1e3:.0f} ms)")
    if worst_rate > MAX_RATE:
        if span >= MIN_SPAN:
            bad.append(f"skew is drifting at {worst_rate * 1e6:.2f} us/s — "
                       f"sensors on one clock cannot drift "
                       f"(expect < {MAX_RATE * 1e6:.0f} us/s)")
        else:
            print(f"note: run is only {span:.1f} s; drift needs "
                  f">= {MIN_SPAN:.0f} s to be conclusive. Use -n to lengthen it.")

    if bad:
        print("verdict: NOT synchronised — free-running.")
        for b in bad:
            print(f"         - {b}")
        print("         If the trigger is meant to be on, check:\n"
              "           cat /sys/module/imx296/parameters/trigger_mode   (want 1)\n"
              "           dmesg | grep 'imx296.*streaming'                 (want EXTERNAL TRIGGER)\n"
              "           ./j106-trigctl.py status                         (want running=1)")
        return 1

    if span < MIN_SPAN:
        print(f"verdict: consistent with synchronised, but only {span:.1f} s of "
              f"data — re-run with -n {int(args.frames * MIN_SPAN / max(span, 0.1))} "
              f"to confirm the drift.")
        return 0

    print("verdict: SYNCHRONISED — skew is bounded and not drifting.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
