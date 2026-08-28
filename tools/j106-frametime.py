#!/usr/bin/env python3
"""j106-frametime.py — recover exposure times from the trigger's periodicity.

A single V4L2 timestamp is a noisy estimate of when a frame was exposed. But
under the hardware trigger the frames are not independent: they are pinned to a
crystal, so their times lie on a straight line. Fitting that line

    t[k] = a*k + b        (k = the V4L2 sequence number, NOT arrival order)

recovers the phase far better than any single stamp - per-frame jitter is about
1.5 us sd, and a least-squares phase falls as 1/sqrt(N). The slope `a` is the
trigger period measured in Tegra clock units, which is what absorbs the drift
between the STM32's crystal and the Tegra's (measured: about -15 ppm, i.e. the
rig walks ~50 ms per hour against CLOCK_MONOTONIC). A single fixed camera<->IMU
offset is only meaningful once that walk is taken out.

Indexing by `sequence` matters: a dropped frame leaves a hole, and fitting
against arrival order would slide every later frame one period earlier and
corrupt both coefficients.

This refuses to fit free-running cameras. Free-running, each sensor is on its
own crystal at an arbitrary phase that is re-randomised every stream start, so
a fit describes nothing that will still be true next time.

  >>> RUN THIS ON THE BOARD. <<<   It opens /dev/video* directly. Reuses
  j106-sync-check.py's Camera class rather than reimplementing V4L2.

  sudo systemctl stop nvargus-daemon           # the nodes must be free
  sudo ./j106-frametime.py -n 300 --trigger-hz 30 --exposure-us 5000
  sudo ./j106-frametime.py -n 600 --json /tmp/fit.json

Pipeline constants and the timebase chain: ../README.md; trigger control:
j106-trigctl.py.
"""
import argparse
import glob
import importlib.util
import json
import math
import os
import sys
import threading

HERE = os.path.dirname(os.path.abspath(__file__))
_spec = importlib.util.spec_from_file_location(
    "scheck", os.path.join(HERE, "j106-sync-check.py"))
scheck = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(scheck)

# ---- measured pipeline constants ------------------------------------------
# All from the hardware-trigger bring-up on IMX296 @1456x1088 (README §5
# Stage 8 §11). They are properties of this sensor at this mode - re-measure if
# either changes.
SENSOR_TRIG_TO_EXP_US = 14.26   # datasheet: t_exp = t_pulse + this
READOUT_MS = 16.1               # end of exposure -> the V4L2 buffer timestamp
ISP_MS = 1.9                    # readout end -> Argus ISP-done (nearly free)
RAW_DELIVERY_MS = 66.7          # buffer stamp -> DQBUF returns; a DELIVERY
                                # latency, not a timestamp correction (sd 0.03,
                                # inherent, not queue depth)

TRIGGER_MODE_SYSFS = "/sys/module/imx296/parameters/trigger_mode"


def fit_line(seqs, times):
    """Least squares t = a*k + b over (sequence, timestamp).

    Returns a dict with the coefficients, the residual sd, and the standard
    error of the phase AT THE CENTRE of the run. Quoting the phase error at
    k=0 instead would be dominated by extrapolating the slope back to a
    sequence number the run never contained.
    """
    n = len(seqs)
    if n < 3:
        return None
    mk = sum(seqs) / n
    mt = sum(times) / n
    skk = sum((k - mk) ** 2 for k in seqs)
    if skk == 0:
        return None
    a = sum((k - mk) * (t - mt) for k, t in zip(seqs, times)) / skk
    b = mt - a * mk
    resid = [t - (a * k + b) for k, t in zip(seqs, times)]
    dof = n - 2
    sse = sum(r * r for r in resid)
    sigma = math.sqrt(sse / dof) if dof > 0 else 0.0
    return {"a": a, "b": b, "n": n, "resid_sd": sigma,
            "slope_se": sigma / math.sqrt(skk) if skk else 0.0,
            "phase_se": sigma / math.sqrt(n),
            "resid_max": max(abs(r) for r in resid),
            "k0": seqs[0], "k1": seqs[-1], "resid": resid}


