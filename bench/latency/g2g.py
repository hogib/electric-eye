#!/usr/bin/env python3
# /// script
# requires-python = ">=3.9"
# dependencies = ["numpy", "pygame"]
# ///
"""g2g: the video path's latency, measured through the camera's own lens.

This program fills a screen with a pattern that changes at every refresh
and encodes the refresh's number. Point the ROV's camera at that screen.
The pictures that come out of the video path -- decoded, where an
application would use them -- are read here, the number in each is
decoded, and the frame's latency is the time between the refresh that
showed that number and the frame's arrival:

    screen -> lens -> sensor -> camera -> ROV software -> tether ->
    topside software -> decoded frame

That is every stage of glass-to-glass latency with one display in the
loop, as glass-to-glass has; the display is the one showing the pattern
instead of the one showing the pilot's picture. It needs no hardware
beyond what is on the table, and it measures any video path whose decoded
frames can be piped to it, so electric-eye and the stacks it is compared
with are measured the same way.

    uv run bench/latency/g2g.py --size 1280x720 --seconds 60 \\
        --source 'builddir/bench/bus_cat camera' --json out.json --csv out.csv

--source is a shell command that writes decoded frames to its stdout as
8-bit grey, --size bytes each, as they arrive (bus_cat for electric-eye;
an ffmpeg or GStreamer receiver ending in raw GRAY8 for the others).

How it works
- The pattern is a 4x3 grid of cells: ten carry the refresh counter in
  Gray code, one is always white and one always black. Successive numbers
  differ in one cell, so a camera exposure that straddles two refreshes
  reads as one of the two, never as a third.
- Calibration (about 20 s, first): each cell is lit alone, and the pixels
  that respond in the received picture become that cell's mask. So the
  camera can be at any angle and distance, as long as it sees the whole
  screen. The white and black cells then give each frame its own
  threshold, whatever the exposure does.
- A frame whose cells do not read cleanly is counted as unreadable, not
  guessed at.

What the figures mean
- Resolution is one screen refresh plus one camera exposure: at 60 Hz
  and 30 fps a single frame's figure is uncertain by up to about +-25 ms,
  so read the median and the percentiles over a run, not single frames.
- The time of a refresh is taken when the buffer swap returns. A
  compositor or the panel may show it a refresh later; that is in the
  figure, as the pilot's display latency is in glass-to-glass.

--selftest runs the decoder against a simulated screen and camera with a
known delay and no display, to check the tool itself.
"""
import argparse
import collections
import json
import os
import subprocess
import sys
import threading
import time

import numpy as np

COLS, ROWS = 4, 3
DATA_BITS = 10
CELLS = COLS * ROWS  # ten data cells, then white, then black
WHITE, BLACK = DATA_BITS, DATA_BITS + 1
WRAP = 1 << DATA_BITS
SMALL = 4  # received frames are read at every 4th pixel each way


def gray(n):
    return n ^ (n >> 1)


def ungray(g):
    n = 0
    while g:
        n ^= g
        g >>= 1
    return n


def cells_for(counter):
    """Which cells are lit for a refresh counter."""
    g = gray(counter % WRAP)
    lit = [(g >> i) & 1 == 1 for i in range(DATA_BITS)]
    return lit + [True, False]


