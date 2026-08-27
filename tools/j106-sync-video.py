#!/usr/bin/env python3
"""Record a 2x2 video in which every grid frame is FOUR SIMULTANEOUS exposures.

Unlike the RTP/compositor path (csi_sender.sh -> csi_receiver.sh), which passes
each camera through its own 100 ms jitterbuffer and can leave cells a frame or
two apart, this reads /dev/videoN directly and groups frames by their V4L2
capture timestamp. A grid frame is only emitted when all four cameras have a
frame from the same trigger pulse, so a clock in the scene reads the same in
every cell.

Frames are downsampled to one Bayer phase (728x544 grey) inside the capture
loop to keep up with 30 fps on four cameras.

  sudo ./j106-sync-video.py -n 150 -o /tmp/syncvid.gray
"""
import argparse, fcntl, importlib.util, os, struct, sys, threading
import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
spec = importlib.util.spec_from_file_location("scheck", os.path.join(HERE, "j106-sync-check.py"))
scheck = importlib.util.module_from_spec(spec); spec.loader.exec_module(scheck)

W, H, STRIDE = 1456, 1088, 3072
TW, TH = W // 2, H // 2          # 728x544 after taking one Bayer phase

class VidCam(scheck.Camera):
    def __init__(self, dev):
        super().__init__(dev)
        self.small = []          # uint16 728x544, one per frame

    def run(self, count):
        try:
            fcntl.ioctl(self.fd, scheck.VIDIOC_STREAMON,
                        struct.pack("<i", scheck.BUF_TYPE_VIDEO_CAPTURE))
            for _ in range(count):
                raw = fcntl.ioctl(self.fd, scheck.VIDIOC_DQBUF, scheck.pack_buf())
                b = scheck.unpack_buf(raw)
                self.stamps.append(b["tv_sec"] + b["tv_usec"] / 1e6)
                self.seqs.append(b["sequence"])
                m = self.maps[b["index"]]
                a = np.frombuffer(m, dtype=np.uint8, count=STRIDE * H).reshape(H, STRIDE)
                px = a.view(np.uint16)[:, :W]
                self.small.append(px[::2, ::2].copy())      # 728x544 uint16
                fcntl.ioctl(self.fd, scheck.VIDIOC_QBUF, scheck.pack_buf(b["index"]))
            fcntl.ioctl(self.fd, scheck.VIDIOC_STREAMOFF,
                        struct.pack("<i", scheck.BUF_TYPE_VIDEO_CAPTURE))
        except Exception as exc:
            self.error = exc

ap = argparse.ArgumentParser()
ap.add_argument("-n", "--frames", type=int, default=150)
ap.add_argument("-o", "--out", default="/tmp/syncvid.gray")
ap.add_argument("-t", "--tol-us", type=float, default=2000.0)
ap.add_argument("-d", "--device", action="append", dest="devices")
a = ap.parse_args()

devs = a.devices or sorted(__import__("glob").glob("/dev/video*"))
cams = [VidCam(d) for d in devs]
for c in cams: c.open()
ts = [threading.Thread(target=c.run, args=(a.frames,)) for c in cams]
for t in ts: t.start()
for t in ts: t.join()
for c in cams:
    if c.error: print(f"{c.name}: {c.error}"); sys.exit(1)

for c in cams:
    gaps = np.diff(c.seqs)
    print(f"{c.name}: {len(c.small)} frames, seq gaps>1: {(gaps>1).sum()}")

# fixed per-camera scale (computed once) so brightness cannot flicker
scale = []
for c in cams:
    hi = np.percentile(c.small[len(c.small)//2], 99.5)
    scale.append(255.0 / max(hi, 1))

# group by timestamp: for each frame of cam0, take the nearest in the others
base = cams[0]
tol = a.tol_us / 1e6
out = open(a.out, "wb")
n_emit = 0; spreads = []
for i, t0 in enumerate(base.stamps):
    picks = [i]; ok = True
    for c in cams[1:]:
        arr = np.asarray(c.stamps)
        j = int(np.argmin(np.abs(arr - t0)))
        if abs(arr[j] - t0) > tol: ok = False; break
        picks.append(j)
    if not ok: continue
    stset = [cams[k].stamps[picks[k]] for k in range(len(cams))]
    spreads.append((max(stset) - min(stset)) * 1e6)
    tiles = []
    for k, c in enumerate(cams):
        v = c.small[picks[k]].astype(np.float32) * scale[k]
        v = np.clip(v, 0, 255).astype(np.uint8)
        tiles.append(np.rot90(v, 2))          # modules mounted inverted
    grid = np.vstack([np.hstack(tiles[0:2]), np.hstack(tiles[2:4])])
    out.write(grid.tobytes()); n_emit += 1
out.close()
sp = np.asarray(spreads)
print(f"emitted {n_emit} grid frames of {TW*2}x{TH*2}")
print(f"per-frame timestamp spread across the 4 cameras: "
      f"max={sp.max():.1f} us  mean={sp.mean():.2f} us")
for c in cams: c.close()
