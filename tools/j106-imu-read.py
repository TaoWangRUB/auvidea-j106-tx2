#!/usr/bin/env python3
"""j106-imu-read.py — read the J106's MPU-9250 with data-ready timestamps.

The point of this tool is WHERE the timestamp comes from. The obvious reader
polls the SPI bus in a loop and stamps each sample when the read returns, which
dates the sample to "whenever userspace got round to it" - scheduler latency and
all. That error is invisible in the data and fatal to visual-inertial fusion.

Here the IMU's own data-ready INT line drives the timing: the sensor pulses it
the instant a sample is ready, and this tool waits on that edge and stamps THAT.
The SPI burst afterwards can take as long as it likes; it only fetches numbers
whose time is already known.

Three facts about this board make the code look the way it does:

  * The MPU's INT is wired to gpio-298 (GPIO9_MOTION_INT, AON gpiochip offset
    42) and the J106 INVERTS it, with no pull-up. So the sensor is configured
    push-pull ("totem pole") rather than the default open-drain, and the Tegra
    sees the sensor's rising assertion as a FALLING edge.
  * The GPIO character device stamps its own events with CLOCK_REALTIME
    (gpiolib.c: ktime_get_real_ns), while V4L2 stamps frames with
    CLOCK_MONOTONIC. Mixing the two silently misdates everything by the
    REALTIME-MONOTONIC offset. So the chardev's timestamp is DISCARDED for the
    sample time - it is used only by --latency, to measure how late we woke.
  * MPU-9250 FSYNC is not brought out by the J106, and Tegra's GTE hardware
    timestamping is Xavier-only, so waking userspace on the edge is genuinely
    the best available. --latency exists to put a number on what that costs.

  >>> RUN THIS ON THE BOARD. <<<   It opens /dev/spidev1.0 and /dev/gpiochip*,
  both root-only.

  sudo ./j106-imu-read.py -n 2000 -o /tmp/imu.csv
  sudo ./j106-imu-read.py --latency -n 2000      # edge -> wake-up error budget
  sudo ./j106-imu-read.py --rate 400 --gyro-fs 1000 --accel-fs 8

Output is CSV with a '#' provenance header:
    t_mono_ns, ax, ay, az, gx, gy, gz, temp_c, seq
accelerations in m/s^2, angular rates in rad/s, temperature in degC.

Companion tools: j106-frametime.py (the camera side of the same timebase),
j106-record-sync.py (both on one clock). Timebase notes: ../README.md.
"""
import argparse
import ctypes
import errno
import fcntl
import glob
import math
import os
import select
import statistics
import sys
import time

# ---- clock plumbing -------------------------------------------------------
# Python 3.6 (Ubuntu 18.04 on the board) has no time.clock_gettime_ns, and
# float seconds quantise to ~240 ns at epoch scale - too coarse to measure a
# wake-up. Call clock_gettime(2) directly and keep integer nanoseconds.
CLOCK_REALTIME = 0
CLOCK_MONOTONIC = 1

_libc = ctypes.CDLL("libc.so.6", use_errno=True)


class _Timespec(ctypes.Structure):
    _fields_ = [("tv_sec", ctypes.c_long), ("tv_nsec", ctypes.c_long)]


_libc.clock_gettime.argtypes = [ctypes.c_int, ctypes.POINTER(_Timespec)]
_ts = _Timespec()


def now_ns(clk=CLOCK_MONOTONIC):
    if _libc.clock_gettime(clk, ctypes.byref(_ts)) != 0:
        raise OSError(ctypes.get_errno(), "clock_gettime")
    return _ts.tv_sec * 1000000000 + _ts.tv_nsec


def clock_call_cost_ns(samples=2001):
    """Median cost of one now_ns() call, so --latency can report its own error."""
    deltas = []
    prev = now_ns()
    for _ in range(samples):
        cur = now_ns()
        deltas.append(cur - prev)
        prev = cur
    return statistics.median(deltas)


# ---- ioctl encoding -------------------------------------------------------
_IOC_NONE, _IOC_WRITE, _IOC_READ = 0, 1, 2