def cell_rect(i, w, h):
    """Cell i's rectangle on a w x h surface, with a dark gap around it."""
    cw, ch = w / COLS, h / ROWS
    x, y = (i % COLS) * cw, (i // COLS) * ch
    gx, gy = cw * 0.12, ch * 0.12
    return int(x + gx), int(y + gy), int(cw - 2 * gx), int(ch - 2 * gy)


class Shown:
    """What the screen has shown and when: written by the display loop."""

    def __init__(self):
        self.lock = threading.Lock()
        self.flips = collections.deque(maxlen=8192)  # (time, counter)
        self.phase = ("idle", 0, 0.0)  # name, index, since

    def flip(self, t, counter):
        with self.lock:
            self.flips.append((t, counter))

    def set_phase(self, name, index, t):
        with self.lock:
            self.phase = (name, index, t)

    def when(self, value, before):
        """The latest refresh at or before `before` whose counter is
        `value` modulo the wrap."""
        with self.lock:
            for t, c in reversed(self.flips):
                if t <= before and c % WRAP == value:
                    return t, c
        return None


class Decoder:
    """Takes received frames; learns the masks, then reads the counter."""

    def __init__(self, shown, settle):
        self.shown = shown
        self.settle = settle  # seconds into a calibration step before using it
        self.lock = threading.Lock()
        self.acc = {}  # calibration step -> [sum, count]
        self.masks = None
        self.results = []  # (arrival, counter, latency_s)
        self.unreadable = 0
        self.frames = 0
        self.first = self.last = None

    def frame(self, t, small):
        with self.lock:
            self.frames += 1
            self.first = self.first or t
            self.last = t
        name, index, since = self.shown.phase
        if name == "calibrate":
            if t - since >= self.settle:
                with self.lock:
                    a = self.acc.setdefault(index, [np.zeros(small.shape), 0])
                    a[0] += small
                    a[1] += 1
        elif name == "measure" and self.masks is not None:
            self.read(t, small)

    def finish_calibration(self):
        """Builds the masks. Returns an error string, or None."""
        with self.lock:
            acc = {k: v[0] / v[1] for k, v in self.acc.items() if v[1]}
        if 0 not in acc:
            return "no frames arrived while the screen was dark"
        off = acc[0]
        masks = []
        for i in range(CELLS):
            if i + 1 not in acc:
                return "no frames arrived while cell %d was lit" % i
            diff = acc[i + 1] - off
            peak = float(diff.max())
            if peak < 25:
                return ("cell %d of the pattern was not seen (brightest "
                        "change %.0f of 255): the camera must see the whole "
                        "screen" % (i, peak))
            m = diff > 0.6 * peak
            if m.sum() < 12:
                return "cell %d covers too few pixels in the picture" % i
            masks.append(m)
        for i in range(CELLS):
            for j in range(i):
                if (masks[i] & masks[j]).sum() > 0.2 * masks[i].sum():
                    return ("cells %d and %d overlap in the picture: "
                            "reflections, or the screen is too far away"
                            % (j, i))
        self.masks = masks
        return None

    def read(self, t, small):
        m = [float(small[k].mean()) for k in self.masks]
        white, black = m[WHITE], m[BLACK]
        span = white - black
        if span < 20:
            self.unreadable += 1
            return
        mid = (white + black) / 2
        value, unsure = 0, 0
        for i in range(DATA_BITS):
            if abs(m[i] - mid) < 0.15 * span:
                unsure += 1
            if m[i] > mid:
                value |= 1 << i
        # One cell between levels is an exposure across two refreshes
        # (Gray code); more is not this pattern.
        if unsure > 1:
            self.unreadable += 1
            return
        hit = self.shown.when(ungray(value), t)
        if not hit or t - hit[0] > 5.0:
            self.unreadable += 1
            return
        self.results.append((t, hit[1], t - hit[0]))


def reader(cmd, w, h, decoder, stop):
    """Runs the source command and hands its frames to the decoder."""
    p = subprocess.Popen(cmd, shell=True, stdout=subprocess.PIPE, bufsize=0)
    size = w * h
    buf = bytearray(size)
    view = memoryview(buf)
    try:
        while not stop.is_set():
            got = 0
            while got < size:
                n = p.stdout.readinto(view[got:])
                if not n:
                    return
                got += n
            t = time.monotonic()
            img = np.frombuffer(buf, np.uint8).reshape(h, w)
            decoder.frame(t, img[::SMALL, ::SMALL].astype(np.float32))
    finally:
        p.terminate()
        try:
            p.wait(2)
        except subprocess.TimeoutExpired:
            p.kill()


def run_screen(shown, plan, hz, windowed):
    """Shows the plan's steps on the real screen (main thread)."""
    os.environ.setdefault("PYGAME_HIDE_SUPPORT_PROMPT", "1")
    import pygame
    pygame.init()
    flags = pygame.SCALED if windowed else pygame.FULLSCREEN | pygame.SCALED
    info = pygame.display.Info()
    # SCALED wants a real size: the desktop's, for the whole screen.
    size = (960, 540) if windowed or info.current_w <= 0 else (
        info.current_w, info.current_h)
    try:
        screen = pygame.display.set_mode(size, flags, vsync=1)
    except pygame.error:
        screen = pygame.display.set_mode(size, flags)
    pygame.mouse.set_visible(False)
    w, h = screen.get_size()
    rects = [pygame.Rect(*cell_rect(i, w, h)) for i in range(CELLS)]
    clock = pygame.time.Clock()
    counter = 0
    for name, index, seconds, lit_fn in plan:
        start = time.monotonic()
        shown.set_phase(name, index, start)
        while time.monotonic() - start < seconds:
            for e in pygame.event.get():
                if e.type == pygame.QUIT or (e.type == pygame.KEYDOWN and
                                             e.key in (pygame.K_ESCAPE,
                                                       pygame.K_q)):
                    pygame.quit()
                    return False
            lit = lit_fn(counter)
            screen.fill((0, 0, 0))
            for i, r in enumerate(rects):
                if lit[i]:
                    screen.fill((255, 255, 255), r)
            pygame.display.flip()
            shown.flip(time.monotonic(), counter)
            counter += 1
            clock.tick(hz)  # a ceiling, for when vsync is not honoured
    pygame.quit()
    return True


class FakeRig:
    """A simulated screen and camera, for --selftest: the screen's cells
    seen small, off-centre, dim and noisy, by a 30 fps camera whose
    exposure spans refreshes and whose frames arrive `delay` later."""

    def __init__(self, shown, decoder, delay, hz=60.0, fps=30.0):
        self.shown, self.decoder, self.delay = shown, decoder, delay
        self.hz, self.fps = hz, fps
        self.history = collections.deque(maxlen=4096)  # (time, lit)
        self.lock = threading.Lock()
        self.rng = np.random.default_rng(1)
        self.h, self.w = 180, 320
        # The screen occupies part of the picture.
        self.box = (40, 25, 230, 130)

    def render(self, weights):
        img = np.full((self.h, self.w), 30.0, np.float32)
        x0, y0, bw, bh = self.box
        for i in range(CELLS):
            x, y, cw, ch = cell_rect(i, bw, bh)
            img[y0 + y:y0 + y + ch, x0 + x:x0 + x + cw] += 150.0 * weights[i]
        return img + self.rng.normal(0, 3, img.shape).astype(np.float32)

    def camera(self, stop):
        exposure = 1.0 / 60
        nxt = time.monotonic()
        pending = collections.deque()
        while not stop.is_set():
            now = time.monotonic()
            while pending and pending[0][0] <= now:
                t, img = pending.popleft()
                self.decoder.frame(time.monotonic(), img)
            if now >= nxt:
                nxt += 1.0 / self.fps
                # What was on the screen over the exposure, by time shown.
                with self.lock:
                    hist = list(self.history)
                w = np.zeros(CELLS)
                total = 0.0
                for k, (t, lit) in enumerate(hist):
                    end = hist[k + 1][0] if k + 1 < len(hist) else now
                    a, b = max(t, now - exposure), min(end, now)
                    if b > a:
                        w += (b - a) * np.array(lit, float)
                        total += b - a
                if total > 0:
                    pending.append((now + self.delay, self.render(w / total)))
            time.sleep(0.001)

    def run_screen(self, plan):
        counter = 0
        nxt = time.monotonic()
        for name, index, seconds, lit_fn in plan:
            start = time.monotonic()
            self.shown.set_phase(name, index, start)
            while time.monotonic() - start < seconds:
                lit = lit_fn(counter)
                t = time.monotonic()
                with self.lock:
                    self.history.append((t, lit))
                self.shown.flip(t, counter)
                counter += 1
                nxt += 1.0 / self.hz
                time.sleep(max(0.0, nxt - time.monotonic()))
        return True


def make_plan(hold, seconds, calibrated):
    """The steps: (phase, index, seconds, counter -> lit cells)."""
    plan = [("calibrate", 0, hold, lambda c: [False] * CELLS)]
    for i in range(CELLS):
        plan.append(("calibrate", i + 1, hold,
                     lambda c, i=i: [j == i for j in range(CELLS)]))

    def measure(c):
        return cells_for(c)
    # A second of the running pattern before frames count, while the
    # decoder's masks are being built.
    plan.append(("settle", 0, 1.0, measure))
    plan.append(("measure", 0, seconds, measure))
    return plan


def summarize(decoder, shown):
    lat = np.array([r[2] for r in decoder.results]) * 1000.0
    flips = [t for t, _ in shown.flips]
    gaps = np.diff(flips) if len(flips) > 2 else np.array([])
    out = {
        "frames_read": int(len(lat)),
        "frames_unreadable": int(decoder.unreadable),
        "screen_refresh_ms": round(float(np.median(gaps)) * 1000, 2)
        if len(gaps) else None,
    }
    if decoder.frames > 1 and decoder.last > decoder.first:
        out["source_fps"] = round((decoder.frames - 1) /
                                  (decoder.last - decoder.first), 2)
    if len(lat):
        for name, q in (("min", 0), ("p5", 5), ("p50", 50), ("p95", 95),
                        ("p99", 99), ("max", 100)):
            out["latency_ms_" + name] = round(float(np.percentile(lat, q)), 1)
        out["latency_ms_mean"] = round(float(lat.mean()), 1)
    return out


def main():
    ap = argparse.ArgumentParser(
        description="Video latency through the camera's own lens.")
    ap.add_argument("--source", help="command writing GRAY8 frames to stdout")
    ap.add_argument("--size", default="1280x720")
    ap.add_argument("--seconds", type=float, default=60)
    ap.add_argument("--hold", type=float, default=1.5,
                    help="seconds each calibration step is shown; must be "
                         "over twice the path's latency (default 1.5)")
    ap.add_argument("--hz", type=float, default=60,
                    help="the screen's refresh rate, as a ceiling (default 60)")
    ap.add_argument("--windowed", action="store_true")
    ap.add_argument("--json")
    ap.add_argument("--csv")
    ap.add_argument("--label", default="")
    ap.add_argument("--selftest", action="store_true")
    a = ap.parse_args()

    shown = Shown()
    decoder = Decoder(shown, settle=a.hold * 0.6)
    stop = threading.Event()

    if a.selftest:
        delay = 0.120
        rig = FakeRig(shown, decoder, delay)
        plan = make_plan(0.5, 6.0, None)
        decoder.settle = 0.3
        cam = threading.Thread(target=rig.camera, args=(stop,), daemon=True)
        cam.start()
        ok = run_plan(plan, decoder, lambda p: rig.run_screen(p))
        stop.set()
        s = summarize(decoder, shown)
        print(json.dumps(s, indent=1))
        # The simulated camera adds half its exposure on average.
        want = delay * 1000 + 8
        good = (ok and s.get("frames_read", 0) > 120 and
                abs(s["latency_ms_p50"] - want) < 20 and
                s["frames_unreadable"] <= 0.05 * s["frames_read"])
        print("selftest: %s (median %.1f ms for a simulated %.0f ms)" %
              ("ok" if good else "FAILED", s.get("latency_ms_p50", -1), want))
        return 0 if good else 1

    if not a.source:
        ap.error("--source is required (or --selftest)")
    w, h = (int(x) for x in a.size.lower().split("x"))
    th = threading.Thread(target=reader, args=(a.source, w, h, decoder, stop),
                          daemon=True)
    th.start()
    plan = make_plan(a.hold, a.seconds, None)
    ok = run_plan(plan, decoder,
                  lambda p: run_screen(shown, p, a.hz, a.windowed))
    stop.set()
    s = summarize(decoder, shown)
    s["label"] = a.label
    s["source"] = a.source
    text = json.dumps(s, indent=1)
    print(text)
    if a.json:
        with open(a.json, "w") as f:
            f.write(text + "\n")
    if a.csv:
        with open(a.csv, "w") as f:
            f.write("arrival_s,counter,latency_ms\n")
            for t, c, lat in decoder.results:
                f.write("%.4f,%d,%.2f\n" % (t, c, lat * 1000))
    if not ok:
        return 1
    if decoder.frames == 0:
        print("g2g: the source produced no frames", file=sys.stderr)
        return 1
    return 0


def run_plan(plan, decoder, show):
    """Shows the calibration steps, builds the masks, then the rest."""
    cal = [p for p in plan if p[0] == "calibrate"]
    rest = [p for p in plan if p[0] != "calibrate"]
    state = {"ok": True}

    def staged(_):
        # The masks are built on the settle step's first refresh, from a
        # thread, so the screen never stalls on it.
        def build():
            err = decoder.finish_calibration()
            if err:
                state["ok"] = False
                print("g2g: calibration failed: " + err, file=sys.stderr)
        first = rest[0]

        def settle_fn(c, done=[False]):
            if not done[0]:
                done[0] = True
                threading.Thread(target=build, daemon=True).start()
            return first[3](c)
        steps = cal + [(first[0], first[1], first[2], settle_fn)] + rest[1:]
        return steps

    return show(staged(None)) and state["ok"]


if __name__ == "__main__":
    sys.exit(main())
