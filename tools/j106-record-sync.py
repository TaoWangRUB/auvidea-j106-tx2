#!/usr/bin/env python3
"""j106-record-sync.py — record camera frame times and IMU samples on ONE clock.

Everything here is CLOCK_MONOTONIC, because that is the clock V4L2 stamps
buffers with. Nothing in the output is CLOCK_REALTIME, and nothing is stamped
when userspace happened to notice it:

  cameras  the V4L2 buffer stamp is EndOfFrame. Under the hardware trigger the
           frames are periodic, so the times are replaced by a least-squares
           fit over the sequence number (j106-frametime.py) and then walked
           back through the known readout and exposure to the exposure
           MIDPOINT - which is the instant a VIO wants.
  IMU      each sample is stamped at its data-ready edge, not at the SPI read
           (j106-imu-read.py).

One unknown then remains: Delta, the residual camera<->IMU offset. It is a
single constant because all four cameras share one trigger edge. Until it is
measured (trigger echo into gpio-389) or estimated (Kalibr), it is recorded as
zero AND declared unmeasured in meta.json - it is never silently assumed.

  >>> RUN THIS ON THE BOARD. <<<   Needs root: /dev/video*, /dev/spidev1.0 and
  /dev/gpiochip*.

  sudo systemctl stop nvargus-daemon
  sudo ./j106-record-sync.py -t 60 -o /tmp/rec --trigger-hz 30 --exposure-us 5000

Output is the EuRoC/ASL layout, so Kalibr, VINS and OpenVINS read it directly:

  meta.json          provenance: clocks, trigger state, Delta and its source
  imu0.csv           #timestamp [ns],w_x,w_y,w_z,a_x,a_y,a_z   (exactly EuRoC)
  cam0/data.csv      #timestamp [ns],seq,t_buffer [ns],t_fit [ns]
  UNSYNCHRONISED     present ONLY if the cameras were free-running

Pixel data is deliberately not recorded - four raw streams are ~380 MB/s. Drop
images into camN/data/ (j106-sync-frames.py) and they line up row by row.
"""
import argparse
import glob
import importlib.util
import json
import os
import statistics
import sys
import threading
import time

HERE = os.path.dirname(os.path.abspath(__file__))


def _load(name, path):
    spec = importlib.util.spec_from_file_location(name, os.path.join(HERE, path))
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


scheck = _load("scheck", "j106-sync-check.py")
imu = _load("imu", "j106-imu-read.py")
ft = _load("ft", "j106-frametime.py")

EUROC_IMU_HEADER = ("#timestamp [ns],w_RS_S_x [rad s^-1],w_RS_S_y [rad s^-1],"
                    "w_RS_S_z [rad s^-1],a_RS_S_x [m s^-2],a_RS_S_y [m s^-2],"
                    "a_RS_S_z [m s^-2]\n")


class ImuWorker(threading.Thread):
    """Runs the IMU reader until told to stop, writing EuRoC imu0.csv."""

    def __init__(self, args, path):
        super().__init__(daemon=True)
        self.args = args
        self.path = path
        self.stop_flag = threading.Event()
        self.info = None
        self.stats = imu.Stats()
        self.count = 0
        self.temps = []
        self.error = None
        self.ready = threading.Event()

    def run(self):
        spi = src = mpu = None
        try:
            spi, mpu, src, self.info = imu.open_imu(self.args)
            nominal_ns = int(1e9 / self.info["rate_hz"])
            timeout_ms = max(50, int(8 * nominal_ns / 1e6))
            with open(self.path, "w") as fh:
                fh.write(EUROC_IMU_HEADER)
                self.ready.set()
                for t_ns, v, _seq in imu.iter_samples(
                        mpu, src, nominal_ns, self.args.burst_speed,
                        timeout_ms, self.stats, stop=self.stop_flag.is_set):
                    # EuRoC column order is gyro first, then accel.
                    fh.write("%d,%.9f,%.9f,%.9f,%.9f,%.9f,%.9f\n"
                             % (t_ns, v[3], v[4], v[5], v[0], v[1], v[2]))
                    self.temps.append(v[6])
                    self.count += 1
        except BaseException as exc:                      # noqa: BLE001
            self.error = exc
        finally:
            self.ready.set()
            if mpu is not None:
                mpu.standby()
            if src is not None:
                src.close()
            if spi is not None:
                spi.close()


