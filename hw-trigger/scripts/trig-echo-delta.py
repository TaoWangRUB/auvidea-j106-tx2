#!/usr/bin/env python3
"""Compare the camera's own timestamps against the REAL trigger edge, and report Delta.

The direct half of WIRING.md section 4.4. `trig-echo-stamp.py` records when each trigger
edge actually happened; the capture node's frame log records what the camera claims. This
matches them and reports the difference, which is the camera-side error `e` - the only
unknown left in the camera<->IMU chain once the frame-time fit is done.

THE ARITHMETIC, in CLOCK_MONOTONIC throughout. Under `pol 0` (active_low, this rig's
working polarity) the FALLING edge is the assertion, i.e. the start of exposure:

    true exposure start   = t_edge
    true midpoint         = t_edge + E/2          <- the instant the image represents
    what the node stamps  = t_sof - E/2

If SOF really marks the end of exposure (NVIDIA: "the time the first data from this
capture arrives from the sensor"), then t_sof = t_edge + E and the published stamp lands
exactly on the true midpoint. So the measurable error is

    e = t_sof - t_edge - E

and it should be ~0. Anything else is a constant the stamping does not model, and it
enters Delta directly:  Delta = w - e,  where w is the IMU's wake-up latency (tens of us,
and the reason trig-echo-stamp.py takes its own CLOCK_MONOTONIC rather than the kernel's).

  ./trig-echo-delta.py --echo echo.csv --frames cam1.csv

Both files are written on the board; this can run anywhere.
"""
import argparse
import statistics
import sys


def read_echo(path):
    ts = []
    for line in open(path):
        if line.startswith("#") or line.startswith("seq"):
            continue
        parts = line.strip().split(",")
        if len(parts) >= 3:
            ts.append((int(parts[1]), int(parts[2])))   # wake_ns, kernel_ns
    return ts


def read_frames(path):
    """camN.csv from frame_log_dir: timestamp,seq,capture_id,t_sof,exposure,image"""
    out = []
    for line in open(path):
        if line.startswith("#") or line.startswith("timestamp"):
            continue
        p = line.strip().split(",")
        if len(p) >= 5:
            out.append(dict(stamp=int(p[0]), seq=int(p[1]),
                            sof=int(p[3]), exposure=int(p[4])))
    return out


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--echo", required=True)
    ap.add_argument("--frames", required=True)
    ap.add_argument("--imu-wake-us", type=float, default=50.0,
                    help="IMU wake-up latency w in us; Delta = w - e (default 50)")
    a = ap.parse_args()

    edges = read_echo(a.echo)
    frames = read_frames(a.frames)
    if not edges or not frames:
        raise SystemExit("empty input: %d edges, %d frames" % (len(edges), len(frames)))

    wake = [e[0] for e in edges]
    kern = [e[1] for e in edges]
    lat = [(w - k) / 1e3 for w, k in zip(wake, kern)]      # us
    print("echo:   %d edges;  wake-minus-kernel latency  median %.1f us, p90 %.1f us"
          % (len(wake), statistics.median(lat), sorted(lat)[int(len(lat) * .9)]))
    print("frames: %d" % len(frames))

    # match each frame to the most recent edge at or before its SOF
    errs, matched, exposures = [], 0, set()
    j = 0
    for f in frames:
        while j + 1 < len(wake) and wake[j + 1] <= f["sof"]:
            j += 1
        if not wake or wake[j] > f["sof"]:
            continue
        gap = f["sof"] - wake[j]
        if gap > 60_000_000:            # more than ~2 periods: no usable edge
            continue
        exposures.add(f["exposure"])
        errs.append((gap - f["exposure"]) / 1e6)          # e, in ms
        matched += 1

    if not errs:
        raise SystemExit("no frame matched an edge — were both captured at the same time?")
    errs.sort()
    med = statistics.median(errs)
    print("\nexposure in the frame log: %s us"
          % ", ".join("%d" % (e / 1000) for e in sorted(exposures)))
    print("matched %d frames to edges" % matched)
    print("\n  e = t_sof - t_edge - exposure   (0 if SOF marks the end of exposure)")
    print("    median   %+8.4f ms" % med)
    print("    p5..p95  %+8.4f .. %+8.4f ms"
          % (errs[max(0, len(errs) // 20)], errs[min(len(errs) - 1, len(errs) * 19 // 20)]))
    print("    spread   %8.4f ms" % (errs[-1] - errs[0]))

    delta = a.imu_wake_us / 1e3 - med
    print("\n  Delta = w - e, with w = %.0f us" % a.imu_wake_us)
    print("    -> Delta = %+.4f ms" % delta)
    print("\n  Compare against the solver's value. Agreement means Delta is measured twice")
    print("  by independent means; disagreement means one of them is modelling something")
    print("  the other is not, and the difference is the size of it.")


if __name__ == "__main__":
    main()