def _IOC(direction, typ, nr, size):
    return (direction << 30) | (size << 16) | (typ << 8) | nr


# ---- SPI (spidev) ---------------------------------------------------------
SPI_IOC_MAGIC = ord("k")


class _SpiTransfer(ctypes.Structure):
    _fields_ = [("tx_buf", ctypes.c_uint64), ("rx_buf", ctypes.c_uint64),
                ("len", ctypes.c_uint32), ("speed_hz", ctypes.c_uint32),
                ("delay_usecs", ctypes.c_uint16), ("bits_per_word", ctypes.c_uint8),
                ("cs_change", ctypes.c_uint8), ("tx_nbits", ctypes.c_uint8),
                ("rx_nbits", ctypes.c_uint8), ("pad", ctypes.c_uint16)]


SPI_IOC_WR_MODE = _IOC(_IOC_WRITE, SPI_IOC_MAGIC, 1, 1)
SPI_IOC_WR_BITS_PER_WORD = _IOC(_IOC_WRITE, SPI_IOC_MAGIC, 3, 1)
SPI_IOC_WR_MAX_SPEED_HZ = _IOC(_IOC_WRITE, SPI_IOC_MAGIC, 4, 4)
SPI_IOC_MESSAGE_1 = _IOC(_IOC_WRITE, SPI_IOC_MAGIC, 0, ctypes.sizeof(_SpiTransfer))


class Spi:
    """Minimal spidev: mode 0, 8 bits, one transfer per message."""

    def __init__(self, dev, speed_hz):
        self.dev = dev
        self.speed_hz = speed_hz
        self.fd = os.open(dev, os.O_RDWR)
        fcntl.ioctl(self.fd, SPI_IOC_WR_MODE, ctypes.c_uint8(0))
        fcntl.ioctl(self.fd, SPI_IOC_WR_BITS_PER_WORD, ctypes.c_uint8(8))
        fcntl.ioctl(self.fd, SPI_IOC_WR_MAX_SPEED_HZ, ctypes.c_uint32(speed_hz))

    def xfer(self, tx, speed_hz=None):
        n = len(tx)
        txbuf = ctypes.create_string_buffer(bytes(tx), n)
        rxbuf = ctypes.create_string_buffer(n)
        tr = _SpiTransfer(tx_buf=ctypes.addressof(txbuf),
                          rx_buf=ctypes.addressof(rxbuf), len=n,
                          speed_hz=speed_hz or self.speed_hz, bits_per_word=8)
        fcntl.ioctl(self.fd, SPI_IOC_MESSAGE_1, tr)
        return rxbuf.raw

    def close(self):
        if self.fd is not None:
            os.close(self.fd)
            self.fd = None


# ---- GPIO character device (v1 ABI, the only one on kernel 4.9) ------------
GPIO_MAGIC = 0xB4


class _GpioChipInfo(ctypes.Structure):
    _fields_ = [("name", ctypes.c_char * 32), ("label", ctypes.c_char * 32),
                ("lines", ctypes.c_uint32)]


class _GpioEventRequest(ctypes.Structure):
    _fields_ = [("lineoffset", ctypes.c_uint32), ("handleflags", ctypes.c_uint32),
                ("eventflags", ctypes.c_uint32), ("consumer_label", ctypes.c_char * 32),
                ("fd", ctypes.c_int)]


GPIO_GET_CHIPINFO_IOCTL = _IOC(_IOC_READ, GPIO_MAGIC, 0x01,
                               ctypes.sizeof(_GpioChipInfo))
GPIO_GET_LINEEVENT_IOCTL = _IOC(_IOC_READ | _IOC_WRITE, GPIO_MAGIC, 0x04,
                                ctypes.sizeof(_GpioEventRequest))

GPIOHANDLE_REQUEST_INPUT = 1 << 0
GPIOEVENT_REQUEST_RISING_EDGE = 1 << 0
GPIOEVENT_REQUEST_FALLING_EDGE = 1 << 1
GPIOEVENT_REQUEST_BOTH_EDGES = 3

EVENT_FLAGS = {"rising": GPIOEVENT_REQUEST_RISING_EDGE,
               "falling": GPIOEVENT_REQUEST_FALLING_EDGE,
               "both": GPIOEVENT_REQUEST_BOTH_EDGES}
