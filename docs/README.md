# Electric Eye — internal documentation

These are working documents for changing the code, not a user guide. The
[top-level README](../README.md) covers operating the thing; this covers how it is
built and what will bite you when you modify it.

## What it is

A realtime camera effects pipeline in C99 (GNU dialect, GCC 4.7+ or clang), in two
programs. `eeye` captures frames, runs them through a configurable chain of image
operators, and publishes the result on the **frame bus** (shared memory) for anything
else on the machine. `hellion` moves video across the tether as H.264 over RTP, feeds
QGroundControl, records, and serves the browser page. It targets a Raspberry Pi in a
submersible ROV (a BlueROV), controlled from a topside laptop over a direct Ethernet
tether.

Two things shape almost every design decision in the codebase:

- **It runs unattended on a vehicle you cannot reach.** Failures have to be survivable
  or loud, never silent. A camera that unplugs must not kill the process; a config typo
  must be rejected with a reason rather than half-applied.
- **The operator is at the other end of a cable.** Anything they need to know has to
  cross that link explicitly, because they cannot read the drone's console.

## Reading order

| Document | Covers |
|---|---|
| [architecture.md](architecture.md) | Threads, frame lifecycle, ring buffers, startup and shutdown |
| [capture.md](capture.md) | Both capture backends, pixel formats, resolution negotiation, camera controls |
| [effects.md](effects.md) | The effect chain, every operator, LUT fusion, the buffer ping-pong |
| [output.md](output.md) | `eeye`'s outputs: the virtual camera, its raw recording, health reporting |
| [bus.md](bus.md) | The frame bus: streams between programs, its byte layout, reading it from C or Python |
| [hellion.md](hellion.md) | The tether relay: installing, the tether's network, pipelines, RTP over the tether and to QGC, the two placements, the browser page and its API, `hellion top`, `hellion check` |
| [config.md](config.md) | Every config key, the hot-reload contract, the parser |
| [testing.md](testing.md) | Test suites, and the bit-exactness harness for SIMD work |
| [extending.md](extending.md) | Recipes: add an operator, a config key, a capture backend |

If you are here to change one thing, [extending.md](extending.md) is probably the
fastest route — it lists every file a given kind of change touches.

## The shape of the system

Placement B, the usual one (A moves `eeye` topside; see [hellion.md](hellion.md)):

```
          ROV (Raspberry Pi)                         TOPSIDE (laptop)
┌─────────────────────────────────────┐     ┌─────────────────────────────────────┐
│ camera ─► eeye ─► frame bus "pilot" │     │ hellion :5610 ─► QGroundControl     │
│            ▲      (and "raw")       │     │    ▲     │       :5600 (unchanged)  │
│            │          │             │     │    │     ├─► recording .mp4         │
│  effects.json         ▼             │     │    │     └─► decode ─► bus "pilot"  │
│            ▲       hellion ─H.264/RTP─────────►┘              ├─► /dev/video10  │
│            └────── :8091 ◄────────────config, status──┐       └─► page preview  │
│                                     │     │  hellion :8080 ◄── browser          │
└─────────────────────────────────────┘     └─────────────────────────────────────┘
```

`eeye` itself never touches the network. It reads a local JSON file, writes local
status files, and publishes frames on the bus; everything network-facing is `hellion`.
That split keeps the vision layer — the part autonomy code will sit next to — free of
sockets, ports and the tether's failure modes.

## Source layout

`src/` is a flat set of translation units, no subdirectories. At 19 files and ~10,000
lines that is easier to navigate than a hierarchy would be. Most modules use a prefix
matching their filename (`v4l2_in_*`, `camera_ctrl_*`, `fbus_*`), which is the
namespacing C gives you.

| File | Role |
|---|---|
| `eeye.c` | `main()`: startup order, thread creation, shutdown |
| `video_threads.c` | The three pipeline loops plus the stats logger |
| `video_frame.c` | `VideoFrame` allocation, the frame pool |
| `frame_ring_buffer.c` | The lock-free-ish ring between stages |
| `config.c` | JSON parser, validation, inotify hot-reload |
| `v4l2_in.c` | USB/UVC capture: MMAP streaming, format negotiation |
| `rpicam_in.c` | Pi camera capture: forks `rpicam-vid`, reads MJPEG (or raw yuv420) |
| `jpeg_decode.c` | Shared MJPEG→I422 decode used by both backends |
| `camera_ctrl.c` | Portable camera controls across both backends |
| `conv.c` | Neighborhood operators: sobel, blur, LoG, Canny |
| `point_opps.c` | `contrast` (the one point op that inspects the frame) |
| `effect_chain.c` | Chain runner, LUT fusion, buffer ping-pong |
| `v4l2_out.c` | Writes frames to the loopback device |
| `virtual_cam.c` | The optional webcam output: opens lazily, retries, explains a missing module |
| `child_proc.c` | Runs helper programs (rpicam-vid, mokutil, ffmpeg) safely |
| `fbus.c` | The frame bus: shared-memory streams between programs |
| `bus_in.c` | Capture backend that reads a frame bus stream |
| `recorder.c` | Recording tap: owns the file, on its own thread |
| `health.c` | Runtime health snapshot for topside |

`hellion/` is the second binary, the tether relay ([hellion.md](hellion.md)); it
reuses the bus, capture and output modules from `src/`.

There is no Python in the tree. `install.sh` installs both programs as systemd services
(`eeye.service`, `hellion/hellion.service`).

## Conventions worth knowing before you edit

**Comments explain *why*, not *what*.** The codebase leans heavily on this. If you find
a comment describing a non-obvious tradeoff, it is usually load-bearing — several
document decisions that were made by measurement and would otherwise look arbitrary
(see [effects.md](effects.md) on Canny's hysteresis, for one).

**An unknown config key is a hard error.** Not a warning. A typo should be loud rather
than a silent no-op. This applies inside nested objects too.

**Failures degrade rather than abort, except at startup.** A missing camera retries
forever; a missing loopback device disables only the webcam output. But a config that cannot be
parsed at startup refuses to run, because there is no previous good state to fall back
to.

**Verification is expected to be empirical.** Claims in commit messages here cite
measurements. SIMD work in particular has a fixed bar — bit-exactness against the
scalar implementation under emulation, documented in [testing.md](testing.md).
