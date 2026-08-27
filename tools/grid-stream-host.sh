#!/bin/bash
# grid-stream-host.sh — live 2x3 Argus grid from the J106, streamed to a REMOTE host.
#
# Companion to grid-display-x.sh, which renders into the board's own X desktop.
# This one encodes the composited grid to H.264 and sends it over RTP/UDP, so it
# works headless and lets a laptop on the same link watch the cameras live.
#
# Run this ON THE BOARD:
#     ./grid-stream-host.sh [host-ip] [port]        # default 10.42.0.1 5000
#
# Then ON THE HOST:
#     gst-launch-1.0 -q udpsrc port=5000 \
#       caps="application/x-rtp,media=video,encoding-name=H264,payload=96" \
#       ! rtpjitterbuffer latency=100 ! rtph264depay ! avdec_h264 \
#       ! videoconvert ! autovideosink sync=false
#
# FIXED PORT LAYOUT (each camera always lands in its own physical-port cell):
#     [A][B][C]
#     [D][E][F]
# Mapping is resolved at runtime from each /dev/videoN's i2c name, NOT from the
# Argus sensor-id order (Argus compacts sensor-ids when cameras are missing, so
# sensor-id N != port N). A camera that is absent shows a labelled placeholder.
#
#   port  i2c devname     bus            cell
#   A     imx219 1-0010   c240000 (i2c1)  0   topleft
#   B     imx219 1-0012   c240000 (i2c1)  1   topright
#   C     imx219 2-0010   3180000 (i2c2)  2   centerleft
#   D     imx219 2-0012   3180000 (i2c2)  3   centerright
#   E     imx219 7-0010   c250000 (i2c7)  4   bottomleft
#   F     imx296 7-0018   c250000 (i2c7)  5   bottomright  (was imx219 7-0012 before 2026-08-24)
#
# Runs until killed.
set -u
export PATH=/usr/bin:/bin:/usr/local/bin:/usr/sbin:/sbin

HOST=${1:-10.42.0.1}
PORT=${2:-5000}
BITRATE=${BITRATE:-12000000}

# Cells are 4:3 (480x360), matching the IMX296's 1456x1088. Using the 16:9
# cells this script started with (640x360) stretched every IMX296 frame by 33%.
xs=(0 480 960 0 480 960); ys=(0 0 0 360 360 360)
CELL_W=480; CELL_H=360
GRID_W=1440; GRID_H=720

# Argus auto-exposure needs clamping on the IMX296. Left to itself it pins BOTH
# gain (47.9 dB of a 48 dB range) and exposure to maximum and still exposes for
# the highlights, burying the image in chroma noise - the AE half of running
# IMX219 ISP tuning on an IMX296. Allowing a long exposure while capping gain
# gives a clean picture. Override for darker scenes, e.g.
#   ARGUS_GAIN='1 16' ARGUS_DGAIN='1 4'   (brighter, noisier)
# Each value is ONE argv element with a space inside it - gst-launch needs
# exposuretimerange=<min> <max> as a single argument, so these cannot be built
# by word-splitting a single string.
ARGUS_EXP=${ARGUS_EXP:-"8000000 16500000"}
ARGUS_GAIN=${ARGUS_GAIN:-"1 8"}
ARGUS_DGAIN=${ARGUS_DGAIN:-"1 2"}
ARGUS_AE_ARR=( "exposuretimerange=$ARGUS_EXP" "gainrange=$ARGUS_GAIN" "ispdigitalgainrange=$ARGUS_DGAIN" )
CELL_PORT=(A B C D E F)

# i2c devname (last token of the v4l2 "name") -> fixed cell index
cell_of_dev() {
  case "$1" in
    1-0010) echo 0;; 1-0012) echo 1;;          # A B
    2-0010|2-001a) echo 2;; 2-0012|2-0018) echo 3;;   # C D (imx219 0x10/0x12 or imx296 0x1a/0x18)
    7-0010|7-001a) echo 4;; 7-0012|7-0018) echo 5;;   # E F (   "     "        "        "       )
    *)       echo -1;;
  esac
}

