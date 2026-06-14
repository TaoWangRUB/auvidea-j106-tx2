#!/usr/bin/env python3
# J106/M110 recovery button -> software forced-recovery (RCM).
#
# On this carrier the M110 "Recovery" button is wired to a plain Tegra GPIO that
# the stock devkit DT maps to KEY_VOLUMEUP (gpio-313) -- it is NOT the bootrom
# FORCE_RECOVERY strap, so by itself it cannot enter recovery. This watches the
# gpio-keys input device and, on a >=1.5 s hold of that button, runs
# `reboot forced-recovery` (supported by the t18x kernel, reboot-t18x.c), which
# puts the board into USB recovery mode (RCM) for flashing over the micro-USB (J17).
#
# Install: copy to /opt/, enable j106-recovery-key.service. See README sec 7.
import struct, time, os, glob
dev = '/dev/input/event4'
for d in glob.glob('/sys/class/input/event*'):
    try:
        if open(d + '/device/name').read().strip() == 'gpio-keys':
            dev = '/dev/input/' + d.split('/')[-1]
    except Exception:
        pass
def log(m): os.system('/usr/bin/logger -t j106-recovery "%s"' % m)
log('handler started on ' + dev)
f = open(dev, 'rb', buffering=0)
KEY_VOLUMEUP = 115           # the M110 recovery button on this carrier
HOLD_SECONDS = 1.5           # guard against accidental short presses
t0 = None
while True:
    data = f.read(24)        # struct input_event on arm64 = 24 bytes
    if not data:
        break
    _, _, etype, code, val = struct.unpack('llHHi', data)
    if etype == 1 and code == KEY_VOLUMEUP:
        if val == 1:
            t0 = time.time()
        elif val == 0 and t0:
            held = time.time() - t0
            t0 = None
            if held >= HOLD_SECONDS:
                log('recovery button held %.1fs -> forced-recovery' % held)
                os.system('/sbin/reboot forced-recovery')
