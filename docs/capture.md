# Capture

Two backends, one shared decoder, and a portable camera-control layer.

## Choosing a backend

`capture_source` in config decides, and `main()` resolves it once at startup:

| Value | Behavior |
|---|---|
| `"auto"` (default) | Runs `rpicam-vid --list-cameras`; uses the Pi camera if one is found, else V4L2 |
| `"rpicam"` | Always the Pi camera module; fails at startup if absent |
| `"v4l2"` | Always a V4L2 device; skips the probe |

Resolution is deliberate: `producer_loop`'s reconnect loop runs repeatedly, and
re-probing on every retry would let the backend silently change mid-session (a USB
webcam unplugged, a CSI camera picked up on the next retry) with the frame pool already
sized for the first one.

`video_threads.c` unifies the two behind a tiny `CaptureHandle` — exactly one of its
two pointers is non-NULL — so `producer_loop` does not branch on backend anywhere
except when opening.

## V4L2 backend (`v4l2_in.c`)

USB/UVC webcams. Direct `ioctl` + MMAP streaming; **no subprocess, no pipe**.

Frames arrive via `VIDIOC_QBUF` / `VIDIOC_DQBUF` against buffers the driver DMAs into,
which `eeye` `mmap`s once at open. `poll()` with a 2s timeout gates each dequeue.

### Format selection

`capture_format` controls it:

| Value | Behavior |
|---|---|
| `"auto"` (default) | MJPEG if offered at exactly this size, else YUYV |
| `"mjpeg"` | MJPEG only; fails if unavailable |
| `"yuyv"` | YUYV only; fails if unavailable |

**Why `auto` prefers MJPEG:** at USB 2.0's ~24.6 MB/s isochronous ceiling, most webcams
only offer higher resolutions through MJPEG, reserving raw YUYV for lower modes. That
is the right default for the cameras most people have.

**Why you might not want it:** YUYV is packed 4:2:2 — the same layout `VideoFrame`
uses, just interleaved — so capture is a deinterleave into the planes with **no decode
at all**. On a camera bought for that, `auto` would silently take MJPEG whenever both
are offered and quietly discard the reason for buying it. Hence the strict explicit
settings: asking for a format and being handed the other one defeats the point of
asking.

The bandwidth constraint is the thing to plan around, since YUYV is uncompressed:

| Resolution @30fps | YUYV bandwidth | Fits USB 2.0? |
|---|---|---|
| 640×480 | 18.4 MB/s | yes |
| 1280×720 | 55.3 MB/s | no — needs USB 3.0 |
| 1920×1080 | 124 MB/s | no — needs USB 3.0 |

The negotiated format is logged at open, because whether a run pays for a decode is
otherwise invisible:

```
/dev/video0: capturing YUYV at 640x480 (no decode -- deinterleave only)
```

`try_set_format()` demands an **exact** dimension match. Unlike the output side, a
substituted size is fatal here: the frame pool is already allocated from `width`/
`height`, so any capture format that does not land on them exactly dooms every
subsequent frame.

### Resolution negotiation

`v4l2_in_negotiate_size()` runs once in `main()`, before the pool is allocated. It asks
the driver (`VIDIOC_ENUM_FRAMESIZES`) what it offers and rewrites the requested
geometry to the closest usable match.

Ranking is **aspect ratio first, pixel count second**. A 4:3 substitute for a 16:9
request re-frames every shot, which is a worse surprise than the same framing at fewer
pixels. An exact match wins outright and is chosen silently.

Two details that are easy to break:

- **Candidates are filtered by the downscale divisibility rule** (both dimensions a
  multiple of `2 * downscale`). The negotiated size is what the chroma math actually
  runs on, so a mode failing that rule would produce frames whose chroma planes do not
  line up with luma. If every mode fails, it says so rather than silently reverting
  `downscale`.
- **Only formats this run will actually open are enumerated.** Considering a format
  `v4l2_in_open` will then refuse could substitute a size only that format offers, and
  every open would fail on it. Demonstrated on the dev webcam, which does MJPEG at
  1280×720 but YUYV only to 640×480: pinning YUYV at 720p correctly steps down to YUYV
  640×360.

It is advisory and cannot fail the caller — the camera legitimately may not be plugged
in yet at startup, and `producer_loop`'s reconnect loop covers that.

It also must run in `main()`, not inside `v4l2_in_open()`: the pool is allocated once
and every frame must match the eventual camera size, and by the time `v4l2_in_open`
runs on the producer thread that size is already committed.

