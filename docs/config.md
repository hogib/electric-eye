# Configuration

One JSON file, hand-written or written over the network, hot-reloaded via inotify.

## The two classes of key

This is the distinction to internalize before changing anything:

**Hot-reloadable** — picked up within ~200ms, no restart. `chain`, `record_path`,
`record_format` (read when a recording starts), `stream_frame_interval`, `stream_quality`, `stream_raw`, and everything under `camera`.

**Startup-only** — read once by `config_load_once()` before any thread exists.
`capture_width`, `capture_height`, `downscale`, `capture_source`, `capture_format`.

Changing a startup-only key in a running config is **detected, logged, and ignored**:

```
Config: geometry changed (1280x720/downscale 1 -> 640x480/downscale 2) but it
only takes effect at startup -- still running at the original size. Restart
eeye to apply it.
```

Warning rather than silence, because a config field that appears to change but does
nothing is exactly the kind of thing that costs an hour to notice. `capture_source` and
`capture_format` get their own separate check so the message names the field that
actually changed.

**Why geometry cannot be hot-reloaded:** it would mean draining and reallocating the
frame pool while three threads hold frames from it, plus renegotiating the format on
both V4L2 devices — and v4l2loopback cannot change format at all while a consumer has
the device open. Restart is the honest answer.

Startup-only keys still live in `Config` (rather than a separate struct) for one
concrete reason: `parse_config` treats an unknown key as a hard error, so the watcher
has to understand them anyway or every reload of a geometry-carrying file would fail.

## Full key reference

```json
{
  "chain": [{"effect": "sobel"}],
  "record_path": "",
  "record_format": "raw",
  "stream_frame_interval": 0,
  "stream_quality": 60,
  "stream_raw": false,
  "capture_width": 1280,
  "capture_height": 720,
  "downscale": 1,
  "capture_source": "auto",
  "capture_format": "auto",
  "camera": {}
}
```

Every key is optional; the values above are the defaults.

### `chain`

An ordered list of stages. **Order is semantic** — blur-then-sobel differs from
sobel-then-blur. An empty chain (`[]`) is a valid pass-through. Capped at
`max_chain_stages` (8); exceeding it is a hard error, not silent truncation.

Effects: `none`, `grayscale`, `invert`, `threshold`, `tint`, `sobel`, `blur`,
`contrast`, `light`, `log`, `canny`.

| Parameter | Effect | Range | Default |
|---|---|---|---|
| `threshold_value` | `threshold` | 0–255 | 0 |
| `tint_u`, `tint_v` | `tint` | 0–255 | 0 |
| `tint_strength` | `tint` | 0–255 | 0 (no-op) |
| `sobel_threshold` | `sobel` | 0–255 | 0 |
| `blur_strength` | `blur` | 0–255 | 0 (= 1 pass) |
| `light_level` | `light` | 0–255 | **128** (neutral) |
| `log_strength` | `log` | 0–255 | 0 (= 1 pass) |
| `log_threshold` | `log` | 0–255 | 0 |
| `canny_strength` | `canny` | 0–255 | 0 (= 1 pass) |
| `canny_low` | `canny` | 0–255 | **40** |
| `canny_high` | `canny` | 0–255 | **90** |

Three defaults are deliberately not zero, because zero would be actively wrong:
`light_level` (0 means fully dark and desaturated), and `canny_low`/`canny_high` (0/0
produces an empty edge map). All three are seeded in `parse_effect_stage`.

**A parameter on the wrong effect is a hard error.** `{"effect":"blur","tint_u":90}` is
rejected. A correctly-spelled key attached to the wrong stage is exactly as much of a
typo as a misspelled one, and just as silent otherwise. `params_for_effect()` holds the
table; `topside/web_ui.py`'s `EFFECT_FIELDS` must stay in sync with it.

### Geometry

`capture_width` / `capture_height` are what `eeye` asks the camera for — a demand, not a
hint (see [capture.md](capture.md) on negotiation).

`downscale` must be **1, 2, 4, or 8**. Both dimensions must be a multiple of
`2 * downscale`:

```
Config: 1280x724 doesn't divide evenly by "downscale": 4 -- both width and
height must be a multiple of 8
```

Width, so the I422 chroma planes stay exactly half-width. Height, because a 4:2:0
source has half-height chroma that gets line-doubled into full-height planes, which
only lands exactly on an even output height. One rule covers both layouts rather than
making the valid geometries depend on which camera is attached.

### Capture source and format

