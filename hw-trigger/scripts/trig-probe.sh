#!/bin/bash
# trig-probe.sh — slow the trigger down until a multimeter can see it.
#
# At 30 Hz with a 5 ms pulse, a DMM on A0 reads a ~0.5 V average and tells you
# almost nothing. This sets 1 Hz with a 500 ms pulse: ~50% duty, so a working
# pin reads ~1.6 V average and visibly swings. That turns "is this pin driving?"
# into a five-second check with no scope.
#
# Measure A0..A3 to the board's GND in DC volts:
#
#   ~1.5-1.7 V swinging   pin is driving      -> fault is downstream (wire,
#                                                 ground return, optocoupler)
#   0 V steady            pin is NOT driving  -> wrong pin, or trigger stopped
#   3.3 V steady          stuck high          -> check polarity / `stop` state
#
# Then measure the module end the same way. If the STM32 pin swings and the
# module's Trig+ does not, the break is in that wire. If both swing and there
# are still no frames, suspect the Trig- return or the optocoupler direction.
#
# Usage:  ./trig-probe.sh          # enter probe mode (1 Hz, 500 ms)
#         ./trig-probe.sh restore  # back to 30 fps / 5 ms

set -u
HERE=$(cd "$(dirname "$0")" && pwd)
SEND="$HERE/trig-send.sh"
export TRIG_QUIET=1

if [ "${1:-probe}" = restore ]; then
	"$SEND" fps 30    >/dev/null
	"$SEND" exp 5000  >/dev/null
	"$SEND" start     >/dev/null
	echo "restored: 30 fps, 5000 us"
	"$SEND" status | grep -E '^(running|fps_milli|ch1_)'
	exit 0
fi

echo "Setting 1 Hz with a 500 ms pulse (~50% duty, DMM-visible)..."
"$SEND" fps 1        >/dev/null
"$SEND" exp 500000   >/dev/null
"$SEND" start        >/dev/null
"$SEND" status | grep -E '^(running|fps_milli|period_us|polarity|ch1_)'
cat <<'TXT'

Now measure, DC volts, against the board GND:

  WeAct A0  ->  camera C Trig+
  WeAct A1  ->  camera D Trig+
  WeAct A2  ->  camera E Trig+
  WeAct A3  ->  camera F Trig+
  WeAct G   ->  all four Trig-   <-- the shared return; if this is missing,
                                     all four cameras fail identically

  ~1.5-1.7 V swinging = driving | 0 V = not driving | 3.3 V = stuck high

Run './trig-probe.sh restore' when done.
TXT