EVENT_SIZE = 16                       # struct gpioevent_data: u64 + u32, padded


def resolve_line(global_gpio):
    """Map a sysfs GPIO number (e.g. 298) to (/dev/gpiochipN, offset).

    Deliberately not hard-coded: the chardev index and the sysfs base are
    assigned in probe order and are not guaranteed across kernels. Matching is
    by the chip's label and line count, which are properties of the hardware.
    """
    for sysdir in sorted(glob.glob("/sys/class/gpio/gpiochip*")):
        try:
            with open(os.path.join(sysdir, "base")) as fh:
                base = int(fh.read().strip())
            with open(os.path.join(sysdir, "ngpio")) as fh:
                ngpio = int(fh.read().strip())
            with open(os.path.join(sysdir, "label")) as fh:
                label = fh.read().strip()
        except OSError:
            continue
        if not base <= global_gpio < base + ngpio:
            continue
        denied = []
        for devpath in sorted(glob.glob("/dev/gpiochip*")):
            info = _GpioChipInfo()
            try:
                fd = os.open(devpath, os.O_RDONLY)
            except OSError as exc:
                if exc.errno in (errno.EACCES, errno.EPERM):
                    denied.append(devpath)
                continue
            try:
                fcntl.ioctl(fd, GPIO_GET_CHIPINFO_IOCTL, info)
            except OSError:
                continue
            finally:
                os.close(fd)
            if info.label.decode() == label and info.lines == ngpio:
                return devpath, global_gpio - base, label
        if denied:
            raise SystemExit("cannot open %s (permission denied) — run as root"
                             % ", ".join(denied))
        raise RuntimeError("gpio-%d is on sysfs chip %r (base %d) but no "
                           "/dev/gpiochip* reports that label"
                           % (global_gpio, label, base))
    raise RuntimeError("no gpiochip owns gpio-%d" % global_gpio)


class EdgeSource:
    """A GPIO line requested for edge events, with a poll()-able fd."""

    def __init__(self, global_gpio, edge, label="j106-imu-read"):
        self.chip, self.offset, self.chip_label = resolve_line(global_gpio)
        self.gpio = global_gpio
        self.edge = edge
        chip_fd = os.open(self.chip, os.O_RDONLY)
        req = _GpioEventRequest(lineoffset=self.offset,
                                handleflags=GPIOHANDLE_REQUEST_INPUT,
                                eventflags=EVENT_FLAGS[edge],
                                consumer_label=label.encode())
        try:
            fcntl.ioctl(chip_fd, GPIO_GET_LINEEVENT_IOCTL, req)
        except OSError as exc:
            os.close(chip_fd)
            if exc.errno == errno.EBUSY:
                raise SystemExit(
                    "gpio-%d (%s offset %d) is already claimed by another "
                    "consumer.\nCheck /sys/kernel/debug/gpio for who holds it."
                    % (global_gpio, self.chip, self.offset))
            raise
        os.close(chip_fd)
        self.fd = req.fd
        self.poller = select.poll()
        self.poller.register(self.fd, select.POLLIN)

    def wait(self, timeout_ms):
        """Block for an edge. Returns (n_events, last_kernel_realtime_ns).

        n_events > 1 means the kernel had already queued more than one edge
        before we ran - the data registers only ever hold the newest sample, so
        every event beyond the last one is a sample we lost.
        """
        if not self.poller.poll(timeout_ms):
            return 0, 0
        data = os.read(self.fd, EVENT_SIZE * 16)
        n = len(data) // EVENT_SIZE
        last = data[(n - 1) * EVENT_SIZE:(n - 1) * EVENT_SIZE + 8]
        return n, int.from_bytes(last, "little")

    def close(self):
        if self.fd is not None:
            os.close(self.fd)
            self.fd = None


# ---- MPU-9250 -------------------------------------------------------------
SMPLRT_DIV, CONFIG, GYRO_CONFIG, ACCEL_CONFIG, ACCEL_CONFIG2 = \
    0x19, 0x1A, 0x1B, 0x1C, 0x1D
