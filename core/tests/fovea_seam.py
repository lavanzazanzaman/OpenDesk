#!/usr/bin/env python3
"""Foveated streaming: does the fovea crop match the periphery where the headset blends them (radius 0.43-0.45)?
Composes like the headset (VideoPlayer + DefoveationPrimitive: periphery through the warp, nearest-sampled; the
fovea at FoveaTopLeft) from a harness run's dumped left-eye streams and FoveaTopLeft log.
  fovea_seam.py WORKDIR PW PH FW FH EYE_W EYE_H FRAME"""
import subprocess
import sys

import numpy as np

W, pw, ph, fw, fh, ew, eh, n = sys.argv[1], *map(int, sys.argv[2:9])


def frame(path, w, h):
    raw = subprocess.run(["ffmpeg", "-v", "error", "-i", path, "-vf", f"select=eq(n\\,{n})", "-frames:v", "1",
                          "-f", "rawvideo", "-pix_fmt", "gray", "-"], capture_output=True).stdout
    return np.frombuffer(raw[:w * h], np.uint8).reshape(h, w).astype(float)


def warp(s):  # the headset's defoveation: screen -> stream image
    return np.where(s < .25, 2 * s * s, np.where(s > .75, 1 - 2 * (1 - s) ** 2, 1.5 * (s - .25) + .125))


P, F = frame(W + "/vr-0.bin", pw, ph), frame(W + "/vr-1.bin", fw, fh)
lines = [l for l in open(W + "/fovea.txt").read().split("\n") if l]
x0, y0 = map(int, lines[min(n, len(lines) - 1)].split()[:2])
ys, xs = np.mgrid[0:fh, 0:fw]
tx = np.clip((warp((x0 + xs + .5) / ew) * pw).astype(int), 0, pw - 1)
ty = np.clip((warp((y0 + ys + .5) / eh) * ph).astype(int), 0, ph - 1)
d = np.abs(F - P[ty, tx])
r = np.hypot((xs + .5) / fw - .5, (ys + .5) / fh - .5)
ring = d[(r >= .43) & (r < .45)]
tail = np.percentile(ring, 99.9)
ok = ring.mean() < 1.5 and tail < 20  # codec noise only (a few pixels on edges may differ more)
print(f"{'PASS' if ok else 'FAIL'}  fovea matches the periphery at the blend ring (mean |diff| {ring.mean():.2f}, 99.9% under {tail:.0f}, max {ring.max():.0f})")
sys.exit(0 if ok else 1)
