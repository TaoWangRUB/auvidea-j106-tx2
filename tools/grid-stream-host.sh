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
BITRATE=${BITRATE:-8000000}

xs=(0 640 1280 0 640 1280); ys=(0 0 0 360 360 360)
CELL_PORT=(A B C D E F)

# i2c devname (last token of the v4l2 "name") -> fixed cell index
cell_of_dev() {
  case "$1" in
    1-0010) echo 0;; 1-0012) echo 1;;          # A B
    2-0010) echo 2;; 2-0012) echo 3;;          # C D
    7-0010) echo 4;; 7-0012|7-0018) echo 5;;   # E F (F: imx219 @0x12 or imx296 @0x18)
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
echo "[grid-stream-host] streaming 1920x720 H.264 to ${HOST}:${PORT}"

A=( nvcompositor name=comp )
for N in 0 1 2 3 4 5; do
  A+=( sink_${N}::xpos=${xs[$N]} sink_${N}::ypos=${ys[$N]} sink_${N}::width=640 sink_${N}::height=360 )
done
# nvcompositor's src pad is RGBA-only, but nvv4l2h264enc needs NV12, so convert
# between them. The queues either side of nvvidconv matter: without them the
# pipeline either refuses to link or dies at runtime with
# "nvbuffer_composite Failed".
A+=( '!' 'video/x-raw(memory:NVMM),width=1920,height=720'
     '!' queue '!' nvvidconv '!' 'video/x-raw(memory:NVMM),format=NV12' '!' queue
     '!' nvv4l2h264enc insert-sps-pps=1 idrinterval=30 bitrate=$BITRATE
     '!' h264parse '!' rtph264pay config-interval=1 pt=96
     '!' udpsink host=$HOST port=$PORT sync=false async=false )

for N in 0 1 2 3 4 5; do
  if [ -n "${CELL_SID[$N]:-}" ]; then
    # live camera -> its own port cell, no text over the image
    A+=( nvarguscamerasrc sensor-id=${CELL_SID[$N]}
         '!' 'video/x-raw(memory:NVMM),width=640,height=360,framerate=30/1'
         '!' nvvidconv flip-method=2 '!' queue '!' comp.sink_$N )
  else
    # empty port cell -> dark placeholder labelled with the port letter
    A+=( videotestsrc is-live=true pattern=2
         '!' 'video/x-raw,width=640,height=360,framerate=30/1'
         '!' textoverlay text="PORT ${CELL_PORT[$N]} - no camera" valignment=center halignment=center font-desc="Sans Bold 24"
         '!' nvvidconv '!' 'video/x-raw(memory:NVMM),width=640,height=360' '!' queue '!' comp.sink_$N )
  fi
done

exec gst-launch-1.0 "${A[@]}"
