#!/usr/bin/env python3
"""j106-camera-config.py — one command to put a given camera population on the board.

  >>> RUN THIS ON THE x86-64 BUILD HOST (this repo's machine), NOT ON THE TX2. <<<

  It cross-builds the DTB here and reaches the board over SSH (--target, default
  nvidia@10.42.0.157) to deploy, reboot and verify. It needs cpp, dtc and the
  j106build/ tree (stock-c03.dts + the kernel include tree), none of which exist
  on the board. Contrast with tools/j106-detect-cameras.sh, which is the opposite:
  that one runs ON the board. This script's --detect ships it there for you.

Tegra binds sensors from a static device tree, so each CSI port's sensor family is
fixed by the DTB (see README §6.7). This wraps the whole procedure: describe the
population you want, and it generates the dtsi, builds the DTB, deploys it with its
own extlinux LABEL, installs the matching ISP tuning, reboots and verifies.

If a DTB for the requested population already exists it is reused, so a repeat
configuration is just a LABEL selection and a reboot -- no rebuild.

  # ports C,D,E,F are IMX296; everything else IMX219
  ./j106-camera-config.py --imx296 C,D,E,F

  # build only, do not touch the board
  ./j106-camera-config.py --imx296 C,D,E,F --no-deploy

  # what is fitted right now?
  ./j106-camera-config.py --detect

Ports A..F map to fixed buses and CSI channels; the carrier's address shifter puts
the south sensor of each pair at native^2:

  port  bus              csi  imx219   imx296
  A     i2c@c240000 (1)   0   0x10     0x1a
  B     i2c@c240000 (1)   1   0x12     0x18
  C     i2c@3180000 (2)   2   0x10     0x1a
  D     i2c@3180000 (2)   3   0x12     0x18
  E     i2c@c250000 (7)   4   0x10     0x1a
  F     i2c@c250000 (7)   5   0x12     0x18
"""
import argparse, os, re, subprocess, sys, shlex

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(HERE)
DTSI = os.path.join(REPO, "tx2-j106-6csi", "tegra186-camera-j106-imx219.dtsi")
BUILD = os.path.join(REPO, "j106build")
KSRC = os.path.join(BUILD, "r3276", "ksrc")

# port -> (bus node, bus number, csi index, north/south, argus position)
PORTS = {
    "A": ("c240000", 1, 0, "north", "topleft"),
    "B": ("c240000", 1, 1, "south", "topright"),
    "C": ("3180000", 2, 2, "north", "centerleft"),
    "D": ("3180000", 2, 3, "south", "centerright"),
    "E": ("c250000", 7, 4, "north", "bottomleft"),
    "F": ("c250000", 7, 5, "south", "bottomright"),
}
ADDR = {("imx219", "north"): "10", ("imx219", "south"): "12",
        ("imx296", "north"): "1a", ("imx296", "south"): "18"}
LANES = {"imx219": 2, "imx296": 1}


def sh(cmd, **kw):
    return subprocess.run(cmd, shell=isinstance(cmd, str), check=False,
                          capture_output=True, text=True, **kw)


def sensor_node(port, fam):
    """Emit the DT sensor node for one port."""
    bus, busno, csi, pos, _ = PORTS[port]
    addr = ADDR[(fam, pos)]
    lo = port.lower()
    if fam == "imx296":
        modes = '\t\t\tmode0 { IMX296_MODE0("serial_%s") };\n' % lo
    else:
        modes = "".join('\t\t\tmode%d { IMX219_MODE%d("serial_%s") };\n' % (i, i, lo)
                        for i in range(5))
    return (
        '\t\t%s_%s@%s {\n'
        '\t\t\t%s_HW_RESOURCES\n'
        '\t\t\treg = <0x%s>;\n'
        '%s'
        '\t\t\tports {\n'
        '\t\t\t\t#address-cells = <1>;\n'
        '\t\t\t\t#size-cells = <0>;\n'
        '\t\t\t\tport@0 {\n'
        '\t\t\t\t\treg = <0>;\n'
        '\t\t\t\t\tj106_%s_out%d: endpoint {\n'
        '\t\t\t\t\t\tport-index = <%d>;\n'
        '\t\t\t\t\t\tbus-width = <%d>;\n'
        '\t\t\t\t\t\tremote-endpoint = <&j106_csi_in%d>;\n'
        '\t\t\t\t\t};\n'
        '\t\t\t\t};\n'
        '\t\t\t};\n'
        '\t\t};'
    ) % (fam, lo, addr, fam.upper(), addr, modes, fam, csi, csi, LANES[fam], csi)