def sqrt_n_table(seqs, times, out=sys.stdout):
    """Refit over growing prefixes: the phase error should fall as 1/sqrt(N).

    This is a check on the fit itself. If the residuals were correlated rather
    than independent jitter - a slowly wandering phase, say - the measured
    error would flatten out instead of tracking the ideal curve, and the whole
    "average away the jitter" premise would be wrong.
    """
    n = len(seqs)
    sizes = [s for s in (25, 50, 100, 200, 400, 800, 1600) if s <= n]
    if n not in sizes:
        sizes.append(n)
    if len(sizes) < 2:
        return
    # The ideal curve is anchored on the FULL run's residual sd, not on the
    # smallest prefix's: sigma estimated from 25 frames is itself noisy, and
    # anchoring there would bend the reference line rather than the data.
    full = fit_line(seqs, times)
    if not full:
        return
    print("\n  phase uncertainty vs frame count (should track 1/sqrt(N))",
          file=out)
    print("    %8s %14s %14s" % ("N", "measured", "ideal"), file=out)
    for s in sizes:
        f = fit_line(seqs[:s], times[:s])
        if not f:
            continue
        ideal = full["resid_sd"] / math.sqrt(s)
        print("    %8d %11.3f us %11.3f us"
              % (s, f["phase_se"] * 1e6, ideal * 1e6), file=out)


def trigger_mode():
    """(value, explanation) from the driver's module parameter, or (None, why)."""
    try:
        with open(TRIGGER_MODE_SYSFS) as fh:
            return int(fh.read().strip()), None
    except FileNotFoundError:
        return None, ("%s is absent — the imx296 driver is not loaded (an "
                      "IMX219 population?), so the trigger state cannot be "
                      "read here" % TRIGGER_MODE_SYSFS)
    except (OSError, ValueError) as exc:
        return None, "%s unreadable: %s" % (TRIGGER_MODE_SYSFS, exc)


def check_triggered(live):
    """Refuse to fit unless the cameras really are on one trigger.

    Two independent checks, because each covers the other's blind spot: the
    module parameter says what the DRIVER was told, and the inter-camera skew
    says what the WIRING actually did. With one camera only the first is
    available - which is exactly why the second is not skipped when it can run.
    """
    problems, notes = [], []

    mode, why = trigger_mode()
    if mode is None:
        notes.append(why)
    elif mode == 0:
        problems.append("imx296.trigger_mode = 0 — the cameras are free-running.\n"
                        "  echo 1 | sudo tee %s" % TRIGGER_MODE_SYSFS)
    else:
        notes.append("imx296.trigger_mode = %d" % mode)

    if len(live) >= 2:
        # Same two criteria as j106-sync-check.py, and the same thresholds:
        # offset carries the short runs, drift carries the long ones.
        ref = live[0]
        worst_skew = worst_rate = 0.0
        for other in live[1:]:
            sk = scheck.nearest_skew(ref.stamps, other.stamps)
            worst_skew = max(worst_skew, max(abs(s) for s in sk))
            worst_rate = max(worst_rate, abs(scheck.drift_rate(ref.stamps, sk)))
        span = ref.stamps[-1] - ref.stamps[0]
        notes.append("worst inter-camera skew %.1f us, drift %.2f us/s over %.1f s"
                     % (worst_skew * 1e6, worst_rate * 1e6, span))
        if worst_skew > 1000e-6:
            problems.append("cameras sit %.1f ms apart — no shared trigger edge "
                            "can do that" % (worst_skew * 1e3))
        if worst_rate > 2e-6 and span >= 5.0:
            problems.append("skew drifts at %.2f us/s — sensors on one clock "
                            "cannot drift" % (worst_rate * 1e6))
    else:
        notes.append("only one camera — the inter-camera skew test cannot run; "
                     "the module parameter is the only evidence")

    return problems, notes


