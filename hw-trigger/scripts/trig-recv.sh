#!/bin/bash
# trig-recv.sh — the RECEIVER half: how fast is each camera actually delivering?
#
# Counts frames rather than trusting a rate the driver reports. v4l2-ctl emits
# one '<' per captured frame, so a run that times out still yields a usable
# count — which matters here, because the failure being tested for is "no
# frames at all", where any tool that waits for N frames simply hangs.
#
# Usage:  ./trig-recv.sh [seconds] [device ...]
#         ./trig-recv.sh 5                     # all four, 5 s
#         ./trig-recv.sh 10 /dev/video0

set -u
SECS=${1:-5}
shift || true
DEVS=("$@")
[ ${#DEVS[@]} -eq 0 ] && DEVS=(/dev/video0 /dev/video1 /dev/video2 /dev/video3)

TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT

for d in "${DEVS[@]}"; do
	[ -e "$d" ] || { echo "$d: MISSING"; continue; }
	(
		# stream-count is set far above what could arrive, so the timeout
		# is what ends the run and the window is exactly SECS.
		timeout "$SECS" v4l2-ctl -d "$d" --stream-mmap \
			--stream-count=100000 --stream-to=/dev/null \
			> "$TMP/$(basename "$d").out" 2>&1
	) &
done
wait

printf '%-14s %8s %9s\n' DEVICE FRAMES FPS
for d in "${DEVS[@]}"; do
	f="$TMP/$(basename "$d").out"
	[ -f "$f" ] || continue
	n=$(tr -cd '<' < "$f" | wc -c)
	fps=$(awk -v n="$n" -v s="$SECS" 'BEGIN{printf "%.2f", n/s}')
	printf '%-14s %8s %9s\n' "$d" "$n" "$fps"
done