def generate(base, layout):
    """Rewrite the base dtsi for the requested per-port families."""
    s = base
    for port, fam in layout.items():
        bus, busno, csi, pos, apos = PORTS[port]
        lo = port.lower()
        # 1) sensor node: replace whichever family is currently there
        pat = re.compile(r'\t\t(imx219|imx296)_%s@[0-9a-f]+ \{.*?\n\t\t\};' % lo, re.S)
        if not pat.search(s):
            raise SystemExit("could not locate the sensor node for port %s" % port)
        s = pat.sub(lambda m: sensor_node(port, fam).replace('\\', '\\\\'), s, count=1)
        # 2) nvcsi input endpoint: lane width + which sensor label it points at
        s = re.sub(
            r'(j106_csi_in%d: endpoint@\d+ \{\s*\n\s*port-index = <%d>;\s*\n\s*bus-width = )<\d>;(.*?remote-endpoint = <&)j106_imx\d+_out%d(>;)' % (csi, csi, csi),
            lambda m: "%s<%d>;%sj106_%s_out%d%s" % (m.group(1), LANES[fam], m.group(2), fam, csi, m.group(3)),
            s, count=1, flags=re.S)
        # 3) vi input endpoint: lane width
        s = re.sub(r'(j106_vi_in%d: endpoint \{\s*\n\s*port-index = <%d>;[^\n]*\n\s*bus-width = )<\d>;' % (csi, csi),
                   lambda m: "%s<%d>;" % (m.group(1), LANES[fam]), s, count=1)
        # 4) tegra-camera-platform entry -- MUST be scoped to this port's moduleN
        #    block. Ports share a bus (A/B, C/D, E/F), so an unscoped devname
        #    substitution rewrites the sibling port's entry instead of this one.
        mblk = re.search(r'\n\t\t\tmodule%d \{.*?\n\t\t\t\};' % csi, s, re.S)
        if not mblk:
            raise SystemExit("could not locate module%d for port %s" % (csi, port))
        blk = mblk.group(0)
        nb = re.sub(r'badge = "[^"]*";', 'badge = "%s_%s_csi%s";' % (fam, apos, lo), blk, count=1)
        nb = re.sub(r'devname = "[^"]*";', 'devname = "%s %d-00%s";' % (fam, busno, ADDR[(fam, pos)]), nb, count=1)
        nb = re.sub(r'proc-device-tree = "/proc/device-tree/i2c@[^/]*/imx\d+_[a-f]@[0-9a-f]+";',
                    'proc-device-tree = "/proc/device-tree/i2c@%s/%s_%s@%s";' % (bus, fam, lo, ADDR[(fam, pos)]),
                    nb, count=1)
        s = s[:mblk.start()] + nb + s[mblk.end():]
    # 5) lane budget
    total = sum(LANES[f] for f in layout.values())
    counts = {}
    for f in layout.values():
        counts[f] = counts.get(f, 0) + 1
    note = " + ".join("%d x %s x %d lane%s" % (n, f, LANES[f], "" if LANES[f] == 1 else "s")
                      for f, n in sorted(counts.items()))
    s = re.sub(r'/\* [^\n]*lanes total \*/\n\t\tnum_csi_lanes = <\d+>;',
               '/* %s = %d lanes total (generated) */\n\t\tnum_csi_lanes = <%d>;' % (note, total, total),
               s, count=1)
    return s, total