### Downscale

`downscale` (1, 2, 4, or 8) is applied **on the way out of the decode**, not as a
separate pass. Frames handed downstream are already `capture / downscale`, and nothing
downstream ever sees a full-resolution frame.

- **MJPEG path**: the divisor goes to libjpeg-turbo, which scales *during*
  decompression by discarding high-frequency DCT coefficients — so the decode itself
  gets cheaper, not just the work after it.
- **YUYV path**: `downscale_yuyv_frame()` box-averages each N×N source block while
  deinterleaving, so it is one pass rather than a deinterleave plus a resample.

**Why only 1/2/4/8:** libjpeg-turbo supports a fixed set of ratios and, asked for one
it does not support, *silently returns the largest supported size that fits* rather
than failing — which would leave a smaller image sitting in a larger frame with stale
bytes around it. The YUYV box average only divides evenly for integer N. Restricting to
powers of two makes both exact.

The `downscale == 1` route through `unpack_yuyv_frame` is kept byte-identical to what
it was before downscaling existed, so the qemu-verified NEON deinterleave did not need
re-verifying.

## Pi camera backend (`rpicam_in.c`)

CSI ribbon-cable modules. **Forks a `rpicam-vid` child** and reads MJPEG from a pipe.

### Why a subprocess at all

On Pi 5 the `rp1-cfe` driver exposes the CSI sensor as **raw Bayer only** — no MJPEG,
no YUYV — and expects the pipeline to be configured through the Media Controller API.
Turning Bayer into a viewable image means black level, demosaic, white balance, lens
shading and tone mapping, which on this platform is the ISP's job and is driven by
libcamera. Raspberry Pi's own guidance is not to drive Pi 5 cameras through V4L2
directly.

So there is no device node for `v4l2_in.c` to open. Delegating to `rpicam-vid` buys the
whole ISP chain and the per-sensor tuning files for free.

The spawned command is effectively:

```sh
rpicam-vid --codec mjpeg --width W --height H --framerate 30 --quality 93 \
           -t 0 -n --flush -o - [camera control flags]
```

`-t 0` runs until killed, `-n` suppresses the preview window (no display on a
submersible, and drawing one costs real CPU), `--flush` pushes each frame out
immediately rather than letting it sit in a stdio buffer.

There is a second, short-lived fork: `rpicam_in_available()` runs
`rpicam-vid --list-cameras` for the `auto` probe. It keys on the *absence* of "No
cameras available" rather than the presence of a header, because the positive wording
has changed across releases.

### Why MJPEG and not `--codec yuv420`

The raw YUV output is **stride-padded** — Y rows to a multiple of 64 bytes, U/V to 32 —
with no framing and no geometry in the stream, so a consumer must know the padding rule
to unpack it. That is the cause of a long tail of "corrupt raw output" reports
upstream. MJPEG is self-delimiting and carries its own dimensions, so a frame either
decodes correctly or fails loudly.

