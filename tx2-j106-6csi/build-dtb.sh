#!/bin/bash
# Build a single Tegra186 (TX2) DTB from the local Auvidea R32.2.1 kernel_src tree.
# Usage: build.sh <input.dts> <output.dtb>
set -e
IN="$1"; OUT="$2"
if [ -z "$IN" ] || [ -z "$OUT" ]; then echo "usage: build.sh in.dts out.dtb"; exit 2; fi

KSRC=/tmp/j106build/kernel_src
HW=$KSRC/hardware/nvidia
Q=$HW/platform/t18x/quill/kernel-dts
C=$HW/platform/t18x/common/kernel-dts
S18=$HW/soc/t18x/kernel-include
STEG=$HW/soc/tegra/kernel-include
SD18=$HW/soc/t18x/kernel-dts
SDTEG=$HW/soc/tegra/kernel-dts
CTEG=$HW/platform/tegra/common/kernel-dts

INCS="-I $Q -I $C -I $S18 -I $STEG -I $SD18 -I $SDTEG -I $CTEG"
PP="${OUT%.dtb}.pp.dts"

# 1) C-preprocess (resolves #include <...> and version.h macros;
#    LINUX_VERSION=409 => kernel 4.9 => V2 DT bindings)
cpp -nostdinc -undef -D__DTS__ -D__KERNEL__ -DLINUX_VERSION=409 \
    -x assembler-with-cpp $INCS "$IN" -o "$PP"

# 2) Compile to DTB (-@ keeps __symbols__; -i dirs for any /include/ dtsi)
dtc -I dts -O dtb -@ -i "$Q" -i "$C" -i "$SD18" -i "$SDTEG" -i "$CTEG" \
    -o "$OUT" "$PP" 2> "${OUT%.dtb}.dtc.log" || { echo "DTC FAILED:"; cat "${OUT%.dtb}.dtc.log"; exit 1; }

echo "OK -> $OUT ($(stat -c%s "$OUT") bytes)"
echo "--- dtc warnings (count) ---"; wc -l < "${OUT%.dtb}.dtc.log"