def build_dtb(dtsi_text, tag):
    """cpp the camera dtsi, concatenate onto the decompiled stock tree, run dtc."""
    gen = os.path.join(BUILD, "cam-%s.dtsi" % tag)
    open(gen, "w").write(dtsi_text)
    incs = " ".join("-I " + os.path.join(KSRC, p) for p in (
        "hardware/nvidia/soc/tegra/kernel-include",
        "hardware/nvidia/soc/t18x/kernel-include",
        "kernel/kernel-4.9/include"))
    pp = os.path.join(BUILD, "cam-%s.pp.dtsi" % tag)
    r = sh("cpp -nostdinc -undef -D__DTS__ -D__KERNEL__ -DLINUX_VERSION=409 "
           "-x assembler-with-cpp %s %s -o %s" % (incs, shlex.quote(gen), shlex.quote(pp)))
    if r.returncode:
        raise SystemExit("cpp failed:\n" + r.stderr)
    clean = "\n".join(l for l in open(pp).read().split("\n") if not l.startswith("#"))
    combined = os.path.join(BUILD, "combined-%s.dts" % tag)
    parts = [open(os.path.join(BUILD, "stock-c03.dts")).read(), clean]
    for extra in ("usb296.clean.dtsi", "imu296.clean.dtsi"):
        p = os.path.join(BUILD, extra)
        if os.path.exists(p):
            parts.append(open(p).read())
    open(combined, "w").write("\n".join(parts))
    dtb = os.path.join(BUILD, "tegra186-j106-%s.dtb" % tag)
    log = os.path.join(BUILD, "%s.dtc.log" % tag)
    r = sh("dtc -I dts -O dtb -@ -o %s %s 2> %s" % (shlex.quote(dtb), shlex.quote(combined), shlex.quote(log)))
    if r.returncode or not os.path.exists(dtb):
        raise SystemExit("dtc failed, see %s:\n%s" % (log, open(log).read()[-800:]))
    return dtb


def check_host():
    """Refuse to run on the target board, and explain why."""
    if os.path.exists("/proc/device-tree/compatible"):
        compat = open("/proc/device-tree/compatible", "rb").read().decode("ascii", "ignore")
        if "tegra" in compat:
            raise SystemExit(
                "This script runs on the x86-64 BUILD HOST, not on the TX2.\n"
                "It cross-builds the DTB and reaches the board over SSH.\n"
                "On the board you want tools/j106-detect-cameras.sh instead.")
    missing = [t for t in ("cpp", "dtc") if not sh(["which", t]).stdout.strip()]
    if missing:
        raise SystemExit("missing build tool(s): %s" % ", ".join(missing))
    for need in (os.path.join(BUILD, "stock-c03.dts"),
                 os.path.join(KSRC, "kernel", "kernel-4.9", "include")):
        if not os.path.exists(need):
            raise SystemExit(
                "missing %s\n"
                "This needs the j106build/ tree (see README \u00a76.1). It is git-ignored and\n"
                "built locally, so a fresh clone must create it before using this script." % need)


