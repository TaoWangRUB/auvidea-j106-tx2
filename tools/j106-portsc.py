#!/usr/bin/env python3
# Dump the Tegra xHCI PORTSC registers — the only view that distinguishes
# "nothing is plugged in" from "a device is plugged in but never trains".
#
# Read-only, via /dev/mem, so it needs sudo. On the TX2 the host controller is
# at 0x3530000 (see `tegra-xusb 3530000.xhci: irq 71, io mem 0x03530000`).
#
# Port numbering on this board (7 ports = 3 SS + 4 USB2):
#   1..3  usb3-0, usb3-1, usb3-2      (SuperSpeed)
#   4     usb2-0   micro-USB OTG (J1) — the STM32 camtrig lives here
#   5     usb2-1   J2, silkscreen "USB1"
#   6     usb2-2   J3, silkscreen "USB2"
#   7     usb2-3
#
# Readings that matter:
#   CCS=0 PP=1 PLS=RxDetect   powered, polling, no far-end receiver detected
#   CCS=0 on the USB2 port too, with a device attached: the device has committed
#                             to SuperSpeed and is withholding its USB 2.0 pull-up
#   CCS=1 PED=1 PLS=U0 speed=SuperSpeed   trained
import mmap, os, struct, sys

BASE = int(sys.argv[1], 16) if len(sys.argv) > 1 else 0x3530000
PLS = {0: "U0", 1: "U1", 2: "U2", 3: "U3(susp)", 4: "Disabled", 5: "RxDetect",
       6: "Inactive", 7: "Polling", 8: "Recovery", 9: "HotReset", 10: "Compliance",
       11: "TestMode", 15: "Resume"}
SPD = {0: "-", 1: "Full", 2: "Low", 3: "High", 4: "SuperSpeed", 5: "SuperSpeed+"}

pagesz = 4096
off = BASE & ~(pagesz - 1)
delta = BASE - off

fd = os.open("/dev/mem", os.O_RDONLY | os.O_SYNC)
m = mmap.mmap(fd, delta + 0x1000, mmap.MAP_SHARED, mmap.PROT_READ, offset=off)

def rd(o):
    return struct.unpack("<I", m[delta + o:delta + o + 4])[0]

caplen = rd(0x00) & 0xff
nports = (rd(0x04) >> 24) & 0xff
print(f"xHCI @ {BASE:#x}  CAPLENGTH={caplen:#x}  MaxPorts={nports}")
print(f"{'port':>4}  {'PORTSC':>10}  CCS PED OCA  PP  {'PLS':<10} {'speed':<12}")
for i in range(nports):
    v = rd(caplen + 0x400 + 0x10 * i)
    print(f"{i + 1:>4}  {v:#010x}  {v & 1:^3} {(v >> 1) & 1:^3} {(v >> 3) & 1:^3} "
          f"{(v >> 9) & 1:^3}  {PLS.get((v >> 5) & 0xf, str((v >> 5) & 0xf)):<10} "
          f"{SPD.get((v >> 10) & 0xf, str((v >> 10) & 0xf)):<12}")
m.close()
os.close(fd)