def write_camera_csv(path, cam, fit, offset_ns):
    """One row per frame. Column 1 is the corrected exposure midpoint.

    t_buffer and t_fit are kept alongside so the correction stays auditable -
    a recording that only carries the final number cannot be re-derived if a
    pipeline constant is later re-measured.
    """
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "w") as fh:
        fh.write("#timestamp [ns],seq,t_buffer [ns],t_fit [ns]\n")
        for k, t in zip(cam.seqs, cam.stamps):
            t_buf_ns = int(round(t * 1e9))
            t_fit_ns = (int(round((fit["a"] * k + fit["b"]) * 1e9))
                        if fit else t_buf_ns)
            fh.write("%d,%d,%d,%d\n"
                     % (t_fit_ns + offset_ns, k, t_buf_ns, t_fit_ns))


def main():
    ap = argparse.ArgumentParser(
        description="Record camera frame times and IMU samples on one clock.",
        epilog="Runs on the board, as root. See README §5 Stage 8.")
    ap.add_argument("-o", "--output", required=True, help="output directory")
    ap.add_argument("-t", "--duration", type=float, default=30.0,
                    help="seconds to record (default: %(default)s)")
    ap.add_argument("-d", "--device", action="append", dest="devices",
                    help="repeatable; default is every /dev/video*")
    ap.add_argument("--trigger-hz", type=float, default=30.0,
                    help="rate the STM32 was commanded to (default: %(default)s)")
    ap.add_argument("--exposure-us", type=float,
                    help="commanded trigger pulse width, us — needed for the "
                         "exposure midpoint")
    ap.add_argument("--skew-us", type=float, default=0.0,
                    help="optocoupler skew set in the firmware")
    ap.add_argument("--delta-us", type=float, default=0.0,
                    help="camera->IMU offset to apply, us. Default 0, recorded "
                         "as UNMEASURED — see tasks 4.x")
    ap.add_argument("--delta-source", default=None,
                    help="how --delta-us was obtained (e.g. 'trigger-echo "
                         "2026-09-01' or 'kalibr'); recorded in meta.json")
    ap.add_argument("--allow-unsynchronised", action="store_true",
                    help="record even if the cameras are free-running (the "
                         "output is then marked UNSYNCHRONISED)")
    # --- IMU options: same names as j106-imu-read.py, so open_imu() takes this
    # namespace directly rather than a translated copy.
    ap.add_argument("--spidev", default="/dev/spidev1.0")
    ap.add_argument("--gpio", type=int, default=298)
    ap.add_argument("--edge", choices=sorted(imu.EVENT_FLAGS))
    ap.add_argument("--active-low", action="store_true")
    ap.add_argument("--rate", type=float, default=200.0,
                    help="IMU sample rate, Hz (default: %(default)s)")
    ap.add_argument("--gyro-fs", type=int, default=500, choices=sorted(imu.GYRO_FS))
    ap.add_argument("--accel-fs", type=int, default=4, choices=sorted(imu.ACCEL_FS))
    ap.add_argument("--gyro-dlpf", type=int, default=2, choices=sorted(imu.GYRO_DLPF))
    ap.add_argument("--accel-dlpf", type=int, default=2, choices=sorted(imu.ACCEL_DLPF))
    ap.add_argument("--spi-speed", type=int, default=1000000)
    ap.add_argument("--burst-speed", type=int, default=8000000)
    ap.add_argument("--no-imu", action="store_true",
                    help="cameras only (still writes the full provenance)")
    args = ap.parse_args()

    if os.geteuid() != 0:
        print("warning: not root — the device nodes usually need it",
              file=sys.stderr)
    os.makedirs(args.output, exist_ok=True)

    devices = args.devices or sorted(glob.glob("/dev/video*"))
    if not devices:
        sys.exit("no /dev/video* found")
    frames = max(10, int(args.duration * args.trigger_hz))

    cams = [scheck.Camera(d) for d in devices]
    opened = []
    for c in cams:
        try:
            c.open()
            opened.append(c)
        except RuntimeError as exc:
            print("skipping %s: %s" % (c.dev, exc), file=sys.stderr)
    if not opened:
        sys.exit("no camera could be opened")

    worker = None
    try:
        if not args.no_imu:
            worker = ImuWorker(args, os.path.join(args.output, "imu0.csv"))
            worker.start()
            worker.ready.wait(timeout=10)
            if worker.error:
                raise SystemExit("IMU startup failed: %s" % worker.error)

        print("recording %.1f s: %d frames from %s%s ..."
              % (args.duration, frames, ", ".join(c.name for c in opened),
                 "" if args.no_imu else " + IMU @ %.0f Hz" % args.rate))
        t0 = time.time()
        slew = ft.ClockSlew()
        threads = [threading.Thread(target=c.run, args=(frames,))
                   for c in opened]
        for t in threads:
            t.start()
        for t in threads:
            t.join()
        wall = time.time() - t0
        slew_ppm = slew.ppm()
    finally:
        if worker is not None:
            worker.stop_flag.set()
            worker.join(timeout=5)
        for c in opened:
            c.close()

    for c in opened:
        if c.error:
            print("%s: capture error: %s" % (c.name, c.error), file=sys.stderr)
    live = [c for c in opened if len(c.stamps) >= 10]
    if not live:
        sys.exit("no camera delivered enough frames")

    problems, notes = ft.check_triggered(live)
    synchronised = not problems
    print("\ntrigger state")
    for n in notes:
        print("  %s" % n)
    for p in problems:
        print("  PROBLEM: %s" % p)
    if problems and not args.allow_unsynchronised:
        sys.exit("\nrefusing to record: the cameras are free-running, so frame "
                 "times cannot be\nput on a stable timebase. Fix the trigger, or "
                 "pass --allow-unsynchronised\nto record anyway (the output is "
                 "then marked UNSYNCHRONISED).")

    model = ft.exposure_model(args) if args.exposure_us else None
    # Delta is signed the same way round as the correction it belongs to: a
    # positive Delta says the camera instant is LATER on the IMU's clock.
    offset_ns = int(round(args.delta_us * 1000.0))
    if model:
        offset_ns += int(round(model["raw_buffer_to_mid_us"] * 1000.0))

    meta = {
        "tool": "j106-record-sync.py",
        "clock": "CLOCK_MONOTONIC",
        "duration_s": wall,
        "synchronised": synchronised,
        "trigger": {"commanded_hz": args.trigger_hz,
                    "exposure_us": args.exposure_us,
                    "skew_us": args.skew_us,
                    "checks": notes, "problems": problems},
        # V4L2 and the IMU are both stamped on CLOCK_MONOTONIC, which NTP
        # disciplines, so this slew is COMMON MODE and cancels in Delta. It is
        # recorded because it does corrupt any rate figure derived from the fit.
        "clock_monotonic_slew_ppm": slew_ppm,
        "ntp_daemon": ft.ClockSlew.ntp_daemon(),
        "delta_us": args.delta_us,
        "delta_source": args.delta_source or ("UNMEASURED — assumed zero"
                                              if args.delta_us == 0 else "user"),
        "exposure_model": model,
        "constants": {"sensor_trig_to_exp_us": ft.SENSOR_TRIG_TO_EXP_US,
                      "readout_ms": ft.READOUT_MS, "isp_ms": ft.ISP_MS,
                      "raw_delivery_ms": ft.RAW_DELIVERY_MS},
        "timestamp_definition": (
            "cam*/data.csv column 1 = exposure midpoint = fitted EndOfFrame "
            "- readout - t_exp/2 + delta; imu0.csv column 1 = the data-ready "
            "edge (subtract the group delay below for the instant the sample "
            "describes)"),
        "cameras": {}, "imu": None,
    }

    if slew_ppm is not None and abs(slew_ppm) > 1.0:
        print("\nCLOCK_MONOTONIC slew during this run: %+.2f ppm%s"
              % (slew_ppm, " (%s active)" % ft.ClockSlew.ntp_daemon()
                 if ft.ClockSlew.ntp_daemon() else ""))
        print("  Common mode between camera and IMU, so it cancels in Delta — "
              "but it does\n  corrupt the fitted rate. Recorded in meta.json.")

    print("\nper camera")
    for i, c in enumerate(live):
        fit = ft.fit_line(c.seqs, c.stamps)
        write_camera_csv(os.path.join(args.output, "cam%d" % i, "data.csv"),
                         c, fit, offset_ns)
        dropped = (c.seqs[-1] - c.seqs[0] + 1) - len(c.seqs)
        entry = {"device": c.dev, "node": c.name, "frames": len(c.stamps),
                 "dropped": dropped, "dir": "cam%d" % i}
        if fit:
            entry.update({"fit_a_s": fit["a"], "fit_b_s": fit["b"],
                          "period_us": fit["a"] * 1e6, "fps": 1.0 / fit["a"],
                          "resid_sd_us": fit["resid_sd"] * 1e6,
                          "phase_se_us": fit["phase_se"] * 1e6})
            if args.trigger_hz:
                nominal = 1.0 / args.trigger_hz
                entry["rate_offset_ppm"] = (fit["a"] - nominal) / nominal * 1e6
            print("  cam%d %-10s %5d frames, %d dropped, %.4f fps, "
                  "fit resid %.2f us" % (i, c.name, len(c.stamps), dropped,
                                         1.0 / fit["a"], fit["resid_sd"] * 1e6))
        else:
            print("  cam%d %-10s %5d frames — too few to fit; raw buffer "
                  "stamps written" % (i, c.name, len(c.stamps)))
        meta["cameras"]["cam%d" % i] = entry

    if worker is not None:
        if worker.error:
            print("\nIMU: FAILED — %s" % worker.error, file=sys.stderr)
            meta["imu"] = {"error": str(worker.error)}
        else:
            st = worker.stats
            meta["imu"] = dict(worker.info or {})
            meta["imu"].update({
                "file": "imu0.csv", "samples": worker.count,
                "dropped": st.drops, "late_reads": st.late,
                "clipped": st.clipped,
                "interval_sd_us": (statistics.pstdev(st.intervals) / 1000.0
                                   if len(st.intervals) > 1 else None),
                "temp_c": {"min": min(worker.temps), "mean":
                           statistics.mean(worker.temps), "max": max(worker.temps)}
                          if worker.temps else None,
            })
            print("\nIMU  %d samples, %d dropped, %d late reads"
                  % (worker.count, st.drops, st.late))
            if st.timed_out:
                print("     WARNING: the data-ready edge stopped arriving")

    if not synchronised:
        with open(os.path.join(args.output, "UNSYNCHRONISED"), "w") as fh:
            fh.write("The cameras were FREE-RUNNING for this recording.\n\n")
            for p in problems:
                fh.write("  - %s\n" % p)
            fh.write("\nEach sensor ran on its own crystal at a phase that is "
                     "re-randomised on\nevery stream start, so the frame times "
                     "here share no timebase with each\nother or with the IMU. "
                     "Do not use this recording to calibrate a\ncamera<->IMU "
                     "offset.\n")
        print("\nMARKED UNSYNCHRONISED — see %s/UNSYNCHRONISED"
              % args.output)

    with open(os.path.join(args.output, "meta.json"), "w") as fh:
        json.dump(meta, fh, indent=2, sort_keys=True)
    written = ["meta.json", "cam*/data.csv"]
    if worker is not None and not worker.error:
        written.insert(1, "imu0.csv")
    print("\nwrote %s/  (%s)" % (args.output, ", ".join(written)))
    if args.delta_us == 0.0 and not args.delta_source:
        print("NOTE: Delta is UNMEASURED and recorded as zero. The camera and "
              "IMU series\n      share a clock but not yet a calibrated offset "
              "— see tasks 4.x.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
