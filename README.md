# Electric Eye

A realtime webcam effects pipeline for Linux, built for a submersible drone (ROV):
captures from a V4L2 camera, runs the frame through a chain of effects, and outputs
to a [v4l2loopback](https://github.com/umlaeute/v4l2loopback) virtual camera that any
app (browser, Zoom, `ffplay`, ...) can pick up like a normal webcam — plus an optional
tethered web UI for live preview and remote control.

Built and tested targeting a Raspberry Pi 5 with a USB UVC webcam, but nothing in
`src/` is Pi-specific beyond the install script's package manager assumption.

## Features

- **Direct V4L2 I/O on both ends** — no `ffmpeg` subprocess anywhere, at build or run
  time. Capture uses MMAP streaming (`REQBUFS`/`QBUF`/`DQBUF`) and decodes MJPEG
  straight from the driver's DMA buffer via libjpeg-turbo, with zero intermediate
  copies. Output writes directly to the loopback device.
- **Chained effects**, applied in any order you like, each independently
  parameterized: `sobel` (edge detection), `blur`, `grayscale`, `invert`, `threshold`,
  `tint`, `none`. Consecutive point ops (grayscale/invert/threshold/tint) fuse into a
  single lookup-table pass rather than running as separate loops over the frame.
- **NEON-accelerated** on AArch64 for `sobel`, `blur`, and `tint` (falls back to plain
  C, still OpenMP-parallelized, everywhere else).
- **Hot-reloadable JSON config.** Edit the config file — locally, or have a script on
  another device write it over the network — and the running pipeline picks up the
  change within ~200ms, no restart. A sibling `.status` file reports back whether each
  attempt was actually accepted (see [Configuring effects](#configuring-effects)).
- **Optional live-preview streaming + a topside web UI**, for exactly the case this
  project was built for: the drone is somewhere you can't be, connected by a tether.
  See [Live preview + web control](#live-preview--web-control).
- **OpenMP-parallelized effects.** Every hot loop runs across all available cores.
- **Ring buffer diagnostics.** Occupancy high-water marks and stall counters for all
  three internal frame rings, logged periodically and once more at shutdown.
- **Graceful shutdown** on Ctrl+C or `systemctl stop`, and **automatic camera
  reconnect** if the camera isn't plugged in yet or gets disconnected mid-session —
  the pipeline keeps running and picks it back up rather than dying.
- **Automatic `v4l2loopback` load/unload.** `eeye` loads the module itself on startup
  if it isn't already loaded, and unloads it again on exit if it was the one that
  loaded it — no more `sudo modprobe` by hand before every run.
- **One-command install** (`install.sh`) and a **systemd service** for running
  unattended: starts on boot, restarts on failure.

## How it works

```
 camera (MJPEG)                                          v4l2loopback
      │                                                        ▲
      ▼                                                        │
┌─────────────┐   raw    ┌──────────────┐   work    ┌──────────────────┐
│ producer    │ ───────► │ effects      │ ────────► │ consumer         │
│ (v4l2_in)   │  frame   │ (effect_chain│  frame    │ (v4l2_out, plus  │
└─────────────┘  pool    │  /conv/      │  pool     │  record + stream │
                          │  point_opps) │           │  taps)           │
                          └──────────────┘           └──────────────────┘
                                 ▲                             │
                                 │ live snapshot, once per frame│ JPEG, throttled
                          ┌──────────────┐        inotify       ▼
                          │ config watch │ ◄───── eeye_config.json   TCP socket
                          └──────────────┘                          (stream_server)
```

Three threads (producer/effects/consumer) pass frames through a lock-free ring buffer
pool, plus a fourth (`stats_loop`) that periodically logs ring diagnostics. Each
`VideoFrame` carries three buffers: `raw` (the untouched camera frame, written once by
the producer), `work` (what the effect chain writes and the consumer sends out), and
`spare` (so a chain with more than one neighborhood op — e.g. blur then sobel — can
ping-pong between passes without a filter ever consuming its own output). Point
operations (tint, threshold, ...) mutate `work` in place; consecutive point ops in a
chain are fused into one lookup-table pass rather than looping over the frame once per
stage. A fifth thread watches the config file via `inotify` and publishes a new
snapshot atomically on every change; `effects_loop` reads one snapshot per frame, so a
config push can never land mid-frame.

## Requirements

- A V4L2 camera that supports MJPEG capture with **4:2:2 chroma subsampling** (this is
  what most USB UVC webcams provide; verify with
  `v4l2-ctl --list-formats-ext -d /dev/video0` if unsure — see
  [Known limitations](#known-limitations)).
- `v4l2loopback` (kernel module) — `eeye` loads it itself, but loading a kernel module
  needs `CAP_SYS_MODULE`; see [Loading v4l2loopback](#loading-v4l2loopback) below.
- `libturbojpeg`, `meson`, `ninja`, a C23 compiler (gcc or clang), OpenMP (`libgomp`,
  ships with gcc).
- Python 3 (standard library only, nothing to `pip install`) if you want the optional
  web UI — see [Live preview + web control](#live-preview--web-control). Not needed
  for the core pipeline at all.
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
device (`/dev/video0`), output device (`/dev/video10`), stream port (`9000`),
resolution (1280×720), and requested framerate (30fps, best-effort) are currently
compile-time constants in `src/eeye.c` / `src/video_threads.c` — edit and rebuild to
change them.

### Loading v4l2loopback

`eeye` checks whether `v4l2loopback` is already loaded and, if not, loads it itself
via `modprobe` — this needs `CAP_SYS_MODULE`, which a plain user account (even one in
the `video` group) doesn't have. Two ways to satisfy that:

- Run as root: `sudo ./builddir/eeye eeye_config.json`.
- Under `eeye.service` (what `install.sh` sets up), the unit grants just
  `AmbientCapabilities=CAP_SYS_MODULE` rather than running the whole daemon as root.

If the module was already loaded before `eeye` started (by you, or a previous run),
`eeye` leaves its parameters exactly as they were and won't unload it on exit either —
it only unloads a module it loaded itself, so it can't pull the device out from under
some other consumer that set it up independently.

## Configuring effects

Edit `eeye_config.json` (or whatever path you passed as `config_path`) while `eeye` is
running. Every key is optional; anything missing takes the default shown:

```json
{
  "chain": [
    {"effect": "blur"},
    {"effect": "sobel"},
    {"effect": "tint", "tint_u": 90, "tint_v": 150, "tint_strength": 180}
  ],
  "record_path": "",
  "stream_frame_interval": 0,
  "stream_quality": 60
}
```

`"chain"` is a list of stages, **applied in order** — reorder it and you get a
different result (blur-then-sobel looks different from sobel-then-blur). Each stage is
one of `none`, `grayscale`, `invert`, `threshold`, `tint`, `sobel`, `blur`, `contrast`,
`light`, plus that effect's own parameters:

| Key | Used by | Range | Meaning |
|---|---|---|---|
| `threshold_value` | `threshold` | 0–255 | Luma cutoff — below goes black, at/above goes white. |
| `tint_u` | `tint` | 0–255 | Target chroma U (blue–yellow axis, 128 = neutral). |
| `tint_v` | `tint` | 0–255 | Target chroma V (red–green axis, 128 = neutral). |
| `tint_strength` | `tint` | 0–255 | Blend strength: 0 = no change, 255 = fully replaced. |
| `sobel_threshold` | `sobel` | 0–255 | Gradient magnitudes below this are clamped to 0 (raises the bar for what counts as an edge). |
| `blur_strength` | `blur` | 0–255 | How many times to repeat the 5-tap blur pass; 0 and 1 both mean a single pass. Cost is linear in this value but the visible effect isn't (repeated small-kernel blur's effective radius grows with the square root of the pass count), so past ~20 you're mostly paying for frame time, not more blur — the web UI's slider caps there for that reason; edit the JSON directly for higher. |
| `light_level` | `light` | 0–255 | One dial over brightness *and* saturation together; 128 = neutral (default). Below darkens/desaturates toward black, above brightens/boosts saturation. |

`contrast` takes no parameters — it's a full-frame auto min/max luma stretch,
recomputed fresh from whatever the chain has produced so far every single frame, so
there's nothing to tune beyond whether it's in the chain or not. Unlike `contrast`,
`light` doesn't inspect the actual frame — its `light_level` value alone determines
the whole transform, which is what lets it fold into the same fast LUT-based pass as
`grayscale`/`invert`/`threshold`/`tint` instead of needing its own full-frame scan.
`light_level` is also the one field in this table that defaults to 128, not 0, when
omitted — every other optional field's zero-init default already happens to mean "no
change" for its effect, but 0 for `light_level` would mean "fully dark and
desaturated," so it's special-cased to default to neutral instead (see
`parse_effect_stage` in `src/config.c`).

At `blur_strength` 4 and above, `blur` internally downsamples to half resolution,
runs its repeated passes there, then upsamples back — a quarter as many pixels per
pass for a softening that's already hard to tell apart from the full-resolution
version at that pass count. Below 4 it stays full-resolution, since the fixed cost of
that resize isn't worth paying when there's only one or two passes to save it on. See
`blur_plane_repeated_auto` in `src/conv.c`.

A stage carrying a key its effect doesn't use (e.g. `tint_u` on a `blur` stage) is a
hard parse error, not silently ignored — a typo should be loud, not a quiet no-op. An
empty chain (`"chain": []`) is a valid pass-through. Two tint presets to try:

```json
{ "effect": "tint", "tint_u": 90,  "tint_v": 150, "tint_strength": 180 }   // sepia
{ "effect": "tint", "tint_u": 190, "tint_v": 100, "tint_strength": 140 }   // blue
```

`"record_path"` — writes the **untouched** camera frame (independent of the effect
chain) to this path as raw I422, no container, back to back. Empty (or omitted) means
off. This is genuinely large with no compression — ~53MB/s, ~190GB/hour at 1280×720 —
fine for short clips; play back with (matching your resolution/framerate):

```sh
ffplay -f rawvideo -pix_fmt yuv422p -s 1280x720 -r 30 -i FILE
```

`"stream_frame_interval"` / `"stream_quality"` — the live-preview stream tap; see
[Live preview + web control](#live-preview--web-control).

**Writer contract:** write to a temp file in the same directory, then `rename()` it
onto the target path (atomic from a reader's point of view). A plain in-place
overwrite also works — `eeye` watches for both — but rename is what guarantees a
reader never observes a half-written file. A malformed or unparseable config is
rejected with a logged reason, and the pipeline keeps running on whatever config was
last valid.

**Reload feedback:** after every load attempt, `eeye` writes a sibling
`<config_path>.status` file (same atomic-rename contract) containing `{"ok":true}` or
`{"ok":false}` — whether that attempt is what's actually running now. This is what
lets a remote writer (like `pi/config_agent.py` below) confirm a config actually took
effect instead of just confirming it wrote a file.

## Live preview + web control

Two small, optional, stdlib-only Python scripts — `pi/config_agent.py` and
`topside/web_ui.py` (aka the Hellion) — turn this into a tethered control station,
which is the actual use case this project targets: a drone underwater, connected by
a cable to a topside operator.

```
   drone (Pi)                                    topside (any machine)
┌─────────────────────┐                        ┌──────────────────────────┐
│ eeye                │──JPEG, throttled──────► │                          │
│  stream_server :9000│    (TCP socket)         │  topside/web_ui.py       │──► browser
│                      │                         │   (the Hellion) :8080    │
│ pi/config_agent.py   │◄──config JSON──────────│                          │
│  :9001               │   (HTTP, proxied)       └──────────────────────────┘
└─────────────────────┘
```

**On the drone**, alongside `eeye` (same config path both were started with):

```sh
python3 pi/config_agent.py --config-path eeye_config.json
```

**On the topside machine:**

```sh
python3 topside/web_ui.py --drone-host <drone's IP over the tether>
```

Then open `http://<topside machine's IP>:8080/` in a browser. The Hellion's page shows
the live feed and a form for the effect chain, recording path, and stream settings;
hitting Apply sends your changes to `config_agent.py`, which writes them to
`eeye_config.json` and waits for `eeye`'s own `.status` file to confirm whether they
actually took effect — the UI tells you definitively "applied," "rejected" (check
`eeye`'s own log for why), or "no confirmation" (is `eeye` even running?), not just
"the network request succeeded."

The browser only ever talks to the Hellion; it proxies the config read/write to
`config_agent.py` server-to-server, so there's no CORS to deal with and the drone's own
ports don't need to be reachable from whatever network a browser happens to be on.

The stream tap is off by default (`stream_frame_interval: 0` — no JPEG work happens at
all until you turn it on, from the form or the config file directly). It's a lossy,
throttled *preview* only, independent of `record_path`'s full-quality local recording
— nothing valuable rides on the network stream, so it's fine to push the quality/rate
down hard over a constrained link, or up if your tether has the bandwidth (see the
comments in `src/stream_server.c`).

**No authentication on any of this.** Fine on a private point-to-point tether; would
not be fine on a shared or untrusted network — don't expose these ports beyond that
without adding some.

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
- **No Huffman-table (DHT) injection gap** — actually handled: some UVC cameras omit
  the Huffman table from their MJPEG stream, and `src/v4l2_in.c` splices in the
  standard tables when that's detected. Mentioned here only because it's the kind of
  thing worth knowing exists if you're debugging a decode failure on a new camera.
- **Resolution, framerate, and both device paths are compile-time constants**, not
  runtime-configurable or auto-negotiated against what the camera actually supports.
- **Live-preview stream is single-viewer.** `stream_server` accepts one connection at a
  time (last one wins); fine for one `web_ui.py` instance, not a fan-out broadcast.
- **No authentication** on the stream or config-agent ports — see
  [Live preview + web control](#live-preview--web-control).
- **The web UI's chain-editing form has only been tested on a local network loopback
  setup, not yet over a real tether or against real Pi 5 hardware.**

## Development

```sh
meson setup builddir       # once, or after meson.build changes
meson compile -C builddir  # or: just build
```

Project layout: `src/` is a flat set of translation units, no subdirectories — camera
capture (`v4l2_in`), virtual-cam output (`v4l2_out`), the live-preview stream tap
(`stream_server`), `v4l2loopback` load/unload (`virtual_cam`), the frame ring buffer,
the `VideoFrame` raw/work/spare pool, effects (`conv.c` for Sobel/blur, `point_opps.c`
for grayscale/invert/threshold/tint), the chain runner (`effect_chain.c`), and config
hot-reload (`config.c`) are each self-contained modules wired together in
`video_threads.c` / `eeye.c`. `pi/` and `topside/` are standalone Python scripts, not
part of the meson build — see [Live preview + web control](#live-preview--web-control).
