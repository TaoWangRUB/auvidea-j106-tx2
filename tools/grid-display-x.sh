#!/bin/bash
# grid-display-x.sh — live 2x3 Argus grid into the X desktop (nv3dsink).
#
# Under a running Xorg session the NVIDIA driver owns the display, so the
# legacy nvoverlaysink can't acquire the overlay plane (pipeline never plays).
# nv3dsink renders into an X window. The queue before each compositor sink
# fixes nvcompositor's live-source latency negotiation
# ("Impossible to configure latency: max 0 < min 0.033").
#
# FIXED PORT LAYOUT (each camera always lands in its own physical-port cell):
#     [A][B][C]
#     [D][E][F]
# Mapping is resolved at runtime from each /dev/videoN's i2c name, NOT from the
# Argus sensor-id order (Argus compacts sensor-ids when cameras are missing, so
# sensor-id N != port N). A camera that is absent shows a labelled placeholder;
# NO text is drawn over a live feed.
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
export DISPLAY=:0 XAUTHORITY=/run/user/1000/gdm/Xauthority

xs=(0 640 1280 0 640 1280); ys=(0 0 0 360 360 360)
CELL_PORT=(A B C D E F)

# i2c devname (last token of the v4l2 "name") -> fixed cell index
cell_of_dev() {
  case "$1" in
    1-0010) echo 0;; 1-0012) echo 1;;   # A B
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
  NAME=$(cat /sys/class/video4linux/$(basename "$V")/name 2>/dev/null)  # e.g. "imx219 2-0010"
  DEV=${NAME##* }                                                       # -> 2-0010
  C=$(cell_of_dev "$DEV")
  if [ "$C" -ge 0 ] 2>/dev/null; then
    CELL_SID[$C]=$SID
    echo "[grid-display-x] $V = $NAME -> port ${CELL_PORT[$C]} (cell $C) = sensor-id $SID"
  fi
  SID=$((SID+1))
done
echo "[grid-display-x] cameras present: ${#CELL_SID[@]} / 6"

A=( nvcompositor name=comp )
for N in 0 1 2 3 4 5; do
  A+=( sink_${N}::xpos=${xs[$N]} sink_${N}::ypos=${ys[$N]} sink_${N}::width=640 sink_${N}::height=360 )
done
A+=( '!' 'video/x-raw(memory:NVMM),width=1920,height=720' '!' nv3dsink sync=false )

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
