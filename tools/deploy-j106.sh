#!/usr/bin/env bash
# deploy-j106.sh — push ALL Auvidea J106/TX2 carrier fixes to a running board, reversibly.
#
# Applies in one shot (no full reflash; reversible via extlinux LABELs):
#   1. patched kernel Image     (shared-reset + 680 Mbps, patch 0001)
#   2. carrier DTB              (6 cameras w/ unique positions + USB VBUS fix)
#   3. extlinux boot entry      (new LABEL, set DEFAULT; prior kept as fallback)
#   4. Arducam camera_overrides.isp -> /var/nvidia/nvcam/settings/  (ISP image-quality fix)
#   5. recovery-button handler  -> systemd service (M110 recovery btn -> RCM)
#
# Usage:
#   ./deploy-j106.sh <Image> <carrier.dtb>
#
# Env (override as needed):
#   TARGET   board ssh target           (default: nvidia@10.42.0.157)
#   SSHOPTS  extra ssh opts             (e.g. -o ProxyJump=user@jump-host)
#   SUDOPW   board sudo password        (default: nvidia)
#   LABEL    extlinux label / suffix    (default: j106)
#
# NOTE on this project's specific access (jump host needs a password): the simplest
# path is to run this FROM the jump host (where the board is directly reachable), or
# set up key-based ssh / an ~/.ssh/config ProxyJump entry first. For a full reflash
# instead of this live deploy, bake the .isp into the rootfs before flash.sh — see
# README §6.
set -euo pipefail
IMAGE=${1:?usage: deploy-j106.sh <Image> <carrier.dtb>}
DTB=${2:?usage: deploy-j106.sh <Image> <carrier.dtb>}
TARGET=${TARGET:-nvidia@10.42.0.157}
SSHOPTS=${SSHOPTS:-}
SUDOPW=${SUDOPW:-nvidia}
LABEL=${LABEL:-j106}
HERE=$(cd "$(dirname "$0")" && pwd)

put(){ ssh $SSHOPTS "$TARGET" "cat > /tmp/$2" < "$1" && echo "   pushed $2"; }

echo ">> pushing artifacts to $TARGET ..."
put "$IMAGE"                                    "Image.$LABEL"
put "$DTB"                                      "$LABEL.dtb"
put "$HERE/nvcam-settings/camera_overrides.isp" "camera_overrides.isp"
put "$HERE/j106-recovery-key.py"                "j106-recovery-key.py"
put "$HERE/j106-recovery-key.service"           "j106-recovery-key.service"
put "$HERE/j106-install.sh"                     "j106-install.sh"

echo ">> installing on board (sudo) ..."
ssh $SSHOPTS "$TARGET" "echo '$SUDOPW' | sudo -S bash /tmp/j106-install.sh '$LABEL'"

echo ">> done. Reboot to apply:"
echo "     ssh $SSHOPTS $TARGET \"echo '$SUDOPW' | sudo -S reboot\""
