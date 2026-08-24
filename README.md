# Electric Eye

Realtime camera effects pipeline for Linux, built for a submersible drone (ROV).
Captures from a camera, runs frames through a chain of effects, and publishes the
result as a [v4l2loopback](https://github.com/umlaeute/v4l2loopback) virtual camera
any app can open — plus an optional tethered web UI for live preview and remote
control.

Runs on a Raspberry Pi with either a USB webcam or a Pi camera module. Nothing in
`src/` is Pi-specific beyond `install.sh`'s package manager.

- [Quick start](#quick-start) · [Field checklist](#field-checklist) ·
  [Troubleshooting](#troubleshooting) · [Config reference](#config-reference)

## Quick start

```sh
git clone <this repo>
cd electric-eye
sudo ./install.sh          # deps, build, v4l2loopback on boot, systemd service
```

Idempotent — re-run it after `git pull` as the update path. If you weren't already
in the `video` group, log out and back in before running `eeye` by hand.

```sh
ffplay /dev/video10        # watch the output
journalctl -u eeye -f      # watch the logs
```

Edit `eeye_config.json` while it runs; changes apply in ~200ms with no restart.

### Manual build

```sh
meson setup builddir && meson compile -C builddir
./builddir/eeye [config_path]     # default: ./eeye_config.json
```

Needs `meson`, `ninja`, a C23 compiler, and OpenMP. `libturbojpeg` is used if
present and built from source automatically if not.

Running by hand needs `CAP_SYS_MODULE` to load v4l2loopback — use `sudo`, or
pre-load the module yourself:

```sh
sudo modprobe v4l2loopback video_nr=10 card_label=VirtualCam exclusive_caps=1
```

The systemd unit grants just that one capability instead of running as root.
`eeye` only unloads a module it loaded itself, so a pre-loaded one is left alone.

## Field checklist

The tether is a **direct Ethernet cable** — no router, no DHCP, no DNS. Nothing
needs configuring: both drone ports listen on IPv6 and IPv4, and every host
self-assigns an IPv6 link-local address on any live link.

**On the drone** (same config path `eeye` was started with — see
[wrong config path](#i-change-settings-and-nothing-happens)):

```sh
python3 pi/config_agent.py --config-path eeye_config.json
```

**Topside:**

```sh
tools/eeye-net check                          # pre-dive: is everything talking?
python3 topside/web_ui.py --drone-host auto   # then open http://localhost:8080/
```

The page has the live feed, **Record** and **Snapshot** buttons, and the effect
chain. Recordings and stills land in `./captures` (`--output-dir` to change it).
Recording can also be driven from the command line, which is what the button
runs underneath:

```sh
tools/eeye-record --split 10m                 # a new file every 10 minutes
```

`check` walks the whole path — cable carrier, control channel, video, and whether
the two drone-side processes agree on a config file — and prints the exact fix for
whatever it finds. It **only reads**: it changes nothing on the drone or topside,
so it is safe mid-dive. Exit codes: `0` healthy, `1` degraded, `2` no drone found.

```sh
tools/eeye-net discover          # find drones, print the command to connect
tools/eeye-net fix               # print (never run) one-time nmcli/mDNS setup
```

Addresses look like `fe80::ba27:ebff:fe4a:1c2d%eth0`. The `%interface` suffix is
part of the address — every link has its own `fe80::/64` — so keep the quotes when
passing one to a shell.

## Troubleshooting

Logs are the first stop: `journalctl -u eeye -f`, or stdout if running by hand.

### No video anywhere

| Log line | Cause | Fix |
|---|---|---|
| `Camera unavailable (attempt N)` | Camera not detected. `eeye` retries forever; it does **not** need a restart. | Check the cable. `v4l2-ctl --list-devices` |
| `Permission denied opening /dev/video0` | Not in the `video` group. | `sudo usermod -aG video $USER`, then log out and back in |
| `Failed to load v4l2loopback` | Needs `CAP_SYS_MODULE`. | Run with `sudo`, or use the systemd unit |
| `Failed to load v4l2loopback` **and** Secure Boot is on | Module built but its signing key was never enrolled — the kernel reports this as the same "operation not permitted" a missing capability does. | Reboot and enroll the key at the blue MOK screen. `mokutil --list-new` shows a pending enrollment |
| `neither MJPEG nor YUYV is available at exactly WxH` | Camera doesn't offer that mode and negotiation couldn't substitute one. | `v4l2-ctl --list-formats-ext -d /dev/video0`, then set `capture_width`/`capture_height` to a listed mode |
| `is not 4:2:2 nor 4:2:0 subsampled` | Unsupported chroma format. | Try a resolution where the camera offers YUYV |

### The picture is frozen but looks live

That was a real hazard and is now surfaced. When frames stop arriving the UI
desaturates the image and shows a pulsing red banner naming how stale it is:

```
LINK LOST — showing a frozen frame from 4.0s ago
```

If you see that, you are **not** looking at live video. `tools/eeye-net check`
diagnoses which half of the link is down.

### Recording stopped without telling me

It tells you now. The sidebar shows `recording: ACTIVE — 812 MB written,
4210 MB free`, and turns red with the reason if it stops:

```
recording: FAILED — write failed: No space left on device
```

Raw I422 is ~53 MB/s at 1280×720, so a disk fills fast. `eeye` also logs how
long you have when recording starts:

```
Recording: 41000 MB free at /data/dive.raw (~780 seconds at this geometry)
```

`downscale` stretches that — it applies before the recording tap, so
`downscale: 2` quarters the rate.

### Is that real, or is it my filter?

Tick **Show raw camera (bypass effects)** in the UI, or set `"stream_raw":
true`. The preview switches to the untouched camera frame while the virtual
camera and recording keep running the full chain, and it hot-reloads like
everything else.

### Black screen in the web UI

Almost always the stream tap being off rather than a network fault — it ships off
by default. From outside, the port **accepts your connection and then sends
nothing**, so a port check looks healthy.

```sh
tools/eeye-net check <address>     # names this specific case
```

Fix: set `"stream_frame_interval"` to `2` or `3`. `0` disables the tap entirely.

### I change settings and nothing happens

`config_agent.py` and `eeye` were given **different config paths**, so your edits
land in a file `eeye` never reads. Both processes look completely healthy and the
UI reports success.

```sh
pgrep -af 'eeye|config_agent'      # on the drone — the paths must match exactly
tools/eeye-net check <address>     # detects it by cross-checking geometry
```

Also check: geometry and `capture_source` are **startup-only** (see
[Config reference](#config-reference)). Changing them is logged and ignored until
restart.

### Config rejected

`eeye` keeps running on the last valid config and logs why. An unknown key is a
hard error, not a warning — a typo should be loud:

```
Config: unknown key "bogus_key"
```

Strict JSON only: no comments, no trailing commas. Validate with
`python3 -m json.tool eeye_config.json`. Each load attempt writes
`<config_path>.status` containing `{"ok":true|false}` — that is the definitive
answer to whether a change took effect.

### Pipeline can't keep up

Set `"downscale"` to `2`. It is by far the biggest lever — see the table in
[Resolution](#resolution). Then reduce `blur_strength` (cost is linear in it, but
visible blur grows much more slowly, so past ~20 you are mostly buying frame time).

`journalctl -u eeye | grep full_stalls` — nonzero means a stage is falling behind.

### The drone won't restart cleanly

```sh
systemctl status eeye
```

The unit restarts on failure but gives up after 5 restarts in 60s, so a persistent
fault surfaces as a failed service instead of looping silently. A missing camera
does not trigger this — that is retried internally.

## Config reference

Every key is optional. Defaults shown:

```json
{
  "chain": [{"effect": "sobel"}],
  "record_path": "",
  "stream_frame_interval": 0,
  "stream_quality": 60,
  "capture_width": 1280,
  "capture_height": 720,
  "downscale": 1,
  "capture_source": "auto",
  "stream_raw": false,
  "camera": {}
}
```

> **Startup-only:** `capture_width`, `capture_height`, `downscale`,
> `capture_source`. Everything else is hot-reloadable. Changing these in a running
> config is logged and ignored — applying them live would mean reallocating the
> frame pool while three threads hold frames from it.

**Writer contract:** write a temp file in the same directory, then `rename()` onto
the target. In-place overwrite also works, but rename guarantees a reader never
sees a half-written file.

### Effects

`"chain"` is a list of stages **applied in order** — blur-then-sobel differs from
sobel-then-blur. An empty chain is a valid pass-through. Stages: `none`,
`grayscale`, `invert`, `threshold`, `tint`, `sobel`, `blur`, `contrast`, `light`,
`log`, `canny`.

| Key | Effect | Range | Meaning |
|---|---|---|---|
| `threshold_value` | `threshold` | 0–255 | Luma cutoff: below black, at/above white |
| `tint_u` | `tint` | 0–255 | Chroma U, blue–yellow (128 = neutral) |
| `tint_v` | `tint` | 0–255 | Chroma V, red–green (128 = neutral) |
| `tint_strength` | `tint` | 0–255 | 0 = no change, 255 = fully replaced |
| `sobel_threshold` | `sobel` | 0–255 | Gradients below this clamp to 0 — suppresses noise edges |
| `blur_strength` | `blur` | 0–255 | Blur passes. 0 and 1 both mean one pass. Past ~20, cost keeps rising but visible blur barely does |
| `light_level` | `light` | 0–255 | Brightness *and* saturation together. **Defaults to 128** (neutral); 0 is fully dark |
| `log_strength` | `log` | 0–255 | Gaussian passes before the Laplacian, so it sets sigma (growing as its square root). 0 and 1 both mean one pass. Higher rejects finer detail — the knob for backscatter |
| `log_threshold` | `log` | 0–255 | How steep the response must be across a zero-crossing to count as an edge. 0 marks every sign change, noise included |
| `canny_strength` | `canny` | 0–255 | Gaussian passes. 0 means 1 — the smoothing is never skipped, see below |
| `canny_low` | `canny` | 0–255 | Weak threshold; kept only where connected to a strong edge. Default 40 |
| `canny_high` | `canny` | 0–255 | Strong threshold; an edge outright. Default 90 |

`contrast` takes no parameters — it is a full-frame auto luma stretch, recomputed
every frame. A stage carrying a key its effect doesn't use is a hard parse error.

`canny` is the most selective of the three edge operators: it thins ridges to
single pixels and then keeps a weak edge **only where it connects to a strong
one**, so faint real contours survive while equally faint isolated noise does
not. Tune `canny_high` first (how obvious an edge must be to count at all), then
`canny_low` (how far a contour is followed once found). A useful starting point
is roughly a 1:2 or 1:3 ratio — the 40/90 defaults.

Its `canny_strength` is deliberately never zero. The Gaussian is not a quality
setting there: it is what bounds the cost of the connectivity pass. Measured at
1280×720, hysteresis on unsmoothed noise costs 10.8 ms against 0.5 ms after a
single 0.07 ms blur pass.

`log` is Laplacian of Gaussian, rendered as Marr–Hildreth zero-crossings: thin,
closed 1px contours rather than `sobel`'s thicker gradient ridges. Smooth first
(`log_strength`), then take the second derivative and mark where it crosses zero
(`log_threshold`). Raise `log_strength` when fine texture is drowning the edges
you care about; raise `log_threshold` when noise is producing speckle.

Sepia: `{"effect": "tint", "tint_u": 90, "tint_v": 150, "tint_strength": 180}`
Blue: `{"effect": "tint", "tint_u": 190, "tint_v": 100, "tint_strength": 140}`

#### Edge operator cost

Measured on a real 1280×720 camera frame (x86_64 dev laptop; a Pi will be
slower, but the ratios hold). The 30 fps budget is 33.3 ms/frame:

| operator | cost | output |
|---|---|---|
| `sobel` | 0.33 ms | gradient magnitude, ridges several px wide |
| `log` (strength 1) | 1.56 ms | thin closed contours, no connectivity filter |
| `canny` (strength 1) | 2.35 ms | thin contours, weak edges kept only if connected |

All three are a small fraction of the budget; pick on output, not cost. Note
`canny`'s time is content-dependent — a frame that is almost entirely noise
costs several times more, which is what `canny_strength` exists to control.

### Resolution

`capture_width`/`capture_height` are what `eeye` asks the camera for. If that exact
mode isn't offered, it enumerates what is and substitutes the closest — **aspect
ratio first, pixel count second**, since a 4:3 stand-in for a 16:9 request reframes
every shot. Exact matches are chosen silently; substitutions are logged:

```
v4l2_in: /dev/video0 does not offer 1920x1080; using 1280x720 instead
```

This needs the camera present at startup. If it isn't, the configured size stays
in force and the reconnect loop takes over.

`"downscale"` (**1, 2, 4, or 8**) shrinks the frame once, during capture decode.
Everything downstream — effects, virtual camera, preview, recording — runs at
`capture ÷ downscale`. Measured at 1280×720 with `blur_strength: 12` + `sobel`,
steady 30fps:

| `downscale` | pipeline | CPU | preview frame |
|---|---|---|---|
| 1 | 1280×720 | 134% | 59.4 KB |
| 2 | 640×360 | 55% | 16.4 KB |
| 4 | 320×180 | 34% | 5.1 KB |
| 8 | 160×90 | 28.5% | 1.9 KB |

Width must be a multiple of `2 × downscale` and height a multiple of
`2 × downscale`; anything else is rejected at startup with the required multiples
named. Only these four values are allowed because both capture paths need an exact
ratio — MJPEG scales during JPEG decompression (so the decode itself gets cheaper),
and YUYV box-averages N×N blocks.

### Camera controls

`"camera"` sets what the sensor does before any effect runs — and underwater
that matters more than anything in the chain, because no effect can recover
detail the sensor never captured. Auto exposure hunts badly in low contrast;
auto white balance gives up once everything is blue-green.

```json
"camera": {
  "auto_exposure": false,
  "exposure": 20000,
  "gain": 30,
  "auto_white_balance": false,
  "white_balance": 3200
}
```

Every field is optional, and **anything you leave out is left alone** — so a
`"camera"` block with one key changes exactly one control.

| key | range | meaning |
|---|---|---|
| `auto_exposure` | bool | Must be `false` before `exposure` takes effect |
| `exposure` | µs | Shutter time. Shorter freezes motion but needs more gain |
| `gain` | 0–100 | Percent of the sensor's own range; raises brightness and noise together |
| `auto_white_balance` | bool | Must be `false` before `white_balance` takes effect |
| `white_balance` | Kelvin | ~2800 (warm) to 6500 (cool) |
| `brightness` | −100–100 | 0 = neutral |
| `contrast` | 0–200 | 100 = neutral |
| `saturation` | 0–200 | 100 = neutral |
| `sharpness` | 0–200 | 100 = neutral |

These are **hot-reloadable** — adjust them from the UI's Camera section and the
picture changes immediately, no restart. They are also re-applied after a camera
reconnect, since a replugged camera comes back at its factory defaults.

The percent-style ranges are deliberate: real cameras disagree about units (this
project's test webcam runs brightness −64..64 and gain 0..128, another will
differ), so `eeye` scales onto whatever the device reports and clamps to it. That
means one config works across cameras and across both capture backends.

**Turn the auto mode off in the same edit as the manual value.** A camera rejects
a manual exposure while auto exposure is on, and reports it as a *permission*
error — so `eeye` translates that into what it actually means:

```
camera: "exposure" was refused because this camera needs its automatic mode
        turned off first -- set the matching auto_* option to false alongside it
```

Controls a camera doesn't have are reported once and skipped, not treated as a
failure. To see what yours offers: `v4l2-ctl -d /dev/video0 --list-ctrls`.

### Camera source

| `capture_source` | Behavior |
|---|---|
| `"auto"` (default) | Use a Pi camera module if one is found, else V4L2 |
| `"rpicam"` | Always the Pi camera module; fails at startup if absent |
| `"v4l2"` | Always a V4L2 device; skips the probe |

Pin it explicitly if the vehicle carries **both** a CSI camera and a USB webcam —
otherwise which one you get depends on probe order.

Pi camera modules don't go through V4L2: on Pi 5 the `rp1-cfe` driver exposes the
sensor as raw Bayer only, and converting that to an image is the ISP's job via
libcamera. So `eeye` spawns `rpicam-vid` and reads MJPEG from a pipe (needs
`rpicam-apps`, preinstalled on Raspberry Pi OS). Consequences: no resolution
negotiation — an unsupported geometry fails at the startup probe rather than being
substituted — and its MJPEG is 4:2:0, which `eeye` handles by line-doubling chroma
during decode.

### Recording

Two options, and for a whole dive you almost certainly want the second.

**Topside, encoded (`tools/eeye-record`)** — ~1.2 GB/hour, and costs the drone
nothing, since it re-uses frames already being sent for the preview:

```sh
tools/eeye-record                          # H.264 to dive-<timestamp>.mp4
tools/eeye-record --raw --split 10m        # raw camera, a new file every 10min
tools/eeye-record --quality high -o run.mp4
```

It records the **preview stream**, so what you get depends on two drone-side
settings — framerate is `30 ÷ stream_frame_interval`, quality is
`stream_quality`, and `stream_raw` decides whether effects are burned in. The
tool prints exactly what it is about to capture before it starts:

```
Source         : processed image (effects burned in), ~15 fps, JPEG quality 75
```

`--raw` / `--effects` flip `stream_raw` on the drone for you. It connects
through `web_ui.py` rather than to the drone directly, because the drone serves
one viewer at a time — connecting straight to it would kick the pilot off
their video.

**Onboard, uncompressed (`"record_path"`)** — writes the camera frame **before
the effect chain** as raw I422, no container. Empty means off. This is the
full-rate, full-quality, effect-free source, and it is enormous: ~53 MB/s,
~190 GB/hour at 1280×720. Use it for short clips where the compression or the
dropped frames of the topside path would matter.

`downscale` applies (it happens during capture decode, upstream of this tap),
so recordings shrink proportionally — ~48 GB/hour at `downscale: 2`. Play back
at `capture ÷ downscale`:

```sh
ffplay -f rawvideo -pix_fmt yuv422p -s 640x360 -r 30 -i FILE   # downscale 2
```

| | topside (`eeye-record`) | onboard (`record_path`) |
|---|---|---|
| size | ~1.2 GB/hour | ~190 GB/hour |
| framerate | `30 ÷ stream_frame_interval` | full capture rate |
| quality | JPEG then H.264 | untouched |
| effects | follows `stream_raw` | always excluded |
| costs the drone | nothing extra | disk write per frame |

### Live preview

`"stream_frame_interval"` — send every Nth frame to a connected viewer. `0`
disables the tap entirely and no JPEG work happens at all. `"stream_quality"` is
JPEG quality 1–100.

`"stream_raw"` (default `false`) sends the **untouched** camera frame to the
preview instead of the processed one, so you can tell a real object from a
filter artifact without dismantling your chain. Preview only — the virtual
camera and the recording tap are unaffected.

This is a lossy, throttled preview, independent of `record_path`'s full-quality
local recording. Nothing valuable rides on it, so push quality and rate down hard
over a constrained link.

## How it works

```
 camera                                                  v4l2loopback
    │                                                          ▲
    ▼                                                          │
┌─────────────┐   raw    ┌──────────────┐   work    ┌──────────────────┐
│ producer    │ ───────► │ effects      │ ────────► │ consumer         │
│ (v4l2_in /  │  frame   │ (effect_chain│  frame    │ (v4l2_out, plus  │
│  rpicam_in) │  pool    │  /conv/      │  pool     │  record + stream │
└─────────────┘          │  point_opps) │           │  taps)           │
                         └──────────────┘           └──────────────────┘
                                ▲                            │
                                │ snapshot, once per frame   │ JPEG, throttled
                         ┌──────────────┐      inotify       ▼
                         │ config watch │ ◄──── eeye_config.json   TCP :9000
                         └──────────────┘
```

Three threads pass frames through lock-free ring buffers. Each `VideoFrame` carries
`raw` (untouched camera frame), `work` (what the chain writes), and `spare` (so
chained neighborhood ops can ping-pong without a filter consuming its own output).
Consecutive point ops fuse into one lookup-table pass. A config-watch thread
publishes snapshots atomically; the effects loop reads one per frame, so a config
push can never split a frame.

Topside, `web_ui.py` proxies config reads/writes to `config_agent.py`
server-to-server (no CORS, and the drone's ports needn't be reachable from the
browser's network) and holds a single stream connection regardless of how many
browsers are watching.

**No authentication on any port.** Fine on a private point-to-point tether; not
fine on a shared network.

## Known limitations

- **Single-viewer stream.** `stream_server` accepts one connection at a time, last
  one wins. `web_ui.py` fans out to multiple browsers from that one connection.
- **Framerate and device paths are compile-time constants** in `src/eeye.c` and
  `src/video_threads.c`. Resolution is configurable.
- **Resolution is negotiated once, at startup.** A *different* camera plugged in
  later is held to the original size. Replugging the same one is fine.
- **The web UI shows the config's geometry, not the negotiated one.** Check
  `eeye`'s startup log for what is actually in use.
- **Onboard recording is uncompressed** (~190 GB/hour), so it is for short clips.
  `tools/eeye-record` encodes topside at ~1.2 GB/hour for anything longer — at
  the preview's framerate and quality, not the camera's.
- **Camera controls on the Pi camera backend need a restart to apply.**
  `rpicam-vid` takes them as command-line arguments, so changing one respawns
  the child — a ~1s gap in the video, announced in the log. The V4L2 backend
  applies them instantly. The Pi-side mapping is **unverified on real CSI
  hardware**; only its argument construction is tested.
- **Snapshots and topside recordings capture the preview**, so they inherit
  `stream_frame_interval`, `stream_quality`, and `stream_raw`.
- **Recording from the UI needs `ffmpeg` on the topside machine.** The Record
  button disables itself and says so when it is missing; everything else,
  snapshots included, works without it.
- **Verified on real Pi hardware with a USB camera.** The Pi camera module backend,
  the web UI over a real tether, and the stale-link/recording-failure indicators
  have not been.

## Development

```sh
meson setup builddir       # once, or after meson.build changes
meson compile -C builddir
```

`src/` is a flat set of translation units: capture (`v4l2_in`, `rpicam_in`, shared
MJPEG decode in `jpeg_decode`), output (`v4l2_out`), preview tap (`stream_server`),
module load/unload (`virtual_cam`), ring buffer, frame pool, effects (`conv.c` for
sobel/blur, `point_opps.c` for contrast), the chain runner (`effect_chain.c` —
where grayscale/invert/threshold/tint/light live, fused into LUTs), and config
hot-reload (`config.c`), wired together in `video_threads.c` / `eeye.c`.

`pi/`, `topside/`, and `tools/` are standalone Python — stdlib only, not part of
the meson build.
