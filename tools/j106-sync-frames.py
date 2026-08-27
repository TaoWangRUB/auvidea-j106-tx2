#!/usr/bin/env python3
"""Capture frames from every camera WITH their V4L2 timestamps, then save the
set that was captured at the same instant.

This is the visual counterpart to j106-sync-check.py: that one proves the
timestamps agree, this one lets you read a clock in the scene and check the
digits agree too. Reuses j106-sync-check.py's Camera class verbatim.

  sudo ./sync_frames.py -n 25 -o /tmp/sf
"""
import argparse, importlib.util, os, struct, sys, threading, fcntl

HERE = os.path.dirname(os.path.abspath(__file__))
spec = importlib.util.spec_from_file_location("scheck", os.path.join(HERE, "j106-sync-check.py"))
scheck = importlib.util.module_from_spec(spec); spec.loader.exec_module(scheck)

KEEP = 8   # frames of pixel data retained per camera (each ~3.3 MB)

class GrabCam(scheck.Camera):
    def __init__(self, dev):
        super().__init__(dev)
        self.frames = []          # list of (stamp, bytes)

    def run(self, count):
        try:
            fcntl.ioctl(self.fd, scheck.VIDIOC_STREAMON,
                        struct.pack("<i", scheck.BUF_TYPE_VIDEO_CAPTURE))
            for _ in range(count):
                raw = fcntl.ioctl(self.fd, scheck.VIDIOC_DQBUF, scheck.pack_buf())
                b = scheck.unpack_buf(raw)
                st = b["tv_sec"] + b["tv_usec"] / 1e6
                self.stamps.append(st); self.seqs.append(b["sequence"])
                m = self.maps[b["index"]]
                self.frames.append((st, m[:b["length"]]))
                if len(self.frames) > KEEP:
                    self.frames.pop(0)
                fcntl.ioctl(self.fd, scheck.VIDIOC_QBUF, scheck.pack_buf(b["index"]))
            fcntl.ioctl(self.fd, scheck.VIDIOC_STREAMOFF,
                        struct.pack("<i", scheck.BUF_TYPE_VIDEO_CAPTURE))
        except Exception as exc:
            self.error = exc

ap = argparse.ArgumentParser()
ap.add_argument("-n", "--frames", type=int, default=25)
ap.add_argument("-o", "--out", default="/tmp/sf")
ap.add_argument("-d", "--device", action="append", dest="devices")
a = ap.parse_args()
devs = a.devices or sorted(__import__("glob").glob("/dev/video*"))
cams = [GrabCam(d) for d in devs]
for c in cams: c.open()
ts = [threading.Thread(target=c.run, args=(a.frames,)) for c in cams]
for t in ts: t.start()
for t in ts: t.join()
for c in cams:
    if c.error: print(f"{c.name}: {c.error}"); sys.exit(1)

# pick, from the retained frames, the combination with the smallest time spread
base = cams[0]
best = None
for st0, _ in base.frames:
    picks = [(st0, 0)]
    ok = True
    for ci, c in enumerate(cams[1:], start=1):
        if not c.frames: ok = False; break
        j = min(range(len(c.frames)), key=lambda k: abs(c.frames[k][0] - st0))
        picks.append((c.frames[j][0], j))
        if abs(c.frames[j][0] - st0) > 0.020: ok = False; break
    if not ok: continue
    spread = max(p[0] for p in picks) - min(p[0] for p in picks)
    if best is None or spread < best[0]:
        best = (spread, st0, [p[1] for p in picks])

if best is None:
    print("no aligned frame set found"); sys.exit(2)
spread, st0, idxs = best
os.makedirs(a.out, exist_ok=True)
print(f"aligned set: spread = {spread*1e6:.1f} us across {len(cams)} cameras")
i0 = min(range(len(base.frames)), key=lambda k: abs(base.frames[k][0] - st0))
allidx = [i0] + idxs[1:]
for c, k in zip(cams, allidx):
    st, data = c.frames[k]
    fn = os.path.join(a.out, f"{c.name}.raw")
    open(fn, "wb").write(data)
    print(f"  {c.name}: t={st:.6f}  offset={1e6*(st-st0):+8.1f} us  -> {fn} ({len(data)} bytes)")
for c in cams: c.close()