INT_PIN_CFG, INT_ENABLE, INT_STATUS = 0x37, 0x38, 0x3A
ACCEL_XOUT_H = 0x3B
USER_CTRL, PWR_MGMT_1, PWR_MGMT_2, WHO_AM_I = 0x6A, 0x6B, 0x6C, 0x75

WHO_AM_I_EXPECTED = 0x71                                  # MPU-9250

ACCEL_FS = {2: 16384.0, 4: 8192.0, 8: 4096.0, 16: 2048.0}       # LSB per g
GYRO_FS = {250: 131.0, 500: 65.5, 1000: 32.8, 2000: 16.4}       # LSB per dps
G_MS2 = 9.80665
DEG2RAD = 3.141592653589793 / 180.0

# Datasheet group delays, ms, for DLPF_CFG / A_DLPF_CFG 0..4. These are the
# reason the timestamp is not the whole story: the edge marks when the FILTERED
# sample was ready, so the instant it describes is earlier by this much. Note
# the gyro path lags the accel path by ~1.0 ms at every matched setting - a
# fusion front-end that treats one timestamp as covering both inherits that.
GYRO_DLPF = {0: (250.0, 0.97), 1: (184.0, 2.9), 2: (92.0, 3.9),
             3: (41.0, 5.9), 4: (20.0, 9.9)}
ACCEL_DLPF = {0: (218.1, 1.88), 1: (218.1, 1.88), 2: (99.0, 2.88),
              3: (44.8, 4.88), 4: (21.2, 6.88)}


class Mpu9250:
    def __init__(self, spi, cfg_speed_hz=1000000):
        self.spi = spi
        self.cfg_speed = cfg_speed_hz        # register access is 1 MHz max
        self.accel_lsb = ACCEL_FS[4]
        self.gyro_lsb = GYRO_FS[500]

    def rd(self, reg):
        return self.spi.xfer(bytes([reg | 0x80, 0]), self.cfg_speed)[1]

    def wr(self, reg, val, verify=True):
        self.spi.xfer(bytes([reg & 0x7F, val]), self.cfg_speed)
        time.sleep(0.001)
        if verify:
            got = self.rd(reg)
            if got != val:
                raise RuntimeError("MPU reg 0x%02x: wrote 0x%02x, read back 0x%02x"
                                   % (reg, val, got))

    def probe(self):
        who = self.rd(WHO_AM_I)
        if who != WHO_AM_I_EXPECTED:
            hint = {0x70: "MPU-6500", 0x73: "MPU-9255", 0x68: "MPU-6050",
                    0x00: "no response - wrong spidev?",
                    0xFF: "no response - wrong spidev?"}.get(who, "unknown")
            raise SystemExit("WHO_AM_I = 0x%02x (%s), expected 0x71 (MPU-9250).\n"
                             "The J106 IMU is Tegra186 SPI3 = /dev/spidev1.0; "
                             "spidev0.0/2.0/3.0 are other controllers."
                             % (who, hint))
        return who

    def configure(self, rate_hz, gyro_fs, accel_fs, gyro_dlpf, accel_dlpf,
                  active_low):
        self.wr(PWR_MGMT_1, 0x80, verify=False)      # device reset
        time.sleep(0.1)
        self.wr(PWR_MGMT_1, 0x01)                    # auto-select best clock
        time.sleep(0.01)
        self.wr(PWR_MGMT_2, 0x00)                    # all axes enabled
        self.wr(USER_CTRL, 0x10)                     # I2C_IF_DIS: SPI only
        self.wr(CONFIG, gyro_dlpf & 0x07)            # FIFO_MODE=0, DLPF
        self.wr(GYRO_CONFIG, {250: 0, 500: 1, 1000: 2, 2000: 3}[gyro_fs] << 3)
        self.wr(ACCEL_CONFIG, {2: 0, 4: 1, 8: 2, 16: 3}[accel_fs] << 3)
        self.wr(ACCEL_CONFIG2, accel_dlpf & 0x0F)    # accel_fchoice_b=0
        div = int(round(1000.0 / rate_hz)) - 1
        if not 0 <= div <= 255:
            raise SystemExit("--rate %g gives SMPLRT_DIV %d; the DLPF base rate "
                             "is 1 kHz, so pick a rate in 3.9..1000 Hz" % (rate_hz, div))
        self.wr(SMPLRT_DIV, div)
        # Push-pull, not the default open-drain: the J106 gives this line no
        # pull-up, so open-drain would never present a usable high. Not latched
        # (a ~50 us pulse per sample) and cleared by any read.
        self.wr(INT_PIN_CFG, (0x80 if active_low else 0x00) | 0x10)
        self.wr(INT_ENABLE, 0x01)                    # RAW_RDY_EN
        self.accel_lsb = ACCEL_FS[accel_fs]
        self.gyro_lsb = GYRO_FS[gyro_fs]
        return 1000.0 / (div + 1)

    def read_burst(self, speed_hz):
        """accel(6) temp(2) gyro(6) in one transfer - one consistent sample."""
        raw = self.spi.xfer(bytes([ACCEL_XOUT_H | 0x80]) + b"\0" * 14, speed_hz)[1:]
        vals = [int.from_bytes(raw[i:i + 2], "big", signed=True)
                for i in range(0, 14, 2)]
        ax, ay, az, temp, gx, gy, gz = (vals[0], vals[1], vals[2], vals[3],
                                        vals[4], vals[5], vals[6])
        sa = G_MS2 / self.accel_lsb
        sg = DEG2RAD / self.gyro_lsb
        clipped = any(abs(v) >= 32760 for v in (ax, ay, az, gx, gy, gz))
        return (ax * sa, ay * sa, az * sa, gx * sg, gy * sg, gz * sg,
                temp / 333.87 + 21.0, clipped)

    def standby(self):
        try:
            self.wr(INT_ENABLE, 0x00, verify=False)
            self.wr(PWR_MGMT_1, 0x40, verify=False)   # sleep
        except OSError:
            pass


