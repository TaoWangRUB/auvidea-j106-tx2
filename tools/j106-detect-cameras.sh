#!/usr/bin/env bash
# j106-detect-cameras.sh — report which sensor family is populated on each J106 CSI port.
#
# Tegra binds sensors from a STATIC device tree, so unlike Raspberry Pi there is no
# firmware-level camera probe. This script is the manual equivalent: it probes the
# three camera i2c buses and tells you which pre-built DTB / extlinux LABEL matches
# the hardware actually fitted.
#
# Run on the board:  ./j106-detect-cameras.sh
#
# Address map (the carrier's shifter XORs address bit 1, so the south sensor of each
# pair appears at native^2):
#
#   port  bus              north (native)        south (shifted)
#   A/B   i2c@c240000 (1)  imx219 0x10           imx219 0x12
#   C/D   i2c@3180000 (2)  imx219 0x10 / imx296 0x1a   imx219 0x12 / imx296 0x18
#   E/F   i2c@c250000 (7)  imx219 0x10 / imx296 0x1a   imx219 0x12 / imx296 0x18
#
# IMX296 also answers at a SLAMODE-strapped alias (0x36 north / 0x34 south); it is the
# same die, so it is reported as an alias, not a second camera.
set -u
SUDO=${SUDO:-"sudo"}
PW=${SUDOPW:-nvidia}

probe() { echo "$PW" | $SUDO -S i2cdetect -y -r "$1" 2>/dev/null | tail -8; }

present() { # bus addr -> 0 if the address answers (either "UU" bound or a hex address)
  local want=$2
  probe "$1" | grep -qiE "(^|[ ])(${want}|UU)([ ]|$)" && return 0 || return 1
}

# cell -> "letter bus north_label south_label"
declare -A BUS=( [A]=1 [B]=1 [C]=2 [D]=2 [E]=7 [F]=7 )
declare -A POS=( [A]=north [B]=south [C]=north [D]=south [E]=north [F]=south )

echo "J106 camera population"
echo "======================"
declare -A FAMILY
for P in A B C D E F; do
  B=${BUS[$P]}
  MAP=$(probe "$B")
  if [ "${POS[$P]}" = north ]; then I219=10; I296=1a; ALIAS=36; else I219=12; I296=18; ALIAS=34; fi
  hit(){ echo "$MAP" | grep -qiE "(^|[ ])($1|UU)([ ]|$)"; }
  # distinguish by which address answers
  HAS296=$(echo "$MAP" | grep -oiE "(^|[ ])$I296([ ]|$)" | head -1)
  HAS219=$(echo "$MAP" | grep -oiE "(^|[ ])$I219([ ]|$)" | head -1)
  BOUND=$(ls /sys/class/video4linux/*/name 2>/dev/null | while read -r f; do
            n=$(cat "$f"); case "$n" in *"$B-00$I296"*) echo imx296;; *"$B-00$I219"*) echo imx219;; esac; done | head -1)
  if   [ -n "$BOUND" ];                 then FAM="$BOUND (bound)"
  elif [ -n "${HAS296// /}" ];          then FAM="imx296 (0x$I296, alias 0x$ALIAS)"
  elif [ -n "${HAS219// /}" ];          then FAM="imx219 (0x$I219)"
  else                                       FAM="-- empty --"; fi
  FAMILY[$P]=${FAM%% *}
  printf "  port %s  i2c-%-2s  %s\n" "$P" "$B" "$FAM"
done

echo
n296=0; n219=0
for P in A B C D E F; do
  case "${FAMILY[$P]}" in imx296) n296=$((n296+1));; imx219) n219=$((n219+1));; esac
done
echo "  summary: ${n219} x imx219, ${n296} x imx296"
echo "  CSI lanes needed: $((n219*2 + n296*1))   (imx219 = 2 lanes, imx296 = 1 lane)"
echo
echo "  Next: boot the DTB whose sensor nodes match the list above (extlinux LABEL),"
echo "        and install the matching ISP tuning (ISP_FILE= in deploy-j106.sh)."
echo "        The tuning file is GLOBAL - a mixed board cannot have both families correct."
