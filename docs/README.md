# Electric Eye — internal documentation

These are working documents for changing the code, not a user guide. The
[top-level README](../README.md) covers operating the thing; this covers how it is
built and what will bite you when you modify it.

## What it is

A realtime camera effects pipeline in C23. It captures frames, runs them through a
configurable chain of image operators, and publishes the result as a
[v4l2loopback](https://github.com/umlaeute/v4l2loopback) virtual camera. It targets a
Raspberry Pi in a submersible ROV, controlled from a topside laptop over a direct
Ethernet tether.

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
| [output.md](output.md) | Virtual camera, preview stream, recording, health reporting |
| [config.md](config.md) | Every config key, the hot-reload contract, the parser |
| [topside.md](topside.md) | Web UI, config agent, network tooling, tether addressing |
| [testing.md](testing.md) | Test suites, and the bit-exactness harness for SIMD work |
| [extending.md](extending.md) | Recipes: add an operator, a config key, a capture backend |

If you are here to change one thing, [extending.md](extending.md) is probably the
fastest route — it lists every file a given kind of change touches.

## The shape of the system

```
        DRONE (Raspberry Pi)                          TOPSIDE (laptop)
┌──────────────────────────────────┐          ┌───────────────────────────┐
│  camera                          │          │                           │
│    │                             │          │  browser                  │
│    ▼                             │          │    ▲                      │
│  eeye  ──────► /dev/video10      │          │    │ HTTP :8080           │
│    │           (v4l2loopback)    │          │  web_ui.py                │
│    │                             │          │    │      ▲               │
│    ├──JPEG──► stream_server :9000├──────────┼────┘      │               │
│    │                             │  tether  │           │               │
│    └──raw───► record_path        │          │  eeye-record ──► .mp4     │
│                                  │          │                           │
│  config_agent.py :9001 ◄─────────┼──────────┼──── config writes         │
│    │ writes eeye_config.json     │          │                           │
│    └─ relays .health / .status   │          │  eeye-net (diagnostics)   │
└──────────────────────────────────┘          └───────────────────────────┘
```

`eeye` itself never touches the network. It reads a local JSON file and writes local
status files; everything network-facing lives in the Python tools. That split is
deliberate — see [topside.md](topside.md).

## Source layout

`src/` is a flat set of translation units, no subdirectories. At 16 files and ~7,800
lines that is easier to navigate than a hierarchy would be. Most modules use a prefix
matching their filename (`v4l2_in_*`, `camera_ctrl_*`, `stream_server_*`), which is the
namespacing C gives you.

| File | Role |
|---|---|
| `eeye.c` | `main()`: startup order, thread creation, shutdown |
| `video_threads.c` | The three pipeline loops plus the stats logger |
| `video_frame.c` | `VideoFrame` allocation, the frame pool |
| `frame_ring_buffer.c` | The lock-free-ish ring between stages |
| `config.c` | JSON parser, validation, inotify hot-reload |
| `v4l2_in.c` | USB/UVC capture: MMAP streaming, format negotiation |
| `rpicam_in.c` | Pi camera capture: forks `rpicam-vid`, reads MJPEG |
| `jpeg_decode.c` | Shared MJPEG→I422 decode used by both backends |
| `camera_ctrl.c` | Portable camera controls across both backends |
| `conv.c` | Neighborhood operators: sobel, blur, LoG, Canny |
| `point_opps.c` | `contrast` (the one point op that inspects the frame) |
| `effect_chain.c` | Chain runner, LUT fusion, buffer ping-pong |
| `v4l2_out.c` | Writes frames to the loopback device |
| `virtual_cam.c` | Loads/unloads the v4l2loopback module |
| `stream_server.c` | JPEG preview tap over TCP |
| `health.c` | Runtime health snapshot for topside |

Python lives outside the meson build: `pi/` (drone side), `topside/`, `tools/`.

## Conventions worth knowing before you edit

**Comments explain *why*, not *what*.** The codebase leans heavily on this. If you find
a comment describing a non-obvious tradeoff, it is usually load-bearing — several
document decisions that were made by measurement and would otherwise look arbitrary
(see [effects.md](effects.md) on Canny's hysteresis, for one).

**An unknown config key is a hard error.** Not a warning. A typo should be loud rather
than a silent no-op. This applies inside nested objects too.

**Failures degrade rather than abort, except at startup.** A missing camera retries
forever; a failed preview stream disables only itself. But a config that cannot be
parsed at startup refuses to run, because there is no previous good state to fall back
to.

**Verification is expected to be empirical.** Claims in commit messages here cite
measurements. SIMD work in particular has a fixed bar — bit-exactness against the
scalar implementation under emulation, documented in [testing.md](testing.md).
