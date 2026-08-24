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
- **Both camera kinds**: USB/UVC webcams over V4L2, and Raspberry Pi camera modules
  (CSI) via `rpicam-vid`. Auto-detected by default, or pin one with `capture_source`.
  See [Pi camera modules](#pi-camera-modules).
- **Chained effects**, applied in any order you like, each independently
  parameterized: `sobel` (edge detection), `blur`, `grayscale`, `invert`, `threshold`,
  `tint`, `none`. Consecutive point ops (grayscale/invert/threshold/tint) fuse into a
  single lookup-table pass rather than running as separate loops over the frame.
- **NEON-accelerated** on AArch64 for `sobel`, `blur`, and `contrast`'s min/max scan
  (falls back to plain C, still OpenMP-parallelized, everywhere else — including the
  fused point-op LUT chain, where a plain cache-friendly lookup beats a NEON gather).
- **Hot-reloadable JSON config.** Edit the config file — locally, or have a script on
  another device write it over the network — and the running pipeline picks up the
  change within ~200ms, no restart. A sibling `.status` file reports back whether each
  attempt was actually accepted (see [Configuring effects](#configuring-effects)).
- **Configurable, auto-negotiated resolution with an integer downscale.** Ask for a
  resolution and `eeye` falls back to the closest mode the camera actually offers
  (aspect ratio first) rather than refusing to start — then optionally runs the entire
  pipeline at ½, ¼, or ⅛ of it, the single biggest lever on frame cost and tether
  bandwidth. On MJPEG the scaling happens *inside* the JPEG decode, so the decode
  itself gets cheaper too. See
  [Resolution and downscaling](#resolution-and-downscaling).
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

- **A camera**, either of:
  - A **Raspberry Pi camera module** (the CSI ribbon-cable kind), which needs
    `rpicam-apps` installed — it ships by default on Raspberry Pi OS. See
    [Pi camera modules](#pi-camera-modules).
  - A **V4L2 camera** (USB/UVC webcam) offering either MJPEG with **4:2:2 or 4:2:0
    chroma subsampling** or **YUYV**, at the exact resolution you configure. MJPEG is
    preferred where available and YUYV is the automatic fallback — which matters
    because many webcams offer MJPEG only at their higher resolutions, so picking a
    smaller `capture_width`/`capture_height` can land you on YUYV. Both paths are
    fully supported, including downscaling. Check what yours offers with
    `v4l2-ctl --list-formats-ext -d /dev/video0`, and see
    [Known limitations](#known-limitations).
- `v4l2loopback` (kernel module) — `eeye` loads it itself, but loading a kernel module
  needs `CAP_SYS_MODULE`; see [Loading v4l2loopback](#loading-v4l2loopback) below.
- `meson`, `ninja`, a C23 compiler (gcc or clang), OpenMP (`libgomp`, ships with gcc).
  `libturbojpeg` is used if the system has it (`libturbojpeg0-dev` on Debian/Ubuntu),
  but isn't required — if meson can't find it, it builds a static copy from source
  automatically, so there's nothing to install by hand if that package is missing,
  wrongly named, or the runtime `.so` is out of sync with the dev headers.
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

`config_path` defaults to `eeye_config.json` in the current directory. Resolution and
downscaling come from that config file — see
[Resolution and downscaling](#resolution-and-downscaling). The camera device
(`/dev/video0`), output device (`/dev/video10`), stream port (`9000`), and requested
framerate (30fps, best-effort) are still compile-time constants in `src/eeye.c` /
`src/video_threads.c` — edit and rebuild to change those.

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
  "stream_quality": 60,
  "capture_width": 1280,
  "capture_height": 720,
  "downscale": 1
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
fine for short clips.

"Untouched" means untouched *by the effect chain*; `"downscale"` still applies, because
it happens in the capture decode before the recording tap sees the frame. So the file's
dimensions are `capture ÷ downscale`, not `capture_width` × `capture_height` — which
also means downscaling shrinks recordings proportionally (~48GB/hour at `downscale: 2`).
Play back with those dimensions and your framerate:

```sh
# 1280x720 capture with "downscale": 1
ffplay -f rawvideo -pix_fmt yuv422p -s 1280x720 -r 30 -i FILE
# ...the same capture with "downscale": 2
ffplay -f rawvideo -pix_fmt yuv422p -s 640x360 -r 30 -i FILE
```

`"stream_frame_interval"` / `"stream_quality"` — the live-preview stream tap; see
[Live preview + web control](#live-preview--web-control).

### Resolution and downscaling

`"capture_width"` / `"capture_height"` (default 1280×720) are what `eeye` asks the
camera for. If the camera doesn't offer that exact mode, `eeye` enumerates what it
*does* offer (across both MJPEG and YUYV) and picks the closest usable one, logging
the substitution at startup:

```
v4l2_in: /dev/video0 does not offer 1920x1080; using 1280x720 instead
         (closest available at the same aspect ratio, and divisible by downscale 2)
```

Closest means **aspect ratio first, pixel count second** — a 4:3 stand-in for a 16:9
request re-frames every shot, which is a worse surprise than the same framing at
fewer pixels. Candidates that would break the downscale divisibility rule below are
excluded outright. An exact match is always chosen, silently.

This is best-effort, not a guarantee: it needs the camera present at startup to have
anything to enumerate. If it isn't plugged in yet, the configured size stays in force
and the normal reconnect loop takes over. To see the modes yourself:

```sh
v4l2-ctl --list-formats-ext -d /dev/video0
```

`"downscale"` (default 1; must be **1, 2, 4, or 8**) shrinks the frame once, on the
way out of the capture decode, before anything else touches it. Everything downstream
— the frame pool, the whole effect chain, the v4l2loopback output, and the preview
JPEG — then runs at `capture ÷ downscale`. It is by far the cheapest way to buy
headroom for an expensive chain. Measured on the x86_64 dev laptop, capturing 1280×720
MJPEG with a `blur_strength: 12` + `sobel` chain, all at a steady 30fps:

| `downscale` | pipeline | CPU | preview frame |
|---|---|---|---|
| 1 | 1280×720 | 134% | 59.4 KB |
| 2 | 640×360 | 55% | 16.4 KB |
| 4 | 320×180 | 34% | 5.1 KB |
| 8 | 160×90 | 28.5% | 1.9 KB |

It's restricted to those four values rather than an arbitrary target resolution
because both capture paths need the ratio to be exact. On the MJPEG path the divisor
is handed to libjpeg-turbo, which scales *during* decompression by discarding
high-frequency DCT coefficients — so the decode itself gets cheaper, not just the work
after it — but it supports only a fixed set of ratios and, asked for one it doesn't
support, silently returns the largest size that fits rather than failing. On the YUYV
path each output pixel is a box average of an N×N source block, which only divides
evenly for integer N. Widths must be a multiple of `2 × downscale` (so the I422 chroma
planes stay exactly half-width) and heights a multiple of `downscale`; anything else
is rejected at startup, with the required multiples named.

> **These three keys are the only ones that are not hot-reloadable.** They're read once
> at startup. Changing them in a running config is detected and logged, but ignored
> until you restart `eeye` — applying them live would mean reallocating the frame pool
> while three threads hold frames from it, and v4l2loopback can't change format at all
> while a viewer has the device open.

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

### Pi camera modules

Raspberry Pi camera modules (the CSI ribbon-cable ones) work, but they don't go
through V4L2 the way a USB webcam does. On Pi 5 the `rp1-cfe` driver exposes the
sensor as **raw Bayer only** — there's no MJPEG or YUYV node to open, and converting
Bayer into a viewable image (black level, demosaic, white balance, lens shading, tone
mapping) is the ISP's job, driven by libcamera. Raspberry Pi's own guidance is not to
drive Pi 5 cameras through V4L2 directly.

So `eeye` uses the supported path: it spawns `rpicam-vid`, asks for MJPEG on stdout,
and decodes that. This gets the whole ISP chain and the per-sensor tuning files for
free. It uses MJPEG rather than `--codec yuv420` deliberately — the raw YUV output is
stride-padded (Y rows to a multiple of 64 bytes, U/V to 32) with no framing in the
stream, which is a well-known source of "corrupt raw output" reports; MJPEG is
self-delimiting and carries its own dimensions, so a frame either decodes correctly or
fails loudly.

`"capture_source"` picks the backend:

| Value | Behavior |
| --- | --- |
| `"auto"` (default) | Runs `rpicam-vid --list-cameras`; uses the Pi camera if one is found, else falls back to V4L2. |
| `"rpicam"` | Always the Pi camera module. Fails at startup if there isn't one. |
| `"v4l2"` | Always a V4L2 device. Skips the probe entirely. |

Pin it explicitly on a vehicle carrying **both** a CSI camera and a USB webcam —
otherwise which one you get depends on probe order rather than on what you meant.

```json
{
  "chain": [{"effect": "sobel"}],
  "capture_width": 1280,
  "capture_height": 720,
  "downscale": 2,
  "capture_source": "rpicam"
}
```

Like the geometry keys, `capture_source` is read **once at startup** — changing it in
a running config is logged and ignored.

Two differences from the V4L2 path worth knowing:

- **No resolution negotiation.** The ISP scales to whatever geometry it's asked for
  rather than offering a fixed mode list, so there's nothing to enumerate. An
  unsupported geometry fails at the startup probe instead of being quietly substituted.
- **`rpicam-vid`'s MJPEG is 4:2:0**, not the 4:2:2 most webcams emit. `eeye` handles
  both (see [Known limitations](#known-limitations)); the practical effect is that
  chroma is line-doubled during decode.


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
python3 topside/web_ui.py --drone-host auto
```

`auto` finds the drone on whatever cable is plugged in — see
[Direct-cable tether](#direct-cable-tether). Pass an explicit
`--drone-host <address>` instead if you already know it, or if more than one
drone is on the link.

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

Both drone-side ports listen on IPv6 and IPv4 from one socket, so they work over a
link-local address on a bare cable and over ordinary IPv4 on a LAN, with no
switch between the two.

**No authentication on any of this.** Fine on a private point-to-point tether; would
not be fine on a shared or untrusted network — don't expose these ports beyond that
without adding some.

### Direct-cable tether

The deployment case this targets has **no topside network at all**: the drone's
Pi and the topside machine are joined by one Ethernet cable, with no router, no
DHCP server, and no DNS. Nothing needs configuring for that to work.

Both of the drone's ports listen on IPv6 as well as IPv4, and every host
self-assigns an IPv6 link-local address (`fe80::/64`) on any live link with no
server involved — so a bare cable is already a working network the moment both
ends are powered. `tools/eeye-net` finds the drone over it:

```sh
tools/eeye-net discover
```

```
Searching for a drone on: enp0s31f6
  searching enp0s31f6...
    found a drone at fe80::ba27:ebff:fe4a:1c2d%enp0s31f6

[  ok  ] drone at fe80::ba27:ebff:fe4a:1c2d%enp0s31f6 (via enp0s31f6)
[  ok  ] video stream delivering frames (17629 bytes, 640x480)

  Start the topside UI with:
    python3 topside/web_ui.py --drone-host 'fe80::ba27:ebff:fe4a:1c2d%enp0s31f6'
  or let it find the drone itself:
    python3 topside/web_ui.py --drone-host auto
```

The address is the drone's own link-local one; the `%interface` suffix is part
of it, not decoration — every link has its own `fe80::/64`, so the interface is
what disambiguates which cable to use. Keep the quotes: `%` is a shell
metacharacter in some contexts.

#### Checking the link

```sh
tools/eeye-net check            # discovers a drone, then checks it
tools/eeye-net check <address>  # check one specific drone
```

It walks the whole path — link carrier, control channel, video, and whether the
two drone-side processes actually agree on a config file — and prints the exact
command to fix whatever it finds. It **only reads**: nothing it does changes the
drone, the config, or this machine's network settings, so it is safe to run at
any time, including mid-dive.

Exit codes suit a pre-dive checklist: `0` healthy, `1` degraded, `2` no drone
found.

Two failures it exists to catch, because neither is visible any other way:

- **The video port accepts your connection and then sends nothing.** That is
  what `"stream_frame_interval": 0` (the default — the tap ships off) looks like
  from outside. A plain port check calls it healthy while you stare at a black
  screen. `check` reads an actual frame, so it can tell "connected but silent"
  from "nothing listening" and names which.
- **`config_agent` and `eeye` given different config paths.** Your edits then
  land in a file `eeye` never reads: the UI reports success and nothing happens.
  Both processes look perfectly healthy — the agent's own reply is byte-identical
  to the healthy case. `check` cross-checks the config's declared geometry
  against the live stream's actual dimensions, which disagree exactly when the
  paths do.

#### One-time setup, if you want it

Not required for the above — link-local needs no setup at all — but useful if
IPv4-only tools are involved, or you want the drone reachable by name:

```sh
tools/eeye-net fix
```

prints (never runs) the `nmcli`, hostname, and mDNS commands for both ends.
The important detail it encodes: create the profile with `ifname '*'` rather
than binding it to one interface name, or it silently stops applying the day
the port changes — a USB Ethernet adapter and a built-in port have different
names.


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

- **MJPEG capture must be 4:2:2 or 4:2:0.** Verified at startup by decoding one real
  frame; any other subsampling fails clearly at launch rather than producing corrupted
  video. 4:2:2 decodes straight into the frame with no conversion; 4:2:0 (what
  `rpicam-vid` produces, and what some cheap UVC hardware uses) has its half-height
  chroma line-doubled during decode, which costs one extra pass over the chroma planes
  and discards nothing the encoder hadn't already discarded. YUYV is packed 4:2:2 by
  definition, so the fallback path is unaffected.
- **No Huffman-table (DHT) injection gap** — actually handled: some UVC cameras omit
  the Huffman table from their MJPEG stream, and `src/v4l2_in.c` splices in the
  standard tables when that's detected. Mentioned here only because it's the kind of
  thing worth knowing exists if you're debugging a decode failure on a new camera.
- **Framerate and both device paths are compile-time constants.** Resolution is
  configurable and negotiated (see
  [Resolution and downscaling](#resolution-and-downscaling)).
- **Pi camera capture goes through `rpicam-vid`, not V4L2 directly.** On Pi 5 the
  `rp1-cfe` driver exposes the CSI sensor as raw Bayer only, with no MJPEG or YUYV
  mode to negotiate, and turning Bayer into an image is the ISP's job via libcamera.
  So that path spawns `rpicam-vid` and reads MJPEG from a pipe. Consequences:
  `capture_source` is startup-only, `VIDIOC_ENUM_FRAMESIZES` negotiation doesn't
  apply (the ISP scales to whatever it's asked for, so an unsupported geometry fails
  at the probe instead of being substituted), and camera controls beyond resolution
  and framerate aren't plumbed through.
- **Resolution is negotiated once, at startup.** If the camera is unplugged at launch
  there's nothing to enumerate, so the configured size stays in force and `eeye`
  retries with it; a *different* camera plugged in later is then held to that size
  rather than renegotiated. Replugging the same camera is unaffected.
- **The web UI's geometry display reads the config file**, so it shows the size you
  asked for, not the one negotiated with the camera. Check `eeye`'s startup log
  (`journalctl -u eeye`) for the size actually in use.
- **Geometry changes need a restart.** `capture_width`/`capture_height`/`downscale`
  are read once at startup; changing them in a running config is logged and ignored.
  `capture_source` is startup-only for the same reason.
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
for `contrast`'s min/max-scan-and-stretch), the chain runner (`effect_chain.c` — also
where grayscale/invert/threshold/tint/light actually live, fused into LUTs; see
[Configuring effects](#configuring-effects)), and config hot-reload (`config.c`) are
each self-contained modules wired together in
`video_threads.c` / `eeye.c`. `pi/` and `topside/` are standalone Python scripts, not
part of the meson build — see [Live preview + web control](#live-preview--web-control).
