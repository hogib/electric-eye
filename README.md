# Electric Eye

Realtime camera effects for a submersible drone (ROV), built for a BlueROV and any
GNU/Linux machine. Two programs:

- **`eeye`** — the vision layer. Takes frames from a camera (or from the tether),
  runs them through a chain of effects, and publishes the pilot's picture and the
  untouched camera frame on the **frame bus** for anything else on the machine —
  `hellion`, autonomy code, OBS through a v4l2loopback webcam. No networking.
- **`hellion`** — everything that touches the tether. Sends the video up from the ROV
  as H.264 over RTP, receives it topside, feeds QGroundControl, records MP4, serves the
  browser control page, and carries `eeye`'s config across. `hellion top` is a live
  dashboard for an SSH session; `hellion check` is a pre-dive check.

Both are C99, built by one Makefile, and need nothing but `libturbojpeg` and (for
`hellion`'s video) `ffmpeg`.

- [Installing](#installing) · [Using it](#using-it) ·
  [Where eeye runs](#where-eeye-runs) · [In the field](#in-the-field) ·
  [Troubleshooting](#troubleshooting) · [Config reference](#config-reference) ·
  [Building by hand](#building-by-hand)

## Installing

Two machines, each installed once: the **ROV** (the vehicle's Pi, or whatever has the
camera) and **topside** (the laptop the pilot uses). Both are installed with the same
**placement** — `b` runs the effects on the ROV and is the one to use; `a` runs them
topside (see [Where eeye runs](#where-eeye-runs)).

`install.sh` installs the packages (`ffmpeg` among them), builds, puts `eeye` and
`hellion` in `/usr/local/bin`, writes their configs, and starts them as **systemd
services** that come back on every boot. It needs Debian, Ubuntu or Raspberry Pi OS
(`apt-get`); elsewhere install the packages it lists and add `--no-packages`.

### On a BlueROV

The tether uses the BlueROV's fixed addresses — topside 192.168.2.1, ROV 192.168.2.2 —
which are the defaults, so there is nothing to type but the role. On the ROV:

```sh
git clone https://github.com/hogib/electric-eye && cd electric-eye
sudo ./install.sh --role rov --placement b
```

Topside, `--tether` names the network port the tether is plugged into and gives it the
topside address (find the name with `ip link`; it is rarely `eth0` nowadays):

```sh
git clone https://github.com/hogib/electric-eye && cd electric-eye
sudo ./install.sh --role topside --placement b --tether enp5s0
```

Leave `--tether` out to set the address yourself. **Never point it at the port your
LAN or internet comes in on**: that port becomes the tether from its next reconnect.
The ROV's network is never touched — on a BlueROV it belongs to BlueOS.

### Two machines on a LAN (testing on the bench)

Any two Linux machines on the same network work — a laptop with a webcam stands in for
the ROV. Give both machines' addresses, the **same command on both**:

```sh
# on the "ROV" (the one with the camera), here 192.168.1.105:
sudo ./install.sh --role rov --placement b --topside-ip 192.168.1.117 --rov-ip 192.168.1.105

# topside, here 192.168.1.117:
sudo ./install.sh --role topside --placement b --topside-ip 192.168.1.117 --rov-ip 192.168.1.105
```

No `--tether` here: the LAN already connects them. Use IP addresses, not hostnames.

### Updating, changing, removing

- **Update:** `git pull && sudo ./install.sh`. It remembers the role, placement and
  addresses, keeps your configs, and restarts the services on the new build.
- **New addresses:** re-run with `--topside-ip`/`--rov-ip`; the config is rewritten and
  the old one kept as `/etc/hellion/hellion.json.bak`.
- **Start over:** `sudo ./install.sh --reset-config` puts back fresh configs (old ones
  kept as `.bak`). `sudo ./install.sh --help` lists everything.
- **Remove:** copy any recordings out of `/var/lib/hellion/recordings` first, then:
  ```sh
  sudo systemctl disable --now hellion eeye
  sudo rm -rf /etc/systemd/system/{hellion,eeye}.service /usr/local/bin/{eeye,hellion} \
      /etc/hellion /var/lib/{eeye,hellion} /etc/modules-load.d/v4l2loopback.conf \
      /etc/modprobe.d/v4l2loopback.conf
  sudo userdel eeye
  sudo nmcli connection delete hellion-tether   # only if you used --tether
  ```

### From the release binaries

The [releases](https://github.com/hogib/electric-eye/releases) have static `eeye` and
`hellion` builds for arm64, armv7 and amd64 that need no libraries. `install.sh` is
still the easy way in: it builds from source, but the result is the same program. To use
the binaries instead, copy them to `/usr/local/bin` as `eeye` and `hellion`, and follow
the comments at the top of `eeye.service` and `hellion/hellion.service`.

## Using it

**Everything is already running** once `install.sh` finishes. Do not start `eeye` or
`hellion` yourself — a second copy finds the ports taken (`8080 already in use`) and
fights the first for the camera and the tether. What you run by hand is only:

```sh
hellion top                       # live dashboard, either machine (q quits); fine over SSH
hellion check                     # pre-dive check: exit 0 healthy, 1 degraded, 2 down
journalctl -u hellion -u eeye -f  # the logs
```

Topside, open **`http://localhost:8080/`** (or `http://<topside>:8080/` from another
device) for the live picture, the **AUTO VISIBILITY** button, effects, camera controls,
recording and snapshots. Point QGroundControl at **UDP 5600** as usual. `/dev/video10`
is the pilot's picture as a webcam, for OBS or a browser (`ffplay /dev/video10`).

If `hellion top` says permission denied, you were not in the `video` group before the
install: log out and back in.

**The services**, on either machine:

```sh
systemctl status hellion eeye          # running? (eeye only exists where it runs)
sudo systemctl restart hellion         # after editing /etc/hellion/hellion.json
sudo systemctl stop hellion eeye       # stop until the next boot or start
```

| What | Where |
|---|---|
| `hellion`'s config (pipelines, addresses, ports) | `/etc/hellion/hellion.json` — restart `hellion` after editing |
| The effects and camera settings | `/var/lib/eeye/effects.json` on the machine running `eeye` — edited live by the page; no restart |
| Recordings, snapshots | `/var/lib/hellion/recordings`, `/var/lib/hellion/snapshots` (topside) |
| Ports | 8080 the page (topside), 8091 the ROV's control port, UDP 5610 the tether video, UDP 5600 QGroundControl |

## Building by hand

```sh
make                              # builddir/eeye and builddir/hellion
make -j check                     # the test suites
```

Needs GNU make, a C99 compiler (GCC 4.7+ or clang), and libturbojpeg 1.4+
(`libturbojpeg0-dev` on Debian and Raspberry Pi OS). OpenMP is used when the compiler
has it. A distro too old to package libturbojpeg 1.4 can point the build at one built
from source; see the top of the `Makefile`.

On 32-bit ARM (32-bit Raspberry Pi OS, Debian armhf) the build turns NEON on itself, so
the effects use the Pi's SIMD unit; the result needs a Pi 2 v1.1 or newer. `make NEON=0`
builds for older boards (every effect then runs several times slower).

Neither program needs root. Both need the `video` group (cameras, v4l2loopback, and the
frame bus's shared memory); the services run as a system user, `eeye`, in that group.
Nothing loads v4l2loopback on demand: `install.sh` sets it to load at boot topside.
Elsewhere:

```sh
sudo modprobe v4l2loopback video_nr=10 card_label=hellion exclusive_caps=1
```

The v4l2loopback webcam is optional: if it is missing, `eeye` says why and carries on.

## Where eeye runs

`eeye` can run on either side of the tether; `hellion` runs on both. Pick one —
**B is the one to use** unless the ROV's Pi cannot afford the effects.

**B — `eeye` on the ROV** (`--placement b`; configs from `hellion/examples/b-rov.json`, `b-topside.json`)

```
ROV:      camera → eeye → frame bus "pilot" → hellion → H.264/RTP ─┐
topside:  ┌──────────────────────────────────── tether UDP 5610 ◄──┘
          └→ hellion → relayed unchanged → QGroundControl (UDP 5600)
                     → decoded → frame bus "pilot" → page preview, /dev/video10
                     → recorded as MP4
```

One encode, on the ROV; QGroundControl gets the ROV's packets unchanged, so the pilot
pays no extra latency. Autonomy code running on the ROV reads the camera from the frame
bus next to `eeye`.

**A — `eeye` topside** (`--placement a`; `a-rov.json`, `a-topside.json`)

```
ROV:      camera → hellion → H.264/RTP → tether UDP 5610
topside:  hellion → decoded → frame bus "camera" → eeye → frame bus "pilot"
          → hellion → H.264/RTP → QGroundControl (UDP 5600), /dev/video10
```

The ROV only encodes its camera; the effects run on the topside machine, which then
encodes again for QGroundControl.

The examples use the BlueROV's fixed addresses (topside 192.168.2.1, ROV 192.168.2.2).
Keep the tether port away from 5600: many viewers also take port+1 for RTCP.
**Turn off BlueOS's own video stream** for the camera `eeye` or `hellion` uses — only
one program can hold a camera.

Cockpit cannot show the processed picture: it gets video from BlueOS over WebRTC.
Use QGroundControl, the page, or the webcam device.

## In the field

**Before the dive**, on the topside machine:

```sh
hellion check
```

```
[ OK ] hellion "topside" (topside) responding
[ OK ] peer: 192.168.2.2:8091, last contact 0.3s ago
[ OK ] pipeline pilot-in: source rtp:5610 (1280x720, 36 frames in 1.2s)
[ OK ] pipeline pilot-in: sink bus:pilot (36 frames)
[ OK ] bus stream pilot: 1280x720, last frame 0.02s ago
Healthy.
```

Exit `0` healthy, `1` degraded (a sink waiting, packet loss of 1% or more, the ROV not
answering), `2` down (no video). Every line names what is wrong.

**During the dive**, the page at `http://<topside>:8080/`:

- **AUTO VISIBILITY** — the big button at the top (or the `A` key). On, `eeye`
  continuously corrects the pilot's picture for what water does to it: the blue-green
  colour cast, a squashed brightness range, and haze. It follows the light as you
  descend, smoothly, with nothing to adjust. **Off by default**; while it is on the
  picture carries an **AUTO VISIBILITY** badge, so nobody mistakes the processed
  picture for the camera's. Under the button, a histogram of the camera's own picture
  and what is wrong with it in words ("strong blue-green cast", "12% blown out —
  too much exposure or light"), which also says when the fix is the camera's exposure
  or the lights rather than anything on screen. See
  [Auto visibility](#auto-visibility).
- **The picture** — any frame bus stream (`pilot`, or `camera`/`raw` to see past the
  effects), at ~15fps. When frames stop it greys out with a banner saying how stale it
  is: a frozen picture never looks live.
- **Effects and camera controls** — applied within ~200ms. The page reports whether
  `eeye` **accepted** the change; if it rejected it, the previous config stays in force
  and on disk.
- **Record** — the H.264 stream as it arrives over the tether, copied into an MP4 with no
  re-encode (~1.8 GB/hour at 4 Mbit/s). Fragmented MP4, so a recording cut off by a
  crash or power loss still plays up to its last second. Files go to
  `/var/lib/hellion/recordings`.
- **Snapshot** — a JPEG of the preview, to `/var/lib/hellion/snapshots`.

**Over SSH**, on either machine:

```sh
hellion top
```

Every pipeline with its fps, drops, and — for the tether — bitrate and **packet loss**;
every frame bus stream; `eeye`'s health; the last log lines; and the **other end of the
tether** as it describes itself, so on the ROV you can see what the topside actually
receives.

## Troubleshooting

Start with `hellion check` and `hellion top`; then the logs:
`journalctl -u hellion -u eeye -f` (the last lines of `hellion`'s are in `top` too).

### Installing and starting

| What you see | Cause | Fix |
|---|---|---|
| `8080 already in use` / `address already in use` when running `hellion` | It is already running, as a service. | Nothing to start: open the page, or run `hellion top`. Stop the copy you started (Ctrl-C) |
| `systemctl status hellion` says `failed`, `start-limit-hit` | It exits at start, usually a config error; systemd gives up after 5 tries. | `journalctl -u hellion -n 20` names the bad line. Fix it (or re-run `install.sh` with `--topside-ip`/`--rov-ip`), then `sudo systemctl restart hellion` |
| `Install failed at line N` | One step failed; the message above it says which. | Fix that and run the same command again — every step is safe to repeat |
| `--tether eth0: no such network interface` | The port has another name. | `ip link` lists them; usually `enp…` or `eno…` |
| Topside log: `lost rtp:5610; reconnecting` every 10s | Nothing is arriving from the ROV: normal while the ROV is off. | Check the ROV with `hellion top` there; its `rtp:` sink must name the topside's address |

### No video anywhere

| What you see | Cause | Fix |
|---|---|---|
| `Camera unavailable (attempt N)` in `eeye`'s log | Camera not detected. `eeye` retries forever; it does **not** need a restart. | Check the cable. `v4l2-ctl --list-devices`; set `capture_device` |
| `Permission denied opening /dev/video0` | The user running it is not in the `video` group (the installed services are). | `sudo usermod -aG video $USER`, then log out and back in |
| `top`: topside `link in` says `no packets` | Nothing arriving on the tether port. | Is `hellion` running on the ROV (`top` there)? Does its `rtp:` sink point at the topside's address and port? |
| `top`: ROV source `waiting … no such stream "pilot"` (placement B) | `eeye` is not running on the ROV, or publishes under another name. | `systemctl status eeye`; `bus_pilot` in its config |
| `Virtual camera: /dev/video10 does not exist because v4l2loopback is not loaded` | The module isn't loaded. Only the webcam device is missing; everything else runs. | `sudo modprobe v4l2loopback video_nr=10 card_label=hellion exclusive_caps=1`, or re-run `install.sh` |
| `modprobe: ... Operation not permitted` **and** Secure Boot is on | The module's signing key was never enrolled — the kernel says "not permitted" either way. | Reboot and enroll the key at the blue MOK screen; `mokutil --list-new` shows it pending |
| `neither MJPEG nor YUYV is available at exactly WxH` | Camera doesn't offer that mode. | `v4l2-ctl --list-formats-ext -d /dev/video0`, then set `capture_width`/`capture_height` to a listed mode |

### QGroundControl shows nothing

QGC listens on UDP 5600. In placement B the topside `hellion` relays to
`127.0.0.1:5600` (`relay` in its config); in placement A its `pilot-out` pipeline sends
there. Make sure **BlueOS is not also sending** to 5600, and nothing else on the
topside machine has the port.

### The picture is frozen but looks live

When frames stop arriving the page desaturates the picture and shows a pulsing banner
naming how stale it is (`LINK LOST — showing a frozen frame from 4.0s ago`). That
includes the page's own connection: if its preview stops while the link is fine (the
topside `hellion` restarted, say), it shows `PICTURE NOT UPDATING — reconnecting the
preview` and reconnects by itself, within a second of the stream being back.
`hellion top` shows which hop stopped.

### Blocky or smeared picture

Packet loss on the tether. `top` shows it on the `link in` line, `check` warns from 1%,
and the decoder's damage count appears next to it. Lower `bitrate_kbps` on the ROV's
`rtp:` sink, or check the tether.

### Recording stopped without telling me

The page shows the recording's elapsed time and size while it runs and turns red with the
reason if it stops (the disk is nearly full, ffmpeg failed). A recording will not
start with under 500 MB free.

If the **video stops** (tether pulled, ROV restarting), the recording is not lost: it
shows *waiting for video*, and when packets return it carries on in a new file next to
the first, `dive-…-part2.mp4`, `-part3.mp4` and so on. Every part plays on its own.

### I change settings and nothing happens

Geometry, devices and stream names are **startup-only** (see
[Config reference](#config-reference)): changing them is logged and ignored until
`eeye` restarts. If the page says `eeye` never answered, `eeye` is not running, or is
watching a different file than `eeye_config` in `hellion`'s config.

### Config rejected

`eeye` keeps running on the last valid config and logs why — an unknown key is a hard
error, so a typo is loud:

```
Config: unknown key "bogus_key"
```

From the page, `hellion` also puts the previous file back, so `eeye` will still start
next time. Strict JSON only: no comments, no trailing commas.

### Pipeline can't keep up

Set `"downscale"` to `2`. It is by far the biggest lever — see the table in
[Resolution](#resolution). Then reduce `blur_strength` (cost is linear in it, but
visible blur grows much more slowly).

`journalctl -u eeye | grep full_stalls` — nonzero means a stage is falling behind. On
the ROV, a Pi 4 can encode in hardware: `"encoder": "h264_v4l2m2m"` on the `rtp:` sink's
pipeline.

### eeye won't restart cleanly

```sh
systemctl status eeye
```

The unit restarts on failure but gives up after 5 restarts in 60s, so a persistent fault
surfaces as a failed service instead of looping silently. A missing camera does not
trigger this — that is retried internally.

## Config reference

`eeye`'s config: `/var/lib/eeye/effects.json` when installed (`eeye_config.json` in the
checkout is a starting point for running it by hand). `hellion`'s own config is in [docs/hellion.md](docs/hellion.md#config).

Every key is optional. Defaults shown:

```json
{
  "chain": [{"effect": "sobel"}],
  "auto_visibility": false,
  "record_path": "",
  "record_format": "raw",
  "capture_width": 1280,
  "capture_height": 720,
  "downscale": 1,
  "capture_source": "auto",
  "capture_format": "auto",
  "capture_device": "/dev/video0",
  "output_device": "/dev/video10",
  "capture_stream": "camera",
  "bus_pilot": "pilot",
  "bus_raw": "raw",
  "camera": {}
}
```

> **Startup-only:** `capture_width`, `capture_height`, `downscale`,
> `capture_source`, `capture_format`, `capture_device`, `output_device`,
> `capture_stream`, `bus_pilot`, `bus_raw`. Everything else is hot-reloadable. Changing these in a running
> config is logged and ignored — applying them live would mean reallocating the
> frame pool while three threads hold frames from it.

**Writer contract:** write a temp file in the same directory, then `rename()` onto
the target. In-place overwrite also works, but rename guarantees a reader never
sees a half-written file.

### Auto visibility

`"auto_visibility": true` (the page's big button) runs a continuous correction ahead of
the chain, on the pilot's picture only — the `raw` stream, recordings `eeye` makes, and
anything autonomy reads stay the camera's own frame. Every frame it measures the
camera's picture and corrects three things, each smoothed over ~0.4s so the picture
neither flickers nor pumps, but follows a descent:

- **Colour cast** — the average colour of well-exposed pixels is moved back to near
  neutral (gray world), scaled with brightness, then saturation is lifted a little.
  A scene's own colours average out to a small cast that is left alone, so a red
  object does not turn the rest of the picture blue.
- **Brightness range** — the 0.5th to 99.5th percentiles are stretched to full range
  (a few specks of backscatter cannot defeat it, unlike `contrast`), with a gamma that
  puts the median at mid-grey. The stretch is capped (~2.7×) so dark open water does
  not turn its sensor noise into texture.
- **Haze** — contrast-limited adaptive histogram equalization (CLAHE) on brightness,
  tile by tile, blended with the global correction.

On synthetic underwater frames it takes the cast from 39 to 9, the brightness range
from 129 to 249 levels, and more than doubles edge contrast; on a still scene the
picture is still. It adds ~1.2 ms per 720p frame on an x86 desktop (all cores); the Pi
is not measured yet. Switching on takes effect on the next frame, with no fade-in.
The effect chain runs on its output, so `auto_visibility` and, say, `sobel` combine.

It works on the picture, not the camera: it cannot recover highlights the sensor
clipped or detail lost in black. The page's notes say when that is the problem.

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

| `downscale` | pipeline | CPU | JPEG frame at q60 |
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
`"camera"` block with one key changes exactly one control. The one exception is
`keep_framerate`, which is on unless you turn it off: many UVC webcams ship with
auto exposure allowed to lengthen the shutter past the frame time in dim light (the
V4L2 control is `exposure_dynamic_framerate`), and the test webcam dropped from 30 to
17 fps in a dim room. Every frame downstream — the effects, the tether, QGC, the
recording — inherits that. Set it `false` if you would rather have a brighter picture
at a lower rate. Cameras without the control ignore it silently.

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
| `keep_framerate` | bool | Default `true`: auto exposure may not lower the frame rate (see below) |

These are **hot-reloadable** — adjust them from the page's Camera section and the
picture changes immediately, no restart. In placement A, where the camera is on the
ROV under `hellion` and `eeye` is topside, the topside `hellion` carries them to the
ROV (`"camera_controls": "peer"`, already in `a-topside.json`); the page says whether
the ROV's camera took them, and the ROV keeps them for its next restart. Measured over
a LAN: ~100–160 ms from the page's request to the camera's registers. They are also re-applied after a camera
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

`"capture_format"` picks the pixel format:

| value | behavior |
|---|---|
| `"auto"` (default) | MJPEG if offered at this size, else YUYV (Pi camera: MJPEG) |
| `"mjpeg"` | MJPEG only; fails at startup if unavailable |
| `"yuyv"` | YUYV only; fails at startup if unavailable (USB cameras) |
| `"yuv420"` | Pi camera only: raw ISP output, no JPEG encode or decode. **Opt-in and not yet verified on a real camera** — see [docs/capture.md](docs/capture.md) |

**Pin `"yuyv"` on a camera you chose for it.** YUYV is packed 4:2:2 — the same
layout the pipeline uses internally — so capture is a deinterleave with **no
decode at all**, where MJPEG costs a decode every frame. But `"auto"` prefers
MJPEG, so a YUYV-capable camera would silently give you the decode anyway.

The catch is bandwidth: YUYV is uncompressed, so 1280×720 at 30fps needs
~55 MB/s and will not fit USB 2.0. Plan on USB 3.0 for YUYV above 480p, and
check what yours actually offers:

```sh
v4l2-ctl --list-formats-ext -d /dev/video0
```

`eeye` logs which format it settled on at startup:

```
/dev/video0: capturing YUYV at 640x480 (no decode -- deinterleave only)
```

Resolution negotiation respects this — pinning `"yuyv"` only considers modes
that format actually offers, so it will step down to a smaller YUYV size rather
than pick an MJPEG-only resolution it then cannot open.

Pi camera modules don't go through V4L2: on Pi 5 the `rp1-cfe` driver exposes the
sensor as raw Bayer only, and converting that to an image is the ISP's job via
libcamera. So `eeye` spawns `rpicam-vid` and reads MJPEG from a pipe (needs
`rpicam-apps`, preinstalled on Raspberry Pi OS). Consequences: no resolution
negotiation — an unsupported geometry fails at the startup probe rather than being
substituted — and its MJPEG is 4:2:0, which `eeye` handles by line-doubling chroma
during decode.

### Recording

Two kinds, for different jobs.

**Topside, `hellion`** — the page's **Record** button. The H.264 stream exactly as it
crosses the tether, copied into a fragmented MP4: ~1.8 GB/hour at the default
4 Mbit/s, no re-encode, and a file cut off by a crash still plays. In placement B that
is the pilot's picture; in placement A it is the camera before effects. Details in
[docs/hellion.md](docs/hellion.md#recording).

**Onboard, `eeye`'s `"record_path"`** — writes the camera frame **before the effect
chain**, on `eeye`'s machine. Empty means off. By default it is raw I422, no container:
full rate, full quality, and enormous — ~53 MB/s, ~190 GB/hour at 1280×720. Use it for
short clips where compression would matter.

`"record_format": "mjpeg"` writes the JPEG frames the camera itself sent instead: no
re-encode, roughly an order of magnitude smaller, playable with
`ffplay -f mjpeg -framerate 30 -i FILE`. It needs an MJPEG camera; with a YUYV camera it
falls back to raw and says so in the log.

Recording runs on its own thread, so a disk that can't keep up drops recorded frames
(counted as `dropped_frames` in the health file) rather than freezing the live feed.
`downscale` applies, so recordings shrink with it. Play raw back at
`capture ÷ downscale`:

```sh
ffplay -f rawvideo -pix_fmt yuv422p -s 640x360 -r 30 -i FILE   # downscale 2
```

### Frame bus and devices

| Key | Default | |
|---|---|---|
| `bus_pilot` | `"pilot"` | Frame bus stream for the processed picture (`""` for none) |
| `bus_raw` | `"raw"` | Frame bus stream for the untouched camera frame (`""` for none) |
| `capture_source: "bus"` | — | Read frames from a frame bus stream instead of a camera (placement A, topside) |
| `capture_stream` | `"camera"` | Which stream, with `capture_source: "bus"`; its size sets the geometry |
| `capture_device` | `"/dev/video0"` | The V4L2 camera |
| `output_device` | `"/dev/video10"` | The v4l2loopback webcam (`""` for none) |

All startup-only. The frame bus is how other programs on the machine get `eeye`'s
video — autonomy code included — with capture timestamps on every frame; its layout and
how to read it from C or Python are in [docs/bus.md](docs/bus.md).

`stream_frame_interval`, `stream_quality` and `stream_raw` fed `eeye`'s old preview
server, which `hellion` replaced. A config that still has them loads; they are ignored,
with one line in the log.

## How it works

```
 camera ──► eeye: capture ─► effects ─► publish ──► frame bus: "pilot", "raw"
                                      └──────────► v4l2loopback webcam (optional)

 frame bus ──► hellion pipelines ──► H.264/RTP over the tether ──► hellion ──►
              (one source, many       (ffmpeg encodes;             relay to QGC,
               sinks, each on its      hellion moves every          decode to the bus,
               own thread)             packet and counts loss)      record, page
```

Inside `eeye`, three threads pass frames through lock-free ring buffers; consecutive
point ops fuse into one lookup-table pass; a config-watch thread hot-reloads the JSON
file, and the effects loop reads one snapshot per frame, so a config push never splits a
frame. Nothing slow runs on the thread that feeds the pilot's picture.

The **frame bus** is shared memory: one file per stream in `/dev/shm`, a ring of frames
under a seqlock. The writer never waits for a reader, so a slow reader — or a crashed
one — can never stall the pilot's picture; readers sleep on a futex until a frame lands.

`hellion` runs **pipelines**: one source (a camera, a frame bus stream, or RTP from the
tether) feeding up to eight sinks (the frame bus, a webcam device, RTP), each sink on its
own thread behind a one-slot mailbox, so a slow sink drops its own frames and holds up
nothing else. ffmpeg does H.264 and RTP in child processes; `hellion` carries every
packet through its own relay, which is what lets it report each link's bitrate and loss
and keep QGC fed while a decoder restarts.

More in [docs/](docs/README.md): [architecture](docs/architecture.md),
[the frame bus](docs/bus.md), [hellion](docs/hellion.md).

**No authentication on any port.** Fine on a private point-to-point tether; not fine on
a shared network.

## Known limitations

- **Tether latency is measured on x86 only** — frame bus to frame bus through both
  `hellion`s, 3.8 ms at 640×480 and 8.3 ms at 720p (down from 70–75 ms in v0.2.0;
  see [docs/hellion.md](docs/hellion.md#latency)). Camera exposure, the tether and the
  Pi's slower encode add to that on the vehicle, and are not measured yet.
- **Recordings' timing assumes `framerate`** (default 30): frames are stamped at the
  nominal rate. `hellion` warns when a source runs more than 10% off it.
- **Cockpit cannot show the processed picture** (it takes video from BlueOS over WebRTC).
- **Resolution is negotiated once, at startup.** A *different* camera plugged in later
  is held to the original size. Replugging the same one is fine.
- **Camera controls on the Pi camera backend need a restart to apply** — a ~1s gap,
  announced in the log. The V4L2 backend applies them instantly. The Pi-side mapping is
  unverified on real CSI hardware.
- **Not yet run on the vehicle.** Everything above is tested on x86 machines: test
  patterns, loopback devices and a simulated lossy link on one, and placement B end to end
  with a USB webcam across a LAN (cable pulls included). The Pi, its camera and the
  Fathom-X tether are next.

## Development

```sh
make                       # builddir/eeye, builddir/hellion
make -j check              # the test suites
make BUILD=debug           # unoptimized, eeye-d and hellion-d
make fuzz-http             # fuzz hellion's HTTP parsers (clang)
```

**[`docs/`](docs/) has the internal documentation** — architecture, both capture
backends, the effect chain, the config system, the frame bus, `hellion`, testing, and
recipes for adding an operator or a config key. Start at
[docs/README.md](docs/README.md), or go straight to [docs/extending.md](docs/extending.md)
if you are here to change one thing.

`src/` is `eeye` and the modules both programs share: capture (`v4l2_in`, `rpicam_in`,
shared MJPEG decode in `jpeg_decode`, `bus_in`), output (`v4l2_out`, the optional webcam
`virtual_cam`), the frame bus (`fbus`), child processes (`child_proc`), effects
(`conv.c`, `point_opps.c`, the chain runner `effect_chain.c`), config hot-reload
(`config.c`), wired together in `video_threads.c` / `eeye.c`. `hellion/` is the second
binary; [docs/hellion.md](docs/hellion.md) maps it.
