#!/usr/bin/env bash
# 4x IMX296 -> H.264 / RTP / UDP sender.  RUN ON THE TX2 (board host, not in a container).
#
# The acceptance stream for the hardware-trigger rig: all four triggered cameras
# pushed to the host so `receiver.sh` can show them as a 2x2 grid and you can
# see 30 fps on four cameras rather than infer it from a frame counter.
#
# Port -> Argus sensor-id is resolved AT RUNTIME from each /dev/videoN's i2c
# name.  Do NOT hard-code it: Argus numbers cameras in /dev/video order, which
# is bind order, not port order.  On this rig the IMX296 modules sit at 0x1a/0x18
# (the IMX219 used 0x10/0x12), so the same physical port has a different i2c
# name depending on which sensor is fitted:
#   port c = 2-001a   port d = 2-0018   port e = 7-001a   port f = 7-0018
# Each port keeps the UDP number the BEV scripts use (c=5002 ... f=5005), so
# scripts/stream/csi_receiver.sh from that tree receives this stream unchanged.
#
#   ./sender.sh [HOST_IP]     # default 10.42.0.1 = the eth direct link;
#                             # over wifi pass the host's wlan IP
#
# Argus serves ONE consumer at a time -> stop any ROS capture or docker camera
# run first.  Ctrl-C stops all.

HOST_IP="${1:-10.42.0.1}"
W=${W:-640}; H=${H:-480}; FPS=${FPS:-30}; BR=${BR:-4000000}

# Kernel socket send buffer per stream.  GStreamer's default is 0 = "whatever
# the kernel gives you", which is net.core.wmem_default (212992 on a stock
# Tegra).  Four cameras emitting an I-frame in the same millisecond overrun
# that, and the surplus is dropped in the kernel with no error anywhere --
# it surfaces only as smeared video.  Run ./net-tune.sh first so wmem_max is
# actually large enough for this request to be honoured.
SNDBUF=${SNDBUF:-4194304}
# Keep RTP payloads under the 1500-byte MTU so nothing is IP-fragmented; a
# fragmented RTP packet loses the whole frame if any fragment is dropped.
RTP_MTU=${RTP_MTU:-1400}

declare -A DEVOF=( [c]=2-001a [d]=2-0018 [e]=7-001a [f]=7-0018 )
declare -A UDP=(   [c]=5002   [d]=5003   [e]=5004   [f]=5005   )

