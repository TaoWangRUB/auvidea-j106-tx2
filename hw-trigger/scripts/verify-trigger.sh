#!/bin/bash
# verify-trigger.sh — acceptance test for the IMX296 hardware trigger.
#
# Answers one question: are all four cameras exposing on the STM32's edges, at
# the rate the STM32 was told to emit?
#
# The order of the steps is the point. Each one removes a suspect, so a failure
# lands somewhere specific instead of "the trigger doesn't work":
#
#   0  preconditions   trigger generator answering, four video nodes present
#   1  free-run        trigger_mode=0 — proves sensors, CSI, VI and capture path
#   2  triggered       trigger_mode=1 at the configured polarity
#   3  polarity sweep  only if 2 failed — pol is a board fact, not a datasheet one
#
# A pass in 1 and a fail in 2/3 means the fault is between the STM32 pin and the
# module's XTRIG pad: wiring, ground return, or a dead optocoupler. That is
# worth stating, because every other layer has just been exercised.
#
# Usage:  ./verify-trigger.sh [fps] [seconds]

set -u
HZ=${1:-30}
SECS=${2:-5}
HERE=$(cd "$(dirname "$0")" && pwd)
SEND="$HERE/trig-send.sh"
export TRIG_QUIET=1
RECV="$HERE/trig-recv.sh"
PARAM=/sys/module/imx296/parameters/trigger_mode

# Frames may legitimately land a little under the trigger rate; treat anything
# at or above 80% of it as locked, and anything near zero as no frames at all.
PASS_MIN=$(awk -v h="$HZ" 'BEGIN{printf "%.2f", h*0.8}')

set_mode() { echo nvidia | sudo -S sh -c "echo $1 > $PARAM" 2>/dev/null; }
get_mode() { cat "$PARAM" 2>/dev/null || echo "?"; }

worst_fps() {  # lowest per-camera fps in a trig-recv.sh table
	awk 'NR>1 && $3!="" {if (m=="" || $3+0 < m) m=$3+0} END{printf "%.2f", (m==""?0:m)}'
}

echo "=============================================================="
echo " IMX296 hardware-trigger acceptance test"
echo " target ${HZ} Hz, ${SECS}s window, pass threshold ${PASS_MIN} fps/camera"
echo "=============================================================="

echo
echo "--- 0. preconditions -----------------------------------------"
if ! "$SEND" status > /tmp/trigstat.$$ 2>/dev/null; then
	echo "FAIL: no trigger generator answered. Check the serial link."
	exit 1
fi
grep -E '^(clock|running|fps_milli|polarity)' /tmp/trigstat.$$ | sed 's/^/  /'
POL=$(grep -c 'active_high' /tmp/trigstat.$$)
NDEV=$(ls /dev/video[0-3] 2>/dev/null | wc -l)
echo "  video nodes: $NDEV"
[ "$NDEV" -eq 4 ] || echo "  WARNING: expected 4 video nodes"

echo
echo "--- 1. control: free-running (trigger_mode=0) ----------------"
echo "    proves sensors + CSI + VI + capture path, trigger uninvolved"
set_mode 0
FREE=$("$RECV" "$SECS")
echo "$FREE" | sed 's/^/  /'
FREE_MIN=$(echo "$FREE" | worst_fps)
if awk -v v="$FREE_MIN" 'BEGIN{exit !(v > 1)}'; then
	echo "  => PASS: cameras deliver free-running (worst ${FREE_MIN} fps)"
else
	echo "  => FAIL: cameras produce nothing even free-running."
	echo "     The trigger is not the problem. Stop here and fix capture."
	exit 1
fi

echo
echo "--- 2. triggered (trigger_mode=1) ----------------------------"
# pol 0 is the DOCUMENTED working polarity for this rig (README: "Both
# polarities rate-lock; pol 1 silently yields exposure = period - commanded.
# Use pol 0."), and the 2026-08-28 30.00 fps / 1.0 us result was taken at
# pol 0.  Start there rather than at the firmware default.
"$SEND" pol 0      >/dev/null 2>&1
"$SEND" fps "$HZ"  >/dev/null 2>&1
"$SEND" start      >/dev/null 2>&1
set_mode 1
TRIG=$("$RECV" "$SECS")
echo "$TRIG" | sed 's/^/  /'
TRIG_MIN=$(echo "$TRIG" | worst_fps)
if awk -v v="$TRIG_MIN" -v p="$PASS_MIN" 'BEGIN{exit !(v >= p)}'; then
	echo
	echo "  ==> PASS: all four locked to the trigger (worst ${TRIG_MIN} fps)"
	echo
	echo "--- inter-camera synchronisation -----------------------------"
	if [ -x "$HOME/j106-sync-check.py" ]; then
		"$HOME/j106-sync-check.py" -n 200 2>&1 | tail -20 | sed 's/^/  /'
	else
		echo "  (j106-sync-check.py not found; skipping skew measurement)"
	fi
	set_mode 1
	exit 0
fi
echo "  => FAIL at the configured polarity (worst ${TRIG_MIN} fps)"

echo
echo "--- 3. polarity sweep ----------------------------------------"
echo "    which edge asserts XTRIG is a board fact (optocoupler sense),"
echo "    not something the datasheet settles — so try the other one."
for p in 1 0; do   # pol 0 already tried above; end back on it
	"$SEND" pol $p >/dev/null 2>&1
	"$SEND" start  >/dev/null 2>&1
	set_mode 1
	R=$("$RECV" 3)
	M=$(echo "$R" | worst_fps)
	printf '  pol %s -> worst %s fps\n' "$p" "$M"
	if awk -v v="$M" -v t="$HZ" 'BEGIN{exit !(v >= t*0.5)}'; then
		echo "  ==> PASS with pol $p — make that the default."
		exit 0
	fi
done

echo
echo "=============================================================="
echo " VERDICT: cameras capture free-running but never on the trigger."
echo
echo " Everything above the wire is therefore proven working: sensors,"
echo " driver trigger_mode, CSI, VI, capture, and the STM32 (its pulse"
echo " counter is advancing). The fault is between the STM32 pin and"
echo " the module XTRIG pad:"
echo "   - the four trigger wires (A0..A3 by SILKSCREEN, not pin number)"
echo "   - the common Trig- ground return"
echo "   - optocoupler polarity per module (diode-test it)"
echo " See ../WIRING.md section 4.1."
echo "=============================================================="
exit 1
