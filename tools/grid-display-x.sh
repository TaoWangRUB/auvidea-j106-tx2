#!/bin/bash
# grid-display-x.sh — live 2x3 Argus grid into the X desktop (nv3dsink).
# Under a running Xorg session the NVIDIA driver owns the display, so the
# legacy nvoverlaysink can't acquire the overlay plane (pipeline never plays).
# nv3dsink renders into an X window. The queue before each compositor sink
# fixes nvcompositor's live-source latency negotiation
# ("Impossible to configure latency: max 0 < min 0.033").
# Layout (sensor-id):  [0][1][2] / [3][4][5]   Runs until killed.
set -u
export PATH=/usr/bin:/bin:/usr/local/bin:/usr/sbin:/sbin
export DISPLAY=:0 XAUTHORITY=/run/user/1000/gdm/Xauthority
NCAM=$(ls /dev/video* 2>/dev/null | wc -l)
echo "[grid-display-x] cameras=$NCAM"
xs=(0 640 1280 0 640 1280); ys=(0 0 0 360 360 360)
A=( nvcompositor name=comp )
for N in 0 1 2 3 4 5; do
  A+=( sink_${N}::xpos=${xs[$N]} sink_${N}::ypos=${ys[$N]} sink_${N}::width=640 sink_${N}::height=360 )
done
A+=( '!' 'video/x-raw(memory:NVMM),width=1920,height=720' '!' nv3dsink sync=false )
for N in 0 1 2 3 4 5; do
  if [ "$N" -lt "$NCAM" ]; then
    A+=( nvarguscamerasrc sensor-id=$N
         '!' 'video/x-raw(memory:NVMM),width=640,height=360,framerate=30/1'
         '!' nvvidconv flip-method=2 '!' queue '!' comp.sink_$N )
  else
    A+=( videotestsrc is-live=true pattern=2
         '!' 'video/x-raw,width=640,height=360,framerate=30/1'
         '!' textoverlay text="CAM $N - none" valignment=center halignment=center font-desc="Sans Bold 28"
         '!' nvvidconv '!' 'video/x-raw(memory:NVMM),width=640,height=360' '!' queue '!' comp.sink_$N )
  fi
done
exec gst-launch-1.0 "${A[@]}"