# resolve port -> Argus sensor-id: the Nth /dev/video (numeric order) is sensor-id N
declare -A SID=(); sid=0
for V in $(ls -v /dev/video* 2>/dev/null); do
  dev=$(cat "/sys/class/video4linux/$(basename "$V")/name" 2>/dev/null)   # "vi-output, imx296 2-001a"
  dev=${dev##* }                                                          # -> 2-001a
  for p in c d e f; do [ "${DEVOF[$p]}" = "$dev" ] && SID[$p]=$sid; done
  sid=$((sid+1))
done

PORTS=()
for p in c d e f; do [ -n "${SID[$p]:-}" ] && PORTS+=("$p"); done
echo "cameras present: ${PORTS[*]:-none} (${#PORTS[@]}/4)"
[ ${#PORTS[@]} -eq 0 ] && { echo "no IMX296 cameras found — is the right DTB booted?"; exit 1; }

# Override which ports to stream:  PORTS_ONLY="c d" ./sender.sh
if [ -n "${PORTS_ONLY:-}" ]; then
  sel=(); for p in $PORTS_ONLY; do [ -n "${SID[$p]:-}" ] && sel+=("$p"); done
  PORTS=("${sel[@]}"); echo "streaming subset: ${PORTS[*]}"
fi

# Modules are mounted inverted on this rig, so rotate 180 in HARDWARE on the ISP
# rather than on the host — a host-side transpose costs CPU per camera.
FLIP=${FLIP:-2}

# --- auto-exposure MUST be clamped (openspec: imx296-camera; tools/grid-stream-host.sh)
# Left to itself Argus pins BOTH gain (47.9 dB of 48) AND exposure to maximum and
# still exposes for the highlights, burying the image in chroma noise.  Clamping
# only the gain is NOT enough: with exposure still free, AE keeps hunting and the
# picture visibly pulses (README records a 3.47 Hz limit cycle, 150 luma p2p).
# All THREE knobs are needed — long exposure ALLOWED, gain CAPPED:
#
#   exposuretimerange   ns, "<min> <max>"  — let it expose long
#   gainrange           linear multiplier  — cap it; this is the noise source
#   ispdigitalgainrange linear multiplier  — cap it too
#
# ⚠ Each value is ONE argv element containing a space; gst-launch needs
# `exposuretimerange=<min> <max>` as a single argument, so these must not be
# built by word-splitting.
#
# ⚠ UNITS: v4l2 `gain` is tenths of a dB (0..480 = 0..48 dB); Argus `gainrange`
# is a LINEAR multiplier.  "1 16" is only 24 dB and in a dim room yields a BLACK
# frame — measured here: it took ports c/e from mean 11.6/51.7 to 0.0/1.3.
#
# Defaults follow tools/grid-stream-host.sh (8-16.5 ms at 30 fps), with the
# exposure ceiling scaled up as the frame rate drops, capped at the ~66.5 ms
# Argus itself will not exceed on this sensor.
_exp_max=$(( 16500000 * 30 / FPS ))
[ "$_exp_max" -gt 66500000 ] && _exp_max=66500000
ARGUS_EXP=${ARGUS_EXP:-"8000000 $_exp_max"}
ARGUS_GAIN=${ARGUS_GAIN:-"1 8"}
ARGUS_DGAIN=${ARGUS_DGAIN:-"1 2"}

TRIGMODE=$(cat /sys/module/imx296/parameters/trigger_mode 2>/dev/null || echo 0)

# AE_LOCK=0 removes the clamp entirely and hands AE back to Argus.  Kept because
# on this rig, in a dim room at 5 fps, unclamped AE settles on ~10 dB and gives a
# visibly better picture than the 1-8 gain cap does — the cap is tuned for 30 fps
# where gain is the noise source, not for the long-exposure low-rate case.
if [ "${AE_LOCK:-auto}" = "0" ]; then
  AE_ARGS=()
  echo "argus AE: UNCLAMPED (AE_LOCK=0)"
else
  AE_ARGS=( "exposuretimerange=$ARGUS_EXP" "gainrange=$ARGUS_GAIN" "ispdigitalgainrange=$ARGUS_DGAIN" )
  echo "argus AE clamp: exp=[$ARGUS_EXP] gain=[$ARGUS_GAIN] dgain=[$ARGUS_DGAIN]"
fi

# In trigger mode exposure IS the XTRIG pulse width, so AE cannot move its main
# actuator at all and hunts purely on gain — lock it outright.
if [ "${AE_LOCK:-auto}" = "1" ] || { [ "${AE_LOCK:-auto}" = "auto" ] && [ "$TRIGMODE" = "1" ]; }; then
  AE_ARGS=( aelock=true gainrange="${AE_GAIN:-16 16}" ispdigitalgainrange="${AE_DGAIN:-4 4}" )
  echo "external trigger active -> AE LOCKED (gain ${AE_GAIN:-16 16}, dgain ${AE_DGAIN:-4 4})"
fi

if [ "$TRIGMODE" = 1 ]; then
  echo "trigger_mode=1 — frames arrive ONLY on STM32 edges."
  echo "  If the grid stays black, the trigger is not reaching the sensors:"
  echo "  run ./verify-trigger.sh to find out where it stops."
else
  echo "trigger_mode=0 — cameras free-run (sensor's own clock), trigger uninvolved."
fi

# clean any previous streamers (by process NAME, so this never matches its own command line)
killall -9 gst-launch-1.0 2>/dev/null || true
trap 'echo; echo "stopping sender..."; pkill -P $$ 2>/dev/null; exit 0' INT TERM

for p in "${PORTS[@]}"; do
  gst-launch-1.0 -q \
    nvarguscamerasrc sensor-id="${SID[$p]}" ${AE_ARGS[@]+"${AE_ARGS[@]}"} ! \
    "video/x-raw(memory:NVMM),width=$W,height=$H,framerate=$FPS/1,format=NV12" ! \
    nvvidconv flip-method=$FLIP ! "video/x-raw(memory:NVMM),format=NV12" ! \
    nvv4l2h264enc bitrate=$BR insert-sps-pps=1 iframeinterval=15 idrinterval=15 maxperf-enable=1 ! \
    h264parse ! rtph264pay config-interval=1 pt=96 mtu=$RTP_MTU ! \
    udpsink host="$HOST_IP" port="${UDP[$p]}" sync=false async=false \
            buffer-size=$SNDBUF &
  echo "  port $p (sensor-id ${SID[$p]}) -> udp $HOST_IP:${UDP[$p]}"
  sleep 3   # stagger Argus session starts; simultaneous opens lose the race and
            # a camera silently fails to stream.
done

echo "streaming ${#PORTS[@]} cameras to $HOST_IP -- run receiver.sh on the host. Ctrl-C to stop."
wait
