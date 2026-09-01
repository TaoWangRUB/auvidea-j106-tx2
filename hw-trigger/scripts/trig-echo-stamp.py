#!/usr/bin/env python3
"""Timestamp the trigger echo on the TX2 — the direct half of the Delta measurement.

WIRING.md section 4.4 route A: one trigger channel is echoed back into a Tegra GPIO
(here gpio-389 / GPIO_PQ5_PI5, M110 J21 pin 8) so the *true* trigger edge can be
timestamped and compared with the camera's reported exposure midpoint. That turns Delta
from something a solver estimates into something measured.

  >>> RUN THIS ON THE BOARD. <<<  It opens /dev/gpiochip*, so it needs root.

    sudo ./trig-echo-stamp.py --seconds 60 --out echo.csv

MEASURE THE FALLING EDGE (the default). The F401 drives PA5 open-drain: it pulls the
line down hard and then *releases* it, so the rising edge is only as fast as a weak
internal pull-up charging the wire. Under `pol 0` the falling edge is also the start of
exposure, which is the instant worth having.

WHY IT TAKES ITS OWN CLOCK_MONOTONIC RATHER THAN THE KERNEL'S EVENT STAMP — and the
intuition points the wrong way here, so it is worth stating. The kernel stamp is the
better number in isolation: it is taken in the interrupt handler, before any scheduling
delay. But Delta is only ever used as a DIFFERENCE against the IMU, and the IMU node
stamps *after* waking on its own data-ready edge, carrying a wake-up latency w of tens of
microseconds. Stamping the echo the same way makes both carry w, and w then cancels in
the difference. Using the kernel stamp here would remove w from one side only and leave
it as a systematic error in Delta - measuring the echo more precisely, and Delta less
accurately.

Both are recorded anyway: `wake_ns` is the one to use, `kernel_ns` is kept so the
wake-up latency itself can be quantified (their difference IS w).

The GPIO plumbing is imported from j106-imu-read.py rather than duplicated - same
resolve_line(), same EdgeSource, same clock call, so the two paths cannot drift apart.
"""
import argparse
import importlib.util
import os
import select
import struct
import sys

HERE = os.path.dirname(os.path.abspath(__file__))


def load_imu_module():
    """Import j106-imu-read.py by path (the hyphens make it un-importable by name)."""
    for cand in (os.path.join(HERE, "j106-imu-read.py"),
                 "/home/nvidia/tools/j106-imu-read.py",
                 os.path.join(HERE, "..", "..", "tools", "j106-imu-read.py")):
        if os.path.exists(cand):
            spec = importlib.util.spec_from_file_location("j106imu", cand)
            mod = importlib.util.module_from_spec(spec)
            spec.loader.exec_module(mod)
            return mod, cand
    raise SystemExit("cannot find j106-imu-read.py — it owns the GPIO edge plumbing")


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--gpio", type=int, default=389,
                    help="global GPIO number of the echo line (default 389 = J21 pin 8)")
    ap.add_argument("--edge", default="falling", choices=("falling", "rising", "both"),
                    help="falling by default: the edge the open-drain pin drives hard")
    ap.add_argument("--seconds", type=float, default=60.0)
    ap.add_argument("--out", default="echo.csv")
    a = ap.parse_args()

    imu, path = load_imu_module()
    print("gpio plumbing from %s" % path)

    src = imu.EdgeSource(a.gpio, a.edge, label="trig-echo-stamp")
    print("gpio-%d -> %s offset %d (%s), waiting on %s edges"
          % (a.gpio, src.chip, src.offset, src.chip_label, a.edge))

    t0 = imu.now_ns()
    deadline = t0 + int(a.seconds * 1e9)
    poller = select.poll()
    poller.register(src.fd, select.POLLIN)

    n = 0
    prev = None
    intervals = []
    with open(a.out, "w") as fh:
        fh.write("# trigger echo edges, CLOCK_MONOTONIC\n")
        fh.write("# gpio=%d edge=%s chip=%s offset=%d\n"
                 % (a.gpio, a.edge, src.chip, src.offset))
        fh.write("# wake_ns = our stamp after waking (use this one; carries the same\n")
        fh.write("#           wake latency as the IMU path, so it cancels in Delta)\n")
        fh.write("# kernel_ns = the chardev's own stamp; kernel_ns - wake_ns IS that latency\n")
        fh.write("seq,wake_ns,kernel_ns\n")
        while imu.now_ns() < deadline:
            if not poller.poll(500):
                continue
            data = os.read(src.fd, imu.EVENT_SIZE)
            wake = imu.now_ns()
            kern, _id = struct.unpack("=QI", data[:12])
            fh.write("%d,%d,%d\n" % (n, wake, kern))
            if prev is not None:
                intervals.append((wake - prev) / 1e6)
            prev = wake
            n += 1

    if not n:
        raise SystemExit("no edges seen — is the wire connected and the generator running?")
    intervals.sort()
    mid = intervals[len(intervals) // 2] if intervals else float("nan")
    print("%d edges in %.1f s -> %.3f Hz" % (n, a.seconds, n / a.seconds))
    print("  median interval %.4f ms" % mid)
    if intervals:
        print("  spread p5..p95 %.4f .. %.4f ms"
              % (intervals[max(0, len(intervals) // 20)],
                 intervals[min(len(intervals) - 1, len(intervals) * 19 // 20)]))
    print("wrote %s" % a.out)


if __name__ == "__main__":
    main()
