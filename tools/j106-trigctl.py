#!/usr/bin/env python3
"""j106-trigctl.py — drive the IMX296 external-trigger generator.

Talks the line protocol of the STM32H7 firmware in hw-trigger/firmware/ over a
plain serial port. The generator owns the frame rate AND the exposure: in the
IMX296's Fast Trigger mode the asserted pulse width IS the exposure time, so the
sensor's own exposure control does nothing while triggered.

The camera modules' trigger inputs are optocouplers isolated from module ground,
so two firmware commands exist for what that barrier hides -- `pol` (does driving
the LED assert the trigger, or release it?) and `skew` (the opto's on/off delay
asymmetry, which lands straight on exposure). Reach both with `raw`.

  >>> RUNS EITHER SIDE. <<<   Wherever the serial link is: over the firmware's
  USB CDC-ACM port (a /dev/ttyACM*, the usual case -- the same USB-C that powers
  and flashes the board), on the board if the MCU is wired to M110 J22 (a
  /dev/ttyTHS*), or on a USB-serial dongle (a /dev/ttyUSB*). All three speak the
  identical protocol and may be attached at once. Nothing here touches the
  cameras.

  >>> SETTINGS DO NOT SURVIVE A POWER CYCLE. <<<   The firmware boots at its
  compiled-in defaults (30 fps, 5 ms). That matters when the board is powered
  from the host's USB: a host reboot or suspend drops VBUS, resets the MCU, and
  the rig comes back at the defaults with no error anywhere. Re-apply after any
  reconnect, or power the board from its own 5 V and use USB for data only.

  ./j106-trigctl.py status
  ./j106-trigctl.py fps 30 --exposure 5000
  ./j106-trigctl.py start
  ./j106-trigctl.py burst 300
  ./j106-trigctl.py stop
  ./j106-trigctl.py raw 'exp 2 3000'    # per-camera exposure, ch 1..4
  ./j106-trigctl.py raw 'pol 0'
  ./j106-trigctl.py raw 'skew 8000'

Wiring, resistor values and bring-up order: hw-trigger/WIRING.md
"""
import argparse
import sys
import termios
import time

DEFAULT_PORT = "/dev/ttyUSB0"
DEFAULT_BAUD = 115200

# Datasheet limits, repeated here so the tool can say why something was
# refused without a round trip. The firmware enforces them for real.
MIN_PERIOD_US = 16682          # tTGPD 1126 H, all-pixel readout
MAX_FPS = 59.95
MIN_EXPOSURE_US = 15           # tTGSE + tOFFSET, rounded up


class TrigError(Exception):
    pass


class Link:
    """Bare termios serial. Avoids a pyserial dependency on the board."""

    def __init__(self, port, baud):
        try:
            self.fd = open(port, "r+b", buffering=0)
        except OSError as exc:
            raise TrigError(
                f"cannot open {port}: {exc}\n"
                "  - MCU on M110 J22?  try /dev/ttyTHS1, ttyTHS2 or ttyTHS3\n"
                "    (WIRING.md section 4.2 has a loopback test to identify which)\n"
                "  - MCU on a USB dongle?  try /dev/ttyUSB0\n"
                "  - permission denied?  you are not in the 'dialout' group"
            ) from exc

        speed = getattr(termios, f"B{baud}", None)
        if speed is None:
            raise TrigError(f"unsupported baud rate {baud}")

        attrs = termios.tcgetattr(self.fd)
        iflag, oflag, cflag, lflag, _, _, cc = attrs
        iflag = 0
        oflag = 0
        lflag = 0
        cflag = termios.CS8 | termios.CREAD | termios.CLOCAL
        cc = list(cc)
        cc[termios.VMIN] = 0
        cc[termios.VTIME] = 2          # 0.2 s inter-byte timeout
        termios.tcsetattr(
            self.fd, termios.TCSANOW,
            [iflag, oflag, cflag, lflag, speed, speed, cc])
        termios.tcflush(self.fd, termios.TCIOFLUSH)

    def command(self, text, settle=0.35):
        """Send one line, collect what comes back until it goes quiet."""
        termios.tcflush(self.fd, termios.TCIFLUSH)
        self.fd.write((text + "\n").encode())
        deadline = time.time() + settle
        out = b""
        while time.time() < deadline:
            chunk = self.fd.read(256)
            if chunk:
                out += chunk
                deadline = time.time() + 0.15   # keep reading while it talks
        return out.decode(errors="replace").replace("\r", "")

    def close(self):
        self.fd.close()


