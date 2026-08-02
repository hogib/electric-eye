# Electric Eye

A realtime webcam effects pipeline for Linux. Captures from a V4L2 camera, applies an
effect (edge detection, tint, threshold, ...), and outputs to a
[v4l2loopback](https://github.com/umlaeute/v4l2loopback) virtual camera that any app
(browser, Zoom, `ffplay`, ...) can pick up like a normal webcam.

Built and tested targeting a Raspberry Pi 5 with a USB UVC webcam, but nothing here is
Pi-specific beyond the install script's package manager assumption.

## Features

- **Direct V4L2 I/O on both ends** — no `ffmpeg` subprocess anywhere, at build or run
  time. Capture uses MMAP streaming (`REQBUFS`/`QBUF`/`DQBUF`) and decodes MJPEG
  straight from the driver's DMA buffer via libjpeg-turbo, with zero intermediate
  copies. Output writes directly to the loopback device.
- **Six effects**, selectable and parameterized live via config:
  `sobel` (edge detection), `grayscale`, `invert`, `threshold`, `tint`, `none`.
- **Hot-reloadable JSON config.** Edit the config file — locally, or have a script on
  another device write it over `scp`/`rsync` — and the running pipeline picks up the
  change within ~200ms, no restart.
- **OpenMP-parallelized effects.** Every hot loop runs across all available cores.
- **Graceful shutdown** on Ctrl+C or `systemctl stop`, and **automatic camera
  reconnect** if the camera isn't plugged in yet or gets disconnected mid-session —
  the pipeline keeps running and picks it back up rather than dying.
- **One-command install** (`install.sh`) and a **systemd service** for running
  unattended: starts on boot, restarts on failure.

## How it works

```
 camera (MJPEG)                                          v4l2loopback
      │                                                        ▲
      ▼                                                        │
┌─────────────┐   raw    ┌──────────────┐   work    ┌──────────────────┐
│ producer    │ ───────► │ effects      │ ────────► │ consumer         │
│ (v4l2_in)   │  frame   │ (conv/       │  frame    │ (v4l2_out)       │
└─────────────┘  pool    │  point_opps) │  pool     └──────────────────┘
                          └──────────────┘
                                 ▲
                                 │ live snapshot, once per frame
                          ┌──────────────┐        inotify
                          │ config watch │ ◄───── eeye_config.json
                          └──────────────┘
```

Three threads (producer/effects/consumer) pass frames through a lock-free ring buffer
pool. Each `VideoFrame` carries two buffers: `raw` (the untouched camera frame,
written once by the producer) and `work` (what effects write and the consumer sends
out). Neighborhood operations like Sobel read `raw` and write `work` in one pass with
no risk of a filter consuming its own output; point operations (tint, threshold, ...)
copy `raw` into `work` first, then mutate it in place. A fourth thread watches the
config file via `inotify` and publishes a new snapshot atomically on every change;
`effects_loop` reads one snapshot per frame, so a config push can never land
mid-frame.

## Requirements

- A V4L2 camera that supports MJPEG capture with **4:2:2 chroma subsampling** (this is
  what most USB UVC webcams provide; verify with
  `v4l2-ctl --list-formats-ext -d /dev/video0` if unsure — see
  [Known limitations](#known-limitations)).
- `v4l2loopback` (kernel module).
- `libturbojpeg`, `meson`, `ninja`, a C23 compiler (gcc or clang), OpenMP (`libgomp`,
  ships with gcc).
- Debian / Raspberry Pi OS / Ubuntu for `install.sh`. Other distros: install the
  equivalent packages by hand and build with meson directly (see below).

## Install

```sh
git clone <this repo>
cd electric-eye
sudo ./install.sh
```

This installs dependencies, builds the project in place, configures `v4l2loopback` to
load automatically on every boot with the right options, adds you to the `video`
group, and installs + enables the systemd service. Safe to re-run (e.g. after
`git pull`, as an update path) — every step is idempotent.

If you weren't already in the `video` group, log out and back in (or reboot) before
running `eeye` by hand from a terminal. The systemd service doesn't need that, since
it picks up group membership fresh on every start.

## Manual build

```sh
meson setup builddir
meson compile -C builddir
./builddir/eeye [config_path]
```

`config_path` defaults to `eeye_config.json` in the current directory. The camera
device (`/dev/video0`), output device (`/dev/video10`), resolution (1280×720), and
requested framerate (30fps, best-effort) are currently compile-time constants in
`src/eeye.c` / `src/video_threads.c` — edit and rebuild to change them.

## Configuring effects

Edit `eeye_config.json` (or whatever path you passed as `config_path`) while `eeye` is
running. All keys are optional; anything missing takes the default shown:

```json
{
  "effect": "sobel",
  "threshold_value": 128,
  "tint_u": 90,
  "tint_v": 150,
  "tint_strength": 180
}
```

| Key | Used by | Range | Meaning |
|---|---|---|---|
| `effect` | — | `none`, `grayscale`, `invert`, `threshold`, `tint`, `sobel` | Which effect is active. Default `sobel`. |
| `threshold_value` | `threshold` | 0–255 | Luma cutoff — below goes black, at/above goes white. |
| `tint_u` | `tint` | 0–255 | Target chroma U (blue–yellow axis, 128 = neutral). |
| `tint_v` | `tint` | 0–255 | Target chroma V (red–green axis, 128 = neutral). |
| `tint_strength` | `tint` | 0–255 | Blend strength: 0 = no change, 255 = fully replaced. |

Two tint presets to try:

```json
{ "effect": "tint", "tint_u": 90,  "tint_v": 150, "tint_strength": 180 }   // sepia
{ "effect": "tint", "tint_u": 190, "tint_v": 100, "tint_strength": 140 }   // blue
```

**Only one effect runs at a time** — `effect` selects it, the other keys are ignored
unless that specific effect uses them. Chaining multiple effects in one pass isn't
implemented yet.

**Write contract:** write to a temp file in the same directory, then `rename()` it
onto the target path (atomic from a reader's point of view). A plain in-place
overwrite also works — `eeye` watches for both — but rename is what guarantees a
reader never observes a half-written file. A malformed or unparseable config is
rejected with a logged reason, and the pipeline keeps running on whatever config was
last valid.

## Viewing the output

Point any app that can open a webcam at the virtual camera device (`/dev/video10` by
default):

```sh
ffplay /dev/video10
```

Or select it as the camera source in a browser, Zoom, OBS, etc.

## Running as a service

`install.sh` enables this automatically. To manage it directly:

```sh
systemctl status eeye      # is it running?
journalctl -u eeye -f      # follow logs
systemctl restart eeye     # restart
systemctl stop eeye        # stop (graceful — same shutdown path as Ctrl+C)
systemctl disable eeye     # stop starting on boot
```

The unit restarts the process automatically on anything other than an intentional
`systemctl stop`, capped at 5 restarts within 60 seconds so a persistent problem
(rather than a transient one) shows up as a failed service instead of looping
silently forever. A missing or disconnected *camera* doesn't need this — `eeye`
retries that internally without a restart.

## Known limitations

- **Requires 4:2:2 MJPEG from the camera.** Verified at startup by decoding one real
  frame; if your camera is natively 4:2:0 (common on cheap UVC hardware) or another
  subsampling, `eeye` fails clearly at launch rather than producing corrupted video.
  A general chroma-resampling fallback isn't implemented.
- **No Huffman-table (DHT) injection.** Some UVC cameras omit the Huffman table from
  their MJPEG stream, which some decoders require. If every frame fails to decode
  with a libjpeg-turbo error about a missing Huffman table, that's this — a fixable,
  contained gap in `src/v4l2_in.c`, not something already handled.
- **One effect at a time**, not a chain.
- **Resolution, framerate, and both device paths are compile-time constants**, not
  runtime-configurable or auto-negotiated against what the camera actually supports.
- **No NEON.** Effects run in plain C (parallelized via OpenMP, not vectorized).
  Measured cost is low enough on a Pi 5 that this hasn't been a bottleneck.

## Development

```sh
meson setup builddir       # once, or after meson.build changes
meson compile -C builddir  # or: just build
```

Project layout: `src/` is a flat set of translation units, no subdirectories — camera
capture (`v4l2_in`), virtual-cam output (`v4l2_out`), the frame ring buffer, the
`VideoFrame` raw/work pool, effects (`conv.c` for Sobel, `point_opps.c` for everything
else), and config hot-reload (`config.c`) are each self-contained modules wired
together in `video_threads.c` / `eeye.c`.
