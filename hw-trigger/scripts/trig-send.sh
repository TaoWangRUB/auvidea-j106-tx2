#!/bin/bash
# trig-send.sh — the SENDER half: drive the STM32 trigger generator.
#
# Thin wrapper over j106-trigctl.py that picks a working port by itself, so the
# receiver side and the acceptance test do not each re-implement that choice.
#
# Port selection order, and why:
#   /dev/ttyTHS1  M110 J22 UART2 — the production link, present with no USB
#   /dev/ttyACM0  USB CDC        — convenient, but ModemManager probes it
#   /dev/ttyUSB0  USB-TTL dongle — bench fallback
#
# Usage:
#   ./trig-send.sh status
#   ./trig-send.sh fps 30
#   ./trig-send.sh exp 5000
#   ./trig-send.sh pol 0|1
#   ./trig-send.sh start | stop
#   ./trig-send.sh raw "burst 10"

set -u
TRIGCTL=${TRIGCTL:-$(command -v j106-trigctl.py || echo "$HOME/j106-trigctl.py")}

pick_port() {
	if [ -n "${TRIG_PORT:-}" ]; then echo "$TRIG_PORT"; return; fi
	for p in /dev/ttyTHS1 /dev/ttyACM0 /dev/ttyUSB0; do
		[ -e "$p" ] || continue
		# A port that exists is not necessarily answering: ttyTHS* always
		# exist on a Tegra whether anything is wired to them or not.
		if timeout 8 "$TRIGCTL" --port "$p" status 2>/dev/null | grep -q '^clock='; then
			echo "$p"; return
		fi
	done
	return 1
}

PORT=$(pick_port) || { echo "trig-send: no trigger generator answered on ttyTHS1/ttyACM0/ttyUSB0" >&2; exit 1; }
[ "${TRIG_QUIET:-0}" = 1 ] || echo "# trigger on $PORT" >&2

# j106-trigctl.py exposes only: status start stop help fps exposure burst raw.
# `pol`, `skew` and `period` are firmware commands with no subcommand of their
# own, so they go through `raw`.  Mapping them here keeps every caller from
# having to know which is which.
CMD=${1:-status}; shift || true
case "$CMD" in
  exp)             timeout 15 "$TRIGCTL" --port "$PORT" exposure "$@" ;;
  pol|skew|period) timeout 15 "$TRIGCTL" --port "$PORT" raw "$CMD $*" ;;
  raw)             timeout 15 "$TRIGCTL" --port "$PORT" raw "$*" ;;
  port)            echo "$PORT" ;;
  *)               timeout 15 "$TRIGCTL" --port "$PORT" "$CMD" "$@" ;;
esac
