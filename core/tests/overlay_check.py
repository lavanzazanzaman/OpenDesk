#!/usr/bin/env python3
"""Counts the frames of a dumped VR eye stream with the overlay test's magenta quad at the centre.
Usage: overlay_check.py STREAM.bin  (prints: frames, frames with the quad, frames showing something else there)"""
import subprocess
import sys

SIZE = 64
raw = subprocess.run(["ffmpeg", "-v", "error", "-i", sys.argv[1], "-vf", f"scale={SIZE}:{SIZE}:flags=neighbor",
                      "-f", "rawvideo", "-pix_fmt", "rgb24", "-"], capture_output=True).stdout
frame = SIZE * SIZE * 3
frames = quad = other = 0
for start in range(0, len(raw) - frame + 1, frame):
    frames += 1
    offset = start + (SIZE // 2 * SIZE + SIZE // 2) * 3
    r, g, b = raw[offset], raw[offset + 1], raw[offset + 2]
    if r > 170 and b > 170 and g < 90:
        quad += 1
    else:
        other += 1
print(f"frames {frames} quad {quad} other {other}")