The cost is real and worth knowing: `rpicam-vid` **software-encodes** the JPEG (its
`mjpeg_encoder.cpp` uses libjpeg), and `eeye` immediately decodes it. That is two JPEG
operations per frame on the Pi's CPU, where a USB webcam's own chip does the encode for
free. Removing it is a live optimization option — see
[extending.md](extending.md#remove-the-jpeg-round-trip-pi-camera).

`rpicam_jpeg_quality` is a compile-time constant (93) in `rpicam_in.c`. It is the
*capture* quality feeding the whole pipeline — distinct from `stream_quality`, which
only affects the preview.

### Frame framing

`find_jpeg_end()` finds where one JPEG ends in the accumulated buffer. It walks marker
segments to reach SOS and only *then* scans entropy-coded data for EOI.

A naive scan for the bytes `FF D9` would be wrong: before SOS, an APP segment carrying
a thumbnail or vendor blob can contain those two bytes as ordinary payload, and the
frame would be truncated there. Inside entropy-coded data a literal `FF` is always
stuffed (`FF 00`) or a restart marker (`FF D0`–`D7`), so `FF D9` there genuinely is the
end.

Return values are three-way: a length, `0` for "need more bytes", or `SIZE_MAX` for
"this is not a JPEG stream" — which the caller treats as a stream-level failure rather
than waiting forever for a frame that will never come.

Whatever follows a decoded frame stays in the buffer for the next call; a single
`read()` routinely straddles a frame boundary.

## Shared decode (`jpeg_decode.c`)

Both backends face the same MJPEG→I422 problem from different sources, so it lives in
one place.

Decodes straight into `VideoFrame`'s `raw_planes` via `tjDecompressToYUVPlanes` — no
intermediate RGB, no extra copy.

**Two subsamplings are supported:**

- **4:2:2** — libjpeg-turbo's plane layout matches I422 exactly, so it decodes straight
  in with nothing to convert.
- **4:2:0** — what `rpicam-vid` produces (it calls `jpeg_set_defaults()` without
  overriding sampling factors). Chroma is half-height, so it decodes into scratch planes
  and is **line-doubled** into the full-height I422 planes. Luma still decodes directly,
  since it is full-resolution in both layouts.

Line doubling is nearest-neighbour, not interpolated: the source genuinely has no
information between those rows, and interpolating would blur chroma edges to buy
smoothness the 4:2:0 encoder already discarded.

This is why **output height must be even** — a half-height chroma plane only fills a
full-height one exactly when the output height is even. Combined with the width rule,
that is the `2 * downscale` divisibility requirement in config validation.

**DHT injection**: some UVC cameras omit the Huffman table from their MJPEG stream and
rely on the decoder already knowing the standard tables. Each frame is scanned for an
existing DHT (by walking marker segments, not by byte search) and the standard tables
are spliced in after SOI only when genuinely missing. A camera that emits its own DHT
pays only the scan.

Result codes distinguish **transient** (a corrupt payload — worth a retry, since
isolated bad USB transfers are real) from **format mismatch** (dimensions or
subsampling disagree with what `open()` validated — retrying cannot fix it).

## Camera controls (`camera_ctrl.c`)

Exposure, gain, white balance and the usual image adjustments, expressed once and
applied by whichever backend is running. Hot-reloadable — light changes as the drone
descends, and surfacing to adjust exposure is not an option.

### Three things this had to get right

**1. Ordering, and why it looks like a permissions bug.** A camera rejects a manual
value while its auto mode is on, and reports **EPERM** — the same errno a genuine
permissions problem gives. Verified on the dev webcam:

```
$ v4l2-ctl --set-ctrl exposure_time_absolute=200
Error setting controls: Permission denied      # auto_exposure still on

$ v4l2-ctl --set-ctrl auto_exposure=1          # manual first
$ v4l2-ctl --set-ctrl exposure_time_absolute=200
exposure_time_absolute: 200
```

So auto toggles are written **first, in the same pass**, and an EPERM on a manual
control is reported as an ordering problem rather than sending the operator to check
their `video` group membership.

**2. Units differ between backends.** V4L2 counts exposure in 100µs units; `rpicam-vid
--shutter` in 1µs. The config uses **microseconds** (the finer of the two) and converts
— rounded, not truncated, so a value near a boundary does not land a step low.

**3. Ranges are per-camera.** The dev webcam runs brightness −64..64, contrast 0..100,
gain 0..128; another will differ. So the config uses **percent ranges with documented
neutrals** and scales onto whatever `VIDIOC_QUERYCTRL` reports, clamping rather than
failing when a value does not fit. Verified: brightness 100 → 64, contrast 150 → 75,
saturation 50 → 25.

### The unset sentinel

Every field defaults to `camera_ctrl_unset` (`INT32_MIN`), meaning "leave this control
alone". This cannot be a plain `0` — omitting `brightness` would drive it to mid-range
and `contrast` to its minimum on every config that did not mention them.

The sentinel is set in `config_defaults` itself rather than patched in at each use, so
all three copies of that struct are correct.

### Backend differences

- **V4L2**: ioctls on the live device, so changes apply instantly. `producer_loop`
  compares against the last-applied set and only re-issues on change — a
  `VIDIOC_S_CTRL` per frame would be pointless syscall traffic.
- **rpicam**: controls are **spawn-time CLI arguments**, so a change respawns the
  child. That is a visible ~1s gap in the video, so it is announced in the log rather
  than done silently, and it skips the reconnect backoff because it is deliberate
  rather than a failure.

Controls are re-applied after **every** successful open, not just the first — a camera
unplugged mid-dive otherwise comes back at factory defaults with nobody noticing until
the footage is useless.

Some UVC cameras accept a write and then ignore it, so each control is read back after
writing and a mismatch is logged.

**Unverified**: the rpicam control mapping has never run against real CSI hardware.
Only its argument construction is tested.