def report(text):
    """Print the reply; return False if the firmware refused."""
    lines = [ln for ln in text.splitlines() if ln.strip()]
    if not lines:
        print("no reply — is the MCU powered and the port right?",
              file=sys.stderr)
        return False
    for line in lines:
        print(line)
    return not any(ln.startswith("err") for ln in lines)


def main():
    ap = argparse.ArgumentParser(
        description="Drive the IMX296 external-trigger generator.",
        epilog="Wiring and bring-up: hw-trigger/WIRING.md")
    ap.add_argument("--port", default=DEFAULT_PORT,
                    help=f"serial device (default {DEFAULT_PORT})")
    ap.add_argument("--baud", type=int, default=DEFAULT_BAUD)

    sub = ap.add_subparsers(dest="cmd")
    sub.add_parser("status", help="report clock, period, exposure, pulse count")
    sub.add_parser("start", help="start triggering")
    sub.add_parser("stop", help="stop; every channel parks unasserted")
    sub.add_parser("help", help="ask the firmware for its own command list")

    p = sub.add_parser("fps", help="set frame rate (and optionally exposure)")
    p.add_argument("value", type=float)
    p.add_argument("--exposure", "-e", type=int,
                   help="exposure in microseconds, applied first")

    p = sub.add_parser("exposure", help="set exposure in microseconds")
    p.add_argument("value", type=int)

    p = sub.add_parser("burst", help="emit N pulses then stop")
    p.add_argument("count", type=int)

    p = sub.add_parser("raw", help="send a literal command line")
    p.add_argument("text")

    args = ap.parse_args()
    if getattr(args, "cmd", None) is None:      # py3.6 has no add_subparsers(required=)
        ap.error("a subcommand is required")

    # Catch the obvious refusals locally, with the reason.
    if args.cmd == "fps":
        if args.value <= 0 or args.value > MAX_FPS:
            sys.exit(f"fps must be >0 and <= {MAX_FPS} — above that the frame "
                     f"period is shorter than the sensor's tTGPD "
                     f"({MIN_PERIOD_US} us), see WIRING.md section 2")
        if args.exposure is not None and args.exposure < MIN_EXPOSURE_US:
            sys.exit(f"exposure must be >= {MIN_EXPOSURE_US} us")
    if args.cmd == "exposure" and args.value < MIN_EXPOSURE_US:
        sys.exit(f"exposure must be >= {MIN_EXPOSURE_US} us")
    if args.cmd == "burst" and args.count < 1:
        sys.exit("burst count must be >= 1")

    try:
        link = Link(args.port, args.baud)
    except TrigError as exc:
        sys.exit(str(exc))

    ok = True
    try:
        if args.cmd == "fps":
            if args.exposure is not None:
                ok &= report(link.command(f"exp {args.exposure}"))
            ok &= report(link.command(f"fps {args.value:g}"))
        elif args.cmd == "exposure":
            ok &= report(link.command(f"exp {args.value}"))
        elif args.cmd == "burst":
            ok &= report(link.command(f"burst {args.count}"))
        elif args.cmd == "raw":
            ok &= report(link.command(args.text))
        else:
            ok &= report(link.command(args.cmd))
    finally:
        link.close()

    sys.exit(0 if ok else 1)


if __name__ == "__main__":
    main()