`capture_source` is `"auto"`, `"v4l2"` or `"rpicam"`. `capture_format` is `"auto"`,
`"mjpeg"` or `"yuyv"` for a V4L2 camera (see [capture.md](capture.md#format-selection)),
or `"yuv420"` for the Pi camera only: raw ISP output with no JPEG encode or decode,
opt-in until verified on hardware (see [capture.md](capture.md)). `"yuv420"` with
`"capture_source": "v4l2"` is refused at parse time; with `"auto"` it is refused at
startup if the probe picks V4L2.

### Streaming and recording

`stream_frame_interval` — send every Nth post-effects frame. **0 disables the tap
entirely** and no JPEG work happens at all. `stream_quality` is JPEG quality 1–100
(values above 100 are clamped by turbojpeg itself, so this field does not separately
range-check).

`stream_raw` sends the untouched camera frame to the preview only.

`record_path` — empty means off. See [output.md](output.md) for the size warning and
the `~` trap.

`record_format` — `"raw"` (default: decoded I422, ~55MB/s at 720p) or `"mjpeg"` (the
camera's own JPEG bytes, roughly an order of magnitude smaller). `"mjpeg"` needs an
MJPEG source; with YUYV or rpicam `yuv420` capture the recording falls back to raw and
logs it. Changing it while recording restarts the recording. See
[output.md](output.md#record_format).

### `camera`

A nested object; every field optional, and **absent means "leave that control alone"**.

| Key | Range | Notes |
|---|---|---|
| `auto_exposure` | bool | Must be `false` before `exposure` takes effect |
| `exposure` | µs | 1–10,000,000 |
| `gain` | 0–100 | Percent of the sensor's own range |
| `auto_white_balance` | bool | Must be `false` before `white_balance` |
| `white_balance` | Kelvin | 1000–15000 |
| `brightness` | −100–100 | 0 neutral |
| `contrast` | 0–200 | 100 neutral |
| `saturation` | 0–200 | 100 neutral |
| `sharpness` | 0–200 | 100 neutral |

Range checks here are deliberately loose — they reject values that could only be a
mistake (a Kelvin temperature of 9, a percentage of 500). The real per-device clamping
happens in `camera_ctrl_apply`, the only place that knows what a given camera accepts.

## The parser

Hand-written recursive descent in `config.c`. No dependency, ~300 lines.

**Strict JSON only.** No comments, no trailing commas. Validate with
`python3 -m json.tool eeye_config.json`.

Parser helpers, by value type: `parse_string`, `parse_u8`, `parse_u16` (capture
dimensions, the only values that do not fit a byte), `parse_i32` (signed and wider —
brightness is negative, exposure exceeds 65535), `parse_bool` (accepts only the two
JSON literals; `0`/`1` are rejected, since silently accepting a number where a bool
belongs hides a typo).

**Unknown keys are hard errors at every level** — top-level, inside a chain stage, and
inside `camera`.

The whole-file read has a 4096-byte limit. If the read fills the buffer exactly, one
more `fgetc` decides whether the file was truncated or simply that size — otherwise a
config exactly at the limit would be rejected.

## Hot reload

A background thread watches the file with inotify.

**It watches the containing directory, not the file.** A watch on the file's inode goes
stale the moment `rename()` replaces it, which is precisely how writers are supposed to
update it.

### Writer contract

**Write to a temp file in the same directory, then `rename()` onto the target.** A
plain in-place overwrite can be observed mid-write and read as a truncated, invalid
file; rename is atomic from a reader's point of view. `eeye` watches for both, but only
rename is safe.

`pi/config_agent.py` implements this in `write_config_atomic()`.

### Reload feedback

After **every** load attempt, `eeye` writes `<config_path>.status` — same atomic-rename
contract — containing one line:

```json
{"ok":true}
```

`true` means that attempt is what is now running; `false` means it was rejected and the
previous config is still active. This exists for a remote writer that has no other way
to know whether what it just wrote took effect.

The detailed reason for a rejection is only ever on `eeye`'s own stdout, deliberately —
so there is not a second place that must agree with `parse_config` about how to phrase
every possible error.

**A rejected reload keeps the previous config running.** Only a failure at *startup* is
fatal, because then there is no previous good state.

### Degradation

If the watch cannot be established (the directory does not exist, `inotify_init1`
fails), the process keeps running with whatever loaded at startup — just without live
reload — and says so. `inotify_fd == -1` is the "static mode" marker.

## Publishing snapshots

`config_publish()` writes into a slot of an 8-entry ring inside `ConfigWatcher`, then
does a release store on `current`. Readers do an acquire load, giving them an immutable
snapshot.

The ring exists so a reader holding an older pointer stays valid; it would take 8
reloads to wrap, far longer than a frame. `Config` is a plain pointer-free POD copied
by value into that ring, which is what makes it safe with no per-slot allocation — and
why `record_path` is a fixed `char[256]` rather than a pointer, and why the chain is a
fixed array.
