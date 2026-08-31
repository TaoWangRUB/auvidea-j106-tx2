#!/usr/bin/env bash
# Receive the four IMX296 H.264 / RTP / UDP streams from the TX2 (sender.sh) and
# show them as a 2x2 grid laid out BY PORT.  RUN ON THE HOST.
#
#   ./receiver.sh
#
# Layout (J106 ports):   [c][d]     c=5002  d=5003
#                        [e][f]     e=5004  f=5005
#
# Every port has its own UDP port, and a camera that is not streaming shows a
# "port X (no signal)" placeholder rather than stalling the whole grid
# (compositor ignore-inactive-pads).  That distinction is the point when
# verifying a trigger: one black cell is one dead trigger wire, four black
# cells is the shared return or the generator.
#
# Needs gstreamer1.0 + gst-libav (avdec_h264).  Ctrl-C to stop.
#
# --- reading the result ---------------------------------------------------
# With trigger_mode=1 the cameras expose ONLY on STM32 edges, so:
#   all four live and smooth   -> the hardware trigger works end to end
#   all four black             -> nothing is reaching any sensor: suspect the
#                                 common Trig- return, or the generator stopped
#   some black                 -> those specific trigger wires
# FPS=1 ./receiver.sh prints the measured per-cell rate to stderr instead of
# making you judge smoothness by eye.

CW=${CW:-480}; CH=${CH:-360}                   # per-cell size

# Kernel socket receive buffer per stream.  This is the one that matters: four
# simultaneous I-frames arrive faster than the decoders drain them, and anything
# past the buffer is dropped by the kernel silently.  Requires net.core.rmem_max
# to be at least this large -- run ./net-tune.sh on this host first.
RCVBUF=${RCVBUF:-8388608}
# rtpjitterbuffer trades latency for tolerance of reordering/jitter.  100 ms is
# comfortable on a direct cable; drop it for a livelier preview, raise it if the
# link is wifi.
JITTER=${JITTER:-100}
CAPS="application/x-rtp,media=video,encoding-name=H264,payload=96"
declare -A UDP=( [c]=5002 [d]=5003 [e]=5004 [f]=5005 )

PORTS_ALL=( ${PORTS:-c d e f} )
N=${#PORTS_ALL[@]}
if   [ "$N" -le 2 ]; then COLS=$N; else COLS=2; fi
ROWS=$(( (N + COLS - 1) / COLS ))
CANVAS_W=$((CW*COLS)); CANVAS_H=$((CH*ROWS))

declare -A X Y
_i=0
for p in "${PORTS_ALL[@]}"; do
  X[$p]=$(( (_i % COLS) * CW ))
  Y[$p]=$(( (_i / COLS) * CH ))
  _i=$((_i+1))
done

# sender.sh already rotates 180 in HARDWARE on the board's ISP (its FLIP=2), so
# rotating again here would put the image back upside down — and cost host CPU
# per camera doing it.  Override only if the sender runs with FLIP=0.
RXFLIP="${RXFLIP:-none}"

# FPS=1 inserts a fpsdisplaysink-style probe per cell.  Off by default because
# it adds text overlay churn to every frame.
FPSTEXT=""
[ "${FPS:-0}" = 1 ] && FPSTEXT="! fpsdisplaysink video-sink=fakesink text-overlay=false signal-fps-measurements=true"

PROPS=""; BRANCHES=""; i=0
# bottom layer: a labelled placeholder per cell, so a dead cell is unmistakable
for p in "${PORTS_ALL[@]}"; do
  PROPS="$PROPS sink_${i}::xpos=${X[$p]} sink_${i}::ypos=${Y[$p]} sink_${i}::width=$CW sink_${i}::height=$CH"
  BRANCHES="$BRANCHES videotestsrc is-live=true pattern=2 ! video/x-raw,width=$CW,height=$CH,framerate=30/1 !"
  BRANCHES="$BRANCHES textoverlay text=\"port $p (no signal)\" valignment=center halignment=center font-desc=\"Sans 13\" ! comp.sink_${i}"
  i=$((i+1))
done
# top layer: the live cameras, overlaid on their cell
for p in "${PORTS_ALL[@]}"; do
  PROPS="$PROPS sink_${i}::xpos=${X[$p]} sink_${i}::ypos=${Y[$p]} sink_${i}::width=$CW sink_${i}::height=$CH"
  BRANCHES="$BRANCHES udpsrc port=${UDP[$p]} caps=$CAPS buffer-size=$RCVBUF ! rtpjitterbuffer latency=$JITTER ! rtph264depay ! avdec_h264 !"
  BRANCHES="$BRANCHES videoflip method=$RXFLIP ! videoconvert ! videoscale ! video/x-raw,width=$CW,height=$CH !"
  BRANCHES="$BRANCHES textoverlay text=\"port $p\" valignment=top halignment=left font-desc=\"Sans Bold 16\" shaded-background=true ! comp.sink_${i}"
  i=$((i+1))
done

SINK="${SINK:-autovideosink sync=false}"          # SINK=fakesink for a headless decode check
CMD="gst-launch-1.0 -e compositor name=comp ignore-inactive-pads=true $PROPS ! \
  video/x-raw,width=$CANVAS_W,height=$CANVAS_H ! videoconvert ! $SINK \
  $BRANCHES"

echo "${COLS}x${ROWS} port grid (${PORTS_ALL[*]}); a dead camera shows 'no signal'. Ctrl-C to stop."
eval exec "$CMD"
