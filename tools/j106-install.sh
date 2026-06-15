#!/usr/bin/env bash
# j106-install.sh — board-side installer for ALL Auvidea J106/TX2 carrier fixes.
# Run as root on the target. Reversible (extlinux LABELs; prior config backed up).
#
#   arg 1: LABEL (extlinux label + artifact suffix; default "j106")
#
# Expects these already copied into /tmp (deploy-j106.sh does this):
#   /tmp/Image.<LABEL>                 patched kernel (shared-reset + 680 Mbps, patch 0001)
#   /tmp/<LABEL>.dtb                   carrier DTB (6 cams + unique positions + USB VBUS fix)
#   /tmp/camera_overrides.isp          Arducam ISP tuning (image-quality fix)
#   /tmp/j106-recovery-key.py          recovery-button handler
#   /tmp/j106-recovery-key.service     its systemd unit
set -e
LABEL=${1:-j106}
CONF=/boot/extlinux/extlinux.conf

echo "[j106-install] kernel Image + carrier DTB"
install -m644 /tmp/Image.$LABEL /boot/Image.$LABEL
install -m644 /tmp/$LABEL.dtb   /boot/$LABEL.dtb

echo "[j106-install] extlinux boot entry (label=$LABEL, reversible)"
cp -n "$CONF" "$CONF.bak-predeploy" 2>/dev/null || true
if ! grep -q "^LABEL $LABEL\$" "$CONF"; then
  cat >> "$CONF" <<LBL

LABEL $LABEL
      MENU LABEL J106 6cam carrier ($LABEL)
      LINUX /boot/Image.$LABEL
      INITRD /boot/initrd
      FDT /boot/$LABEL.dtb
      APPEND \${cbootargs} quiet
LBL
fi
sed -i "s/^DEFAULT .*/DEFAULT $LABEL/" "$CONF"

echo "[j106-install] Argus ISP override (image-quality fix)"
mkdir -p /var/nvidia/nvcam/settings
install -m664 -o root -g root /tmp/camera_overrides.isp /var/nvidia/nvcam/settings/camera_overrides.isp
rm -f /var/nvidia/nvcam/settings/nvcam_cache_*.bin   # force libargus to re-read tuning

echo "[j106-install] recovery-button handler (M110 recovery -> RCM)"
install -m755 /tmp/j106-recovery-key.py      /opt/j106-recovery-key.py
install -m644 /tmp/j106-recovery-key.service /etc/systemd/system/j106-recovery-key.service
systemctl daemon-reload
systemctl enable j106-recovery-key.service >/dev/null 2>&1 || true

echo "[j106-install] done — DEFAULT=$LABEL; ISP override + recovery service installed. Reboot to apply."