# ---- capture --------------------------------------------------------------
class Stats:
    """Counters shared by every consumer of iter_samples()."""

    def __init__(self):
        self.intervals = []      # ns between consecutive edges
        self.latencies = []      # ns, kernel IRQ -> our wake-up (--latency only)
        self.drops = 0
        self.late = 0
        self.clipped = 0
        self.timed_out = False


def iter_samples(mpu, src, nominal_ns, burst_speed, timeout_ms, stats,
                 latency=False, stop=None):
    """Yield (t_mono_ns, values, seq) once per data-ready edge.

    The timestamp is taken the instant poll() returns, BEFORE the SPI burst -
    that ordering is the whole point of the tool, so keep it. Shared by the CLI
    and by j106-record-sync.py, so there is one implementation of the timing.
    """
    prev_ns = None
    seq = 0
    while True:
        if stop is not None and stop():
            return
        n_ev, kern_real_ns = src.wait(timeout_ms)
        if n_ev == 0:
            stats.timed_out = True
            return
        t_ns = now_ns()                         # the sample time: taken first
        if latency:
            # Bracket the REALTIME read between two MONOTONIC reads so the
            # offset is dated to the midpoint, not to one call earlier.
            t_real = now_ns(CLOCK_REALTIME)
            t_ns2 = now_ns()
            offset = t_real - (t_ns + t_ns2) // 2
            stats.latencies.append(t_ns - (kern_real_ns - offset))
        if n_ev > 1:
            stats.late += 1
            stats.drops += n_ev - 1             # only the newest sample survives
        vals = mpu.read_burst(burst_speed)
        if vals[7]:
            stats.clipped += 1
        if prev_ns is not None:
            gap = t_ns - prev_ns
            stats.intervals.append(gap)
            missed = int(round(gap / nominal_ns)) - 1
            if missed > 0:
                stats.drops += missed
        prev_ns = t_ns
        yield t_ns, vals, seq
        seq += 1


