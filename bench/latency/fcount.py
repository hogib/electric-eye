#!/usr/bin/env python3
"""fcount: counts decoded frames arriving on stdin, for a run without g2g.

    receiver | fcount.py --size 1280x720 --skip 6 --seconds 60

Reads GRAY8 frames of --size, ignores the first --skip seconds (start-up),
then counts for --seconds and prints the frame rate and the longest gap
between frames as JSON. Standard library only.
"""
import argparse
import json
import sys
import time

ap = argparse.ArgumentParser()
ap.add_argument("--size", default="1280x720")
ap.add_argument("--skip", type=float, default=5)
ap.add_argument("--seconds", type=float, default=60)
a = ap.parse_args()
w, h = (int(x) for x in a.size.lower().split("x"))
size = w * h
buf = bytearray(size)
view = memoryview(buf)
src = sys.stdin.buffer.raw
start = time.monotonic()
times = []
first = None
while True:
    got = 0
    while got < size:
        n = src.readinto(view[got:])
        if not n:
            break
        got += n
    if got < size:
        break
    t = time.monotonic()
    first = first if first is not None else t - start
    if t - start < a.skip:
        continue
    if t - start > a.skip + a.seconds:
        break
    times.append(t)
gaps = [b - c for b, c in zip(times[1:], times)]
out = {"frames": len(times),
       "first_frame_after_s": round(first, 2) if first is not None else None}
if len(times) > 1:
    out["fps"] = round((len(times) - 1) / (times[-1] - times[0]), 2)
    out["longest_gap_ms"] = round(max(gaps) * 1000, 1)
    out["gaps_over_100ms"] = sum(g > 0.1 for g in gaps)
print(json.dumps(out))