def main():
    ap = argparse.ArgumentParser(description="Configure the J106 camera population end to end.")
    ap.add_argument("--imx296", default="", help="comma/space separated ports that carry IMX296 (rest default to IMX219)")
    ap.add_argument("--imx219", default="", help="explicitly list IMX219 ports (optional)")
    ap.add_argument("--target", default=os.environ.get("TARGET", "nvidia@10.42.0.157"))
    ap.add_argument("--sudopw", default=os.environ.get("SUDOPW", "nvidia"))
    ap.add_argument("--no-deploy", action="store_true", help="build only")
    ap.add_argument("--no-reboot", action="store_true", help="deploy but do not reboot")
    ap.add_argument("--force-build", action="store_true", help="rebuild even if the DTB exists")
    ap.add_argument("--detect", action="store_true", help="just report what is fitted and exit")
    a = ap.parse_args()

    if not a.detect:
        check_host()

    if a.detect:
        r = sh(["ssh", "-o", "ConnectTimeout=8", a.target, "bash -s"],
               input=open(os.path.join(HERE, "j106-detect-cameras.sh")).read())
        print(r.stdout or r.stderr)
        return

    p296 = {p.strip().upper() for p in re.split(r"[ ,]+", a.imx296) if p.strip()}
    p219 = {p.strip().upper() for p in re.split(r"[ ,]+", a.imx219) if p.strip()}
    bad = (p296 | p219) - set(PORTS)
    if bad:
        raise SystemExit("unknown port(s): %s (valid: A-F)" % ",".join(sorted(bad)))
    if p296 & p219:
        raise SystemExit("port(s) listed as both families: %s" % ",".join(sorted(p296 & p219)))
    layout = {p: ("imx296" if p in p296 else "imx219") for p in PORTS}

    # Label names the population. An all-IMX219 board is the stock tree, so it
    # gets its own name rather than "cam296-none", which reads as a failed
    # IMX296 build rather than a deliberate IMX219 one.
    ports296 = "".join(p.lower() for p in sorted(PORTS) if layout[p] == "imx296")
    tag = "cam296-" + ports296 if ports296 else "cam219"
    print("Requested population")
    for p in sorted(PORTS):
        bus, busno, csi, pos, _ = PORTS[p]
        print("   port %s  i2c-%-2s csi%d   %s @0x%s  (%d lane%s)" %
              (p, busno, csi, layout[p], ADDR[(layout[p], pos)], LANES[layout[p]],
               "" if LANES[layout[p]] == 1 else "s"))
    dtb = os.path.join(BUILD, "tegra186-j106-%s.dtb" % tag)

    if os.path.exists(dtb) and not a.force_build:
        print("\n[1/4] DTB already built for this population -> reusing %s" % os.path.basename(dtb))
        total = sum(LANES[f] for f in layout.values())
    else:
        print("\n[1/4] generating dtsi + building DTB (%s)" % tag)
        text, total = generate(open(DTSI).read(), layout)
        dtb = build_dtb(text, tag)
        print("      %s (%d bytes), num_csi_lanes = %d" % (os.path.basename(dtb), os.path.getsize(dtb), total))

    if a.no_deploy:
        print("\n--no-deploy: stopping here. DTB at %s" % dtb)
        return

    # ISP tuning follows the majority family (the file is global to all sensors)
    n296 = sum(1 for f in layout.values() if f == "imx296")
    isp = "camera_overrides.imx296.isp" if n296 >= 3 else "camera_overrides.isp"
    label = tag
    print("\n[2/4] deploying to %s  (LABEL %s, ISP %s)" % (a.target, label, isp))
    for src, dst in ((dtb, "/tmp/%s.dtb" % label),
                     (os.path.join(HERE, "nvcam-settings", isp), "/tmp/camera_overrides.isp")):
        r = sh(["ssh", "-o", "ConnectTimeout=8", a.target, "cat > %s" % dst], input=open(src, "rb").read().decode("latin1"))
        if r.returncode:
            raise SystemExit("failed to copy %s: %s" % (src, r.stderr))
    remote = f"""
echo '{a.sudopw}' | sudo -S bash -c '
set -e
install -m644 /tmp/{label}.dtb /boot/{label}.dtb
mkdir -p /var/nvidia/nvcam/settings
install -m664 /tmp/camera_overrides.isp /var/nvidia/nvcam/settings/camera_overrides.isp
rm -f /var/nvidia/nvcam/settings/nvcam_cache_*.bin
C=/boot/extlinux/extlinux.conf
cp -n $C $C.bak-predeploy || true
grep -q "^LABEL {label}$" $C || cat >> $C <<LBL

LABEL {label}
      MENU LABEL J106 cameras ({label})
      LINUX /boot/Image.j106imx296
      INITRD /boot/initrd
      FDT /boot/{label}.dtb
      APPEND \\${{cbootargs}} quiet
LBL
sed -i "s/^DEFAULT .*/DEFAULT {label}/" $C
echo "      extlinux DEFAULT=$(grep ^DEFAULT $C | awk "{{print \\$2}}")"
'"""
    r = sh(["ssh", "-o", "ConnectTimeout=20", a.target, remote])
    print((r.stdout or "").rstrip() or r.stderr.rstrip())
    if r.returncode:
        raise SystemExit("deploy failed")

    if a.no_reboot:
        print("\n--no-reboot: deployed. Reboot to apply.")
        return
    print("\n[3/4] rebooting")
    sh(["ssh", "-o", "ConnectTimeout=8", a.target, "echo '%s' | sudo -S reboot" % a.sudopw])
    for _ in range(40):
        sh(["ping", "-c", "3", "-W", "2", a.target.split("@")[-1]])
        if sh(["ssh", "-o", "ConnectTimeout=5", a.target, "true"]).returncode == 0:
            break
    print("\n[4/4] verifying")
    r = sh(["ssh", "-o", "ConnectTimeout=8", a.target, "bash -s"],
           input=open(os.path.join(HERE, "j106-detect-cameras.sh")).read())
    print(r.stdout or r.stderr)
    r = sh(["ssh", "-o", "ConnectTimeout=8", a.target,
            "for v in /dev/video*; do echo \"   $v = $(cat /sys/class/video4linux/$(basename $v)/name)\"; done"])
    print(r.stdout.rstrip())


if __name__ == "__main__":
    main()