# Resolve cell -> Argus sensor-id by walking /dev/video* in order (the Nth
# /dev/video == Argus sensor-id N), reading each one's i2c devname.
declare -A CELL_SID
SID=0
for V in $(ls -v /dev/video* 2>/dev/null); do
  NAME=$(cat /sys/class/video4linux/$(basename "$V")/name 2>/dev/null)  # e.g. "imx296 7-0018"
  DEV=${NAME##* }                                                       # -> 7-0018
  C=$(cell_of_dev "$DEV")
  if [ "$C" -ge 0 ] 2>/dev/null; then
    CELL_SID[$C]=$SID
    echo "[grid-stream-host] $V = $NAME -> port ${CELL_PORT[$C]} (cell $C) = sensor-id $SID"
  fi
  SID=$((SID+1))
done
echo "[grid-stream-host] cameras present: ${#CELL_SID[@]} / 6"
if [ -n "${SINK:-}" ]; then
  echo "[grid-stream-host] recording ${GRID_W}x${GRID_H} H.264 -> ${SINK}"
else
  echo "[grid-stream-host] streaming ${GRID_W}x${GRID_H} H.264 to ${HOST}:${PORT}"
fi
echo "[grid-stream-host] argus AE: exp=[$ARGUS_EXP] gain=[$ARGUS_GAIN] dgain=[$ARGUS_DGAIN]"

A=( nvcompositor name=comp )
for N in 0 1 2 3 4 5; do
  A+=( sink_${N}::xpos=${xs[$N]} sink_${N}::ypos=${ys[$N]} sink_${N}::width=${CELL_W} sink_${N}::height=${CELL_H} )
done
# nvcompositor's src pad is RGBA-only, but nvv4l2h264enc needs NV12, so convert
# between them. The queues either side of nvvidconv matter: without them the
# pipeline either refuses to link or dies at runtime with
# "nvbuffer_composite Failed".
# Where the composited, hardware-encoded grid goes. Default is RTP to the host.
# Override to record ON THE BOARD at the end of the hardware pipeline - no network,
# no host decode/re-encode, and branches aligned to the on-board transport (~0.7 ms)
# instead of the host's 100 ms jitterbuffers:
#   SINK="h264parse ! mp4mux ! filesink location=/tmp/grid.mp4" ./grid-stream-host.sh
# Stop it with SIGINT (not SIGKILL) so mp4mux writes its moov atom.
SINK_SPEC=${SINK:-"h264parse ! rtph264pay config-interval=1 pt=96 ! udpsink host=$HOST port=$PORT sync=false async=false"}

A+=( '!' "video/x-raw(memory:NVMM),width=${GRID_W},height=${GRID_H}"
     '!' queue '!' nvvidconv '!' 'video/x-raw(memory:NVMM),format=NV12' '!' queue
     '!' nvv4l2h264enc insert-sps-pps=1 idrinterval=30 bitrate=$BITRATE )
# shellcheck disable=SC2206
A+=( '!' ${SINK_SPEC} )

for N in 0 1 2 3 4 5; do
  if [ -n "${CELL_SID[$N]:-}" ]; then
    # live camera -> its own port cell, no text over the image
    A+=( nvarguscamerasrc sensor-id=${CELL_SID[$N]} "${ARGUS_AE_ARR[@]}"
         '!' "video/x-raw(memory:NVMM),width=${CELL_W},height=${CELL_H},framerate=30/1"
         '!' nvvidconv flip-method=2 '!' queue '!' comp.sink_$N )
  else
    # empty port cell -> dark placeholder labelled with the port letter
    A+=( videotestsrc is-live=true pattern=2
         '!' "video/x-raw,width=${CELL_W},height=${CELL_H},framerate=30/1"
         '!' textoverlay text="PORT ${CELL_PORT[$N]} - no camera" valignment=center halignment=center font-desc="Sans Bold 24"
         '!' nvvidconv '!' "video/x-raw(memory:NVMM),width=${CELL_W},height=${CELL_H}" '!' queue '!' comp.sink_$N )
  fi
done

exec gst-launch-1.0 -e "${A[@]}"