def exposure_model(args):
    """The constant between a V4L2 buffer stamp and the exposure midpoint.

    Fast Trigger makes the asserted pulse width the exposure, so this is not an
    AE estimate that has to be read back per frame - it is known exactly from
    what the STM32 was commanded, and one value covers all four cameras because
    they share the edge.

        trigger edge --[ t_exp ]--> readout --[ 16.1 ms ]--> buffer timestamp

    so  t_mid = t_buffer - readout - t_exp/2.
    """
    if args.exposure_us is None:
        return None
    t_exp_us = args.exposure_us + SENSOR_TRIG_TO_EXP_US + args.skew_us
    return {
        "t_exp_us": t_exp_us,
        "readout_us": READOUT_MS * 1000.0,
        "raw_buffer_to_mid_us": -(READOUT_MS * 1000.0 + t_exp_us / 2.0),
        "argus_sof_to_mid_us": -t_exp_us / 2.0,
    }


def main():
    ap = argparse.ArgumentParser(
        description="Fit t[k] = a*k + b to V4L2 timestamps under the hardware "
                    "trigger, and report exposure times.",
        epilog="Runs on the board. See hw-trigger/WIRING.md and README §5 Stage 8.")
    ap.add_argument("-n", "--frames", type=int, default=300,
                    help="frames per camera (default: %(default)s)")
    ap.add_argument("-d", "--device", action="append", dest="devices",
                    help="repeatable; default is every /dev/video*")
    ap.add_argument("--trigger-hz", type=float,
                    help="rate the STM32 was commanded to, so the fit can "
                         "report the STM32-to-Tegra rate offset in ppm")
    ap.add_argument("--exposure-us", type=float,
                    help="commanded trigger pulse width, us — enables the "
                         "exposure-midpoint output")
    ap.add_argument("--skew-us", type=float, default=0.0,
                    help="optocoupler skew currently set in the firmware "
                         "(j106-trigctl.py raw 'skew ...'), default %(default)s")
    ap.add_argument("--json", metavar="FILE",
                    help="also write the fit as JSON, for j106-record-sync.py")
    ap.add_argument("--force", action="store_true",
                    help="fit even if the cameras look free-running (the "
                         "result describes only this one stream session)")
    args = ap.parse_args()

    devices = args.devices or sorted(glob.glob("/dev/video*"))
    if not devices:
        sys.exit("no /dev/video* found")

    cams = [scheck.Camera(d) for d in devices]
    opened = []
    try:
        for c in cams:
            try:
                c.open()
                opened.append(c)
            except RuntimeError as exc:
                print("skipping %s: %s" % (c.dev, exc), file=sys.stderr)
        if not opened:
            sys.exit("no camera could be opened")

        print("streaming %d frames from %s ..."
              % (args.frames, ", ".join(c.name for c in opened)))
        threads = [threading.Thread(target=c.run, args=(args.frames,))
                   for c in opened]
        for t in threads:
            t.start()
        for t in threads:
            t.join()
    finally:
        for c in opened:
            c.close()

    for c in opened:
        if c.error:
            print("%s: capture error: %s" % (c.name, c.error), file=sys.stderr)
    live = [c for c in opened if len(c.stamps) >= 10]
    if not live:
        sys.exit("no camera delivered enough frames to fit")

    problems, notes = check_triggered(live)
    print("\ntrigger state")
    for n in notes:
        print("  %s" % n)
    if problems:
        for p in problems:
            print("  PROBLEM: %s" % p)
        if not args.force:
            print("\nrefusing to fit: a free-running rig has no stable phase, so "
                  "the coefficients\nwould describe only this stream session and "
                  "would be re-randomised at the next\nSTREAMON. Fix the trigger, "
                  "or pass --force to fit anyway.")
            return 1
        print("\n--force: fitting anyway. These coefficients do NOT survive a "
              "restream.")

    model = exposure_model(args)
    result = {"devices": [c.name for c in live], "clock": "CLOCK_MONOTONIC",
              "frames_requested": args.frames, "triggered": not problems,
              "forced": bool(problems and args.force),
              "constants": {"sensor_trig_to_exp_us": SENSOR_TRIG_TO_EXP_US,
                            "readout_ms": READOUT_MS, "isp_ms": ISP_MS,
                            "raw_delivery_ms": RAW_DELIVERY_MS},
              "exposure_model": model, "cameras": {}}

    print("\nfit  t[k] = a*k + b   (k = V4L2 sequence, t = CLOCK_MONOTONIC)")
    print("  %-10s %6s %7s %14s %11s %11s %11s"
          % ("node", "frames", "dropped", "a (period)", "fps", "resid sd", "phase se"))
    for c in live:
        f = fit_line(c.seqs, c.stamps)
        if not f:
            print("  %-10s too few frames to fit" % c.name)
            continue
        dropped = (c.seqs[-1] - c.seqs[0] + 1) - len(c.seqs)
        print("  %-10s %6d %7d %11.3f us %11.4f %8.3f us %8.3f us"
              % (c.name, f["n"], dropped, f["a"] * 1e6, 1.0 / f["a"],
                 f["resid_sd"] * 1e6, f["phase_se"] * 1e6))
        entry = {"a_s": f["a"], "b_s": f["b"], "period_us": f["a"] * 1e6,
                 "fps": 1.0 / f["a"], "n": f["n"], "dropped": dropped,
                 "seq_first": f["k0"], "seq_last": f["k1"],
                 "resid_sd_us": f["resid_sd"] * 1e6,
                 "resid_max_us": f["resid_max"] * 1e6,
                 "phase_se_us": f["phase_se"] * 1e6,
                 "slope_se_us": f["slope_se"] * 1e6}
        if args.trigger_hz:
            nominal = 1.0 / args.trigger_hz
            ppm = (f["a"] - nominal) / nominal * 1e6
            entry["rate_offset_ppm"] = ppm
            entry["walk_ms_per_hour"] = ppm * 3.6
        if model:
            entry["t_mid_offset_us"] = model["raw_buffer_to_mid_us"]
        result["cameras"][c.name] = entry

    if args.trigger_hz:
        print("\nSTM32 vs Tegra rate (commanded %.4f Hz = %.3f us)"
              % (args.trigger_hz, 1e6 / args.trigger_hz))
        for name, e in result["cameras"].items():
            print("  %-10s %+8.2f ppm   the rig walks %+.1f ms per hour "
                  "against CLOCK_MONOTONIC"
                  % (name, e["rate_offset_ppm"], e["walk_ms_per_hour"]))
        print("  This is why a camera<->IMU offset is a fit, not a constant: a"
              "\n  fixed offset calibrated once goes stale at exactly this rate.")

    if model:
        print("\nexposure midpoint  (commanded pulse %.1f us + sensor %.2f us + "
              "skew %.1f us\n                    = t_exp %.2f us)"
              % (args.exposure_us, SENSOR_TRIG_TO_EXP_US, args.skew_us,
                 model["t_exp_us"]))
        print("  raw V4L2   t_mid = t_buffer %+.3f ms   (buffer stamp is "
              "EndOfFrame)" % (model["raw_buffer_to_mid_us"] / 1000.0))
        print("  Argus      t_mid = t_SOF    %+.3f ms" %
              (model["argus_sof_to_mid_us"] / 1000.0))
        print("  Use the FITTED t[k] in place of the raw stamp: same offset, "
              "less noise.")
        print("  (Delivery is a further %.1f ms after the buffer stamp — that is "
              "latency,\n   not a timestamp correction, so it does NOT enter "
              "t_mid.)" % RAW_DELIVERY_MS)

    ref = max(live, key=lambda c: len(c.stamps))
    sqrt_n_table(ref.seqs, ref.stamps)

    if args.json:
        with open(args.json, "w") as fh:
            json.dump(result, fh, indent=2, sort_keys=True)
        print("\nwrote %s" % args.json)
    return 0


if __name__ == "__main__":
    sys.exit(main())