def open_imu(args):
    """Bring the IMU up and claim its INT line. Returns (spi, mpu, src, info)."""
    edge = args.edge
    if edge is None:
        # The MPU asserts its INT high by default; the J106 inverts the line, so
        # the assertion arrives at the Tegra as a falling edge. --active-low
        # flips the sensor end, and therefore this end too.
        edge = "rising" if args.active_low else "falling"

    spi = Spi(args.spidev, args.spi_speed)
    try:
        mpu = Mpu9250(spi)
        who = mpu.probe()
        actual_rate = mpu.configure(args.rate, args.gyro_fs, args.accel_fs,
                                    args.gyro_dlpf, args.accel_dlpf,
                                    args.active_low)
        src = EdgeSource(args.gpio, edge)
    except BaseException:
        spi.close()
        raise
    info = {"who_am_i": who, "rate_hz": actual_rate, "edge": edge,
            "chip": src.chip, "offset": src.offset, "gpio": args.gpio,
            "spidev": args.spidev,
            "gyro_fs_dps": args.gyro_fs, "accel_fs_g": args.accel_fs,
            "gyro_dlpf_bw_hz": GYRO_DLPF[args.gyro_dlpf][0],
            "gyro_group_delay_ms": GYRO_DLPF[args.gyro_dlpf][1],
            "accel_dlpf_bw_hz": ACCEL_DLPF[args.accel_dlpf][0],
            "accel_group_delay_ms": ACCEL_DLPF[args.accel_dlpf][1],
            "clock": "CLOCK_MONOTONIC"}
    return spi, mpu, src, info


def summarise(intervals_ns, nominal_ns, latencies_ns, drops, late_reads,
              clipped, clock_cost_ns, accel_mean=None, out=sys.stderr):
    def line(txt):
        print(txt, file=out)

    line("")
    line("---- IMU capture summary " + "-" * 40)
    if intervals_ns:
        ivl = [i / 1000.0 for i in intervals_ns]           # us
        line("samples          %d" % (len(intervals_ns) + 1))
        line("interval         mean %.1f us  sd %.1f us  (nominal %.1f us)"
             % (statistics.mean(ivl), statistics.pstdev(ivl), nominal_ns / 1000.0))
        line("                 min %.1f  p95 %.1f  max %.1f us"
             % (min(ivl), sorted(ivl)[int(0.95 * (len(ivl) - 1))], max(ivl)))
        # A dropped sample doubles an interval, which would swamp a ppm-scale
        # figure - so the rate error is taken only over intervals that plainly
        # contain no gap. Same principle as indexing the camera fit by
        # sequence rather than by arrival order.
        clean = [i for i in intervals_ns if 0.75 * nominal_ns < i < 1.25 * nominal_ns]
        if clean:
            drift_ppm = (statistics.mean(clean) - nominal_ns) / nominal_ns * 1e6
            line("rate error       %+.0f ppm vs the commanded rate (IMU crystal vs "
                 "Tegra)" % drift_ppm)
            if len(clean) != len(intervals_ns):
                line("                 over %d of %d intervals; %d spanned a gap"
                     % (len(clean), len(intervals_ns), len(intervals_ns) - len(clean)))
        else:
            line("rate error       not measurable - every interval spans a gap")
    if accel_mean:
        g = math.sqrt(sum(a * a for a in accel_mean))
        line("gravity check    |a| = %.3f m/s^2 at rest  (expect ~9.81)" % g)
        # Orientation cannot change the MAGNITUDE, so a few percent here is the
        # part's uncalibrated zero-g offset (+-60 mg typical = +-0.6 m/s^2),
        # not a wrong scale factor - a wrong full-scale selection would be out
        # by a factor of two. It is exactly what a VIO/Kalibr calibration
        # estimates, so it is reported, not corrected.
        err_mg = abs(g - G_MS2) / G_MS2 * 1000.0
        line("                 %.0f mg from 1 g — %s" % (
            err_mg,
            "within the MPU-9250's uncalibrated zero-g offset (+-60 mg typ)"
            if err_mg < 150 else
            "MORE than the part's spec — check the full-scale setting"))
    line("dropped samples  %d  (gaps in the edge series)" % drops)
    line("late reads       %d  (>1 edge already queued when we woke)" % late_reads)
    if clipped:
        line("CLIPPED          %d samples at full scale - raise --gyro-fs/--accel-fs"
             % clipped)
    if latencies_ns:
        lat = sorted(l / 1000.0 for l in latencies_ns)      # us
        def pct(p):
            return lat[min(len(lat) - 1, int(p * (len(lat) - 1)))]
        med = statistics.median(lat)
        mad = statistics.median([abs(x - med) for x in lat])
        line("")
        line("---- edge -> userspace wake-up latency " + "-" * 26)
        line("(kernel IRQ timestamp vs our CLOCK_MONOTONIC read, %d samples)"
             % len(lat))
        line("median %.1f us    MAD %.2f us" % (med, mad))
        line("min %.1f  p25 %.1f  p75 %.1f  p95 %.1f  p99 %.1f  max %.1f us"
             % (lat[0], pct(0.25), pct(0.75), pct(0.95), pct(0.99), lat[-1]))
        # A single scheduling outlier in a few thousand samples moves the sd by
        # more than the entire body of the distribution does, so the sd is
        # reported next to the MAD rather than instead of it.
        line("mean %.1f us  sd %.1f us  <- sd is outlier-dominated; prefer MAD"
             % (statistics.mean(lat), statistics.pstdev(lat)))
        line("")
        line("Measurement floor: one clock_gettime call costs ~%.1f us here"
             % (clock_cost_ns / 1000.0))
        line("(ctypes into the vDSO, on this CPU). That cost does NOT enter the")
        line("figures above: the REALTIME read is bracketed between two MONOTONIC")
        line("reads and dated to their midpoint, which cancels the call cost to")
        line("first order and leaves only the asymmetry between the two calls.")
        line("")
        line("This latency is mostly BIAS, not jitter: the median is a constant")
        line("that the camera<->IMU offset absorbs, and the MAD is what actually")
        line("limits the IMU timestamp. Run under SCHED_FIFO (chrt -f 80) - it")
        line("cuts the tail by about half and barely moves the median.")
    line("-" * 64)


