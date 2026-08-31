#!/usr/bin/env bash
# net-tune.sh — size the UDP path for the 4-camera stream.  Run on BOTH ends.
#
#   ./net-tune.sh            # apply for this boot
#   ./net-tune.sh --persist  # also write /etc/sysctl.d/99-j106-stream.conf
#   ./net-tune.sh --show     # just report the current values
#   ./net-tune.sh --jumbo    # additionally set MTU 9000 (see the warning below)
#
# WHAT ACTUALLY MATTERS HERE, AND WHAT DOES NOT
#
# The link is gigabit and the stream is ~4 Mbit/s per camera — 16 Mbit/s for
# four, about 1.6% of the wire.  Throughput is not the constraint and no amount
# of tuning makes it faster.  What does bite is BURSTINESS: H.264 I-frames are
# many times the size of a P-frame, and with iframeinterval=15 all four cameras
# can emit one in the same millisecond.  That burst arrives faster than the
# receiver drains it, and if the kernel's UDP receive buffer is smaller than the
# burst the surplus is dropped — silently, showing up as smeared or blocky video
# rather than as an error.  The default 212992 bytes is roughly one 1080p
# I-frame; four at once overrun it.
#
# So the useful knobs are buffer sizes, not bandwidth:
#   rmem_max / rmem_default   receiver side, absorbs the I-frame burst
#   wmem_max / wmem_default   sender side, lets udpsink hand off without stalling
#   netdev_max_backlog        packets the NIC may queue before the stack drains
#
# `rtph264pay mtu` already defaults to 1400, which is under a 1500-byte MTU, so
# there is no IP fragmentation to eliminate — a common thing to "fix" that is
# not actually broken here.

set -u
RMEM=${RMEM:-16777216}          # 16 MB
WMEM=${WMEM:-16777216}
RDEF=${RDEF:-1048576}           # 1 MB default, not just the max
WDEF=${WDEF:-1048576}
BACKLOG=${BACKLOG:-5000}

SYSCTLS=(
  "net.core.rmem_max=$RMEM"
  "net.core.wmem_max=$WMEM"
  "net.core.rmem_default=$RDEF"
  "net.core.wmem_default=$WDEF"
  "net.core.netdev_max_backlog=$BACKLOG"
)

show() {
  echo "current:"
  for k in net.core.rmem_max net.core.wmem_max net.core.rmem_default \
           net.core.wmem_default net.core.netdev_max_backlog; do
    printf '  %-32s %s\n' "$k" "$(sysctl -n $k 2>/dev/null)"
  done
  echo "UDP errors since boot:"
  netstat -su 2>/dev/null | grep -iE 'receive buffer errors|send buffer errors|packet receive errors' | sed 's/^/  /' \
    || echo "  (netstat unavailable)"
}

case "${1:-apply}" in
--show) show; exit 0 ;;
esac

SUDO=""; [ "$(id -u)" -ne 0 ] && SUDO="sudo"
echo "applying:"
for s in "${SYSCTLS[@]}"; do
  $SUDO sysctl -w "$s" >/dev/null && echo "  $s"
done

if [ "${1:-}" = "--persist" ]; then
  printf '%s\n' "# J106 4-camera UDP stream — see hw-trigger/scripts/net-tune.sh" \
    "${SYSCTLS[@]}" | $SUDO tee /etc/sysctl.d/99-j106-stream.conf >/dev/null
  echo "persisted to /etc/sysctl.d/99-j106-stream.conf"
fi

if [ "${1:-}" = "--jumbo" ]; then
  # ⚠ Both ends AND anything between them must agree. A mismatch does not fail
  # loudly: small packets keep flowing and large ones vanish, which looks like
  # random corruption. Only worth it on a direct cable, and only if both NICs
  # accept it — this host's USB NIC may not.
  IF=${IF:-$(ip -br addr | awk '/10\.42\.0\./{print $1; exit}')}
  echo "setting MTU 9000 on $IF (revert: sudo ip link set dev $IF mtu 1500)"
  $SUDO ip link set dev "$IF" mtu 9000 && echo "  ok" || echo "  REJECTED — leave at 1500"
fi

echo
show