def capture(args, out_fh):
    spi, mpu, src, info = open_imu(args)
    try:
        clock_cost = clock_call_cost_ns() if args.latency else 0
        hdr = out_fh.write
        hdr("# j106-imu-read.py — MPU-9250, timestamped on the data-ready edge\n")
        hdr("# who_am_i=0x%02x spidev=%s gpio=%d chip=%s offset=%d edge=%s\n"
            % (info["who_am_i"], info["spidev"], info["gpio"], info["chip"],
               info["offset"], info["edge"]))
        hdr("# rate_hz=%.3f gyro_fs_dps=%d accel_fs_g=%d\n"
            % (info["rate_hz"], info["gyro_fs_dps"], info["accel_fs_g"]))
        hdr("# gyro_dlpf_bw_hz=%.1f gyro_group_delay_ms=%.2f\n"
            % (info["gyro_dlpf_bw_hz"], info["gyro_group_delay_ms"]))
        hdr("# accel_dlpf_bw_hz=%.1f accel_group_delay_ms=%.2f\n"
            % (info["accel_dlpf_bw_hz"], info["accel_group_delay_ms"]))
        hdr("# clock=CLOCK_MONOTONIC (matches V4L2 buffer timestamps; the GPIO\n")
        hdr("#   chardev's own CLOCK_REALTIME stamp is discarded, see --latency)\n")
        hdr("# t_mono_ns is the edge, i.e. when the FILTERED sample was ready;\n")
        hdr("#   subtract the group delay above for the instant it describes.\n")
        hdr("t_mono_ns,ax,ay,az,gx,gy,gz,temp_c,seq\n")

        nominal_ns = int(1e9 / info["rate_hz"])
        timeout_ms = max(50, int(8 * nominal_ns / 1e6))
        stats = Stats()
        asum = [0.0, 0.0, 0.0]
        nsamp = 0
        deadline = now_ns() + int(args.duration * 1e9) if args.duration else None

        def stop():
            return bool(deadline and now_ns() >= deadline)

        for t_ns, vals, seq in iter_samples(mpu, src, nominal_ns,
                                            args.burst_speed, timeout_ms, stats,
                                            latency=args.latency, stop=stop):
            out_fh.write("%d,%.6f,%.6f,%.6f,%.6f,%.6f,%.6f,%.2f,%d\n"
                         % (t_ns, vals[0], vals[1], vals[2], vals[3], vals[4],
                            vals[5], vals[6], seq))
            if args.flush:
                out_fh.flush()
            for i in range(3):
                asum[i] += vals[i]
            nsamp += 1
            if args.count and seq + 1 >= args.count:
                break

        if stats.timed_out:
            print("timeout waiting for a data-ready edge after %d ms — is the "
                  "INT line wired, and is the polarity right? (try --edge %s)"
                  % (timeout_ms,
                     "rising" if info["edge"] == "falling" else "falling"),
                  file=sys.stderr)
        summarise(stats.intervals, nominal_ns, stats.latencies, stats.drops,
                  stats.late, stats.clipped, clock_cost,
                  accel_mean=[a / nsamp for a in asum] if nsamp else None)
        return 0
    finally:
        mpu.standby()
        src.close()
        spi.close()


def main():
    ap = argparse.ArgumentParser(
        description="Read the J106 MPU-9250, timestamped on its data-ready edge.",
        epilog="Runs on the board. Needs root for /dev/spidev* and /dev/gpiochip*.")
    ap.add_argument("--spidev", default="/dev/spidev1.0",
                    help="Tegra186 SPI3 CS0 — the J106 IMU (default: %(default)s)")
    ap.add_argument("--gpio", type=int, default=298,
                    help="sysfs GPIO of the MPU INT line (default: %(default)s "
                         "= GPIO9_MOTION_INT)")
    ap.add_argument("--edge", choices=sorted(EVENT_FLAGS),
                    help="edge to wait for (default: falling, because the J106 "
                         "inverts INT; rising with --active-low)")
    ap.add_argument("--active-low", action="store_true",
                    help="drive the MPU's INT active-low (ACTL=1) instead of "
                         "active-high")
    ap.add_argument("--rate", type=float, default=200.0,
                    help="sample rate in Hz, from the 1 kHz DLPF base "
                         "(default: %(default)s)")
    ap.add_argument("--gyro-fs", type=int, default=500, choices=sorted(GYRO_FS),
                    help="gyro full scale, dps (default: %(default)s)")
    ap.add_argument("--accel-fs", type=int, default=4, choices=sorted(ACCEL_FS),
                    help="accel full scale, g (default: %(default)s)")
    ap.add_argument("--gyro-dlpf", type=int, default=2, choices=sorted(GYRO_DLPF),
                    help="gyro DLPF setting 0..4 (default: %(default)s = 92 Hz)")
    ap.add_argument("--accel-dlpf", type=int, default=2, choices=sorted(ACCEL_DLPF),
                    help="accel DLPF setting 0..4 (default: %(default)s = 99 Hz)")
    ap.add_argument("--spi-speed", type=int, default=1000000,
                    help="SPI clock for register access, Hz — the MPU-9250 "
                         "caps this at 1 MHz (default: %(default)s)")
    ap.add_argument("--burst-speed", type=int, default=8000000,
                    help="SPI clock for the sensor-data burst, Hz — allowed up "
                         "to 20 MHz (default: %(default)s)")
    ap.add_argument("-n", "--count", type=int, default=0,
                    help="stop after N samples (default: run until interrupted)")
    ap.add_argument("-t", "--duration", type=float, default=0.0,
                    help="stop after N seconds")
    ap.add_argument("-o", "--output", help="CSV file (default: stdout)")
    ap.add_argument("--latency", action="store_true",
                    help="also measure edge->wake-up latency, by comparing the "
                         "kernel's REALTIME event stamp with our MONOTONIC read")
    ap.add_argument("--flush", action="store_true",
                    help="flush every sample (for tailing a live capture)")
    args = ap.parse_args()

    if os.geteuid() != 0:
        print("warning: not root — /dev/spidev* and /dev/gpiochip* usually are",
              file=sys.stderr)

    out_fh = open(args.output, "w") if args.output else sys.stdout
    try:
        return capture(args, out_fh)
    except KeyboardInterrupt:
        return 0
    finally:
        if args.output:
            out_fh.close()


if __name__ == "__main__":
    sys.exit(main())
