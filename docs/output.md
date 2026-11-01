# Output

Four things happen to a finished frame, all in `consumer_loop`: it is published on the
frame bus (`pilot`, and the untouched frame as `raw`; see [bus.md](bus.md)), and
optionally goes to the virtual camera and the recording tap, and is counted in the
health snapshot. Everything that leaves the machine — the preview, H.264 to the other
end of the tether, the MP4 recording — is `hellion`'s, reading the bus
([hellion.md](hellion.md)).

## Virtual camera (`v4l2_out.c`, `virtual_cam.c`)

The primary output. Frames are written to a v4l2loopback device (`/dev/video10` by
default) that any application can open like a normal webcam.

### Why `write()` and not MMAP

The capture side uses MMAP streaming; the output side deliberately does not. For a
device that is simply pushing already-formatted frames, `write()` costs one syscall per
frame and needs no buffer negotiation. On the capture side MMAP genuinely matters,
because it lets the driver DMA directly into memory we already own.

This was investigated and explicitly rejected as an optimization: v4l2loopback shares
one buffer between `write()` and its MMAP output path, so MMAP would not have saved a
copy. Checked against the driver source before writing any code.

### Optional, and retried

`output_device` sets the device (`""` turns it off). It is **optional**: a device that
is missing at startup, or a write that fails mid-run, is logged and retried, and the
rest of the pipeline carries on. `virtual_cam.c` owns the retry:

| Situation | Retry | Logged |
|---|---|---|
| Device node missing | stat every 2s | once, with why it might be missing |
| Node exists, open fails | backs off 2s → 4s → … → 30s | each attempt, with the reason |
| Node opens at a different frame size | same back-off | each attempt |
| A write fails | closed at once, reopened 2s later | once |

It used to be fatal, with systemd's `Restart=` as the recovery. That coupled restarts:
under `exclusive_caps=1`, a reader holding the device can stop a restarted `eeye` from
reopening it for a moment, and those failures counted against the unit's start limit.
The webcam device is a convenience for programs that only know webcams; it must not be
able to take the pipeline down.

### The module is loaded at boot, not by `eeye`

`install.sh` puts v4l2loopback in `/etc/modules-load.d` with its options in
`/etc/modprobe.d`. `eeye` never runs `modprobe`, so it needs no `CAP_SYS_MODULE` (the
unit runs with no capabilities at all), and it can never unload the module out from
under another program.

### The Secure Boot diagnostic

When the device is missing because the module isn't loaded, `eeye` says how to load it,
and checks for the case that is commonly misread as a permissions problem. On Secure
Boot systems `v4l2loopback-dkms` may have built and signed the module fine, but the
signing key was never enrolled — the kernel then refuses to load any unenrolled
out-of-tree module with **the same EPERM** a permissions problem produces.

Distinguishing them would ideally mean reading dmesg for the lockdown message, but
`kernel.dmesg_restrict` (the Debian/Ubuntu default) blocks that for an unprivileged
process. So `eeye` runs `mokutil`, which reads EFI variables and needs no special
capability, and mentions MOK enrollment when one is pending.

## The preview, and seeing the raw frame

`eeye` used to serve its own JPEG preview over TCP; `hellion` serves the page's preview
now, from any frame bus stream on the topside machine. Choosing the frame before
effects — `camera` in placement A — answers the question nothing else can from topside:
"is that thing in the water real, or is it my filter?", without dismantling the chain.
In placement B only `pilot` crosses the tether, so that view is not available topside;
`raw` is published on the ROV (one copy per frame of planes `eeye` already has) for
anything running there, autonomy included.

## Recording tap

`record_path` writes the **untouched** camera frame, independent of the effect chain:
by default `raw_data` as raw I422 with no container, frames back to back.

### It runs on its own thread

The recorder (`recorder.c`) owns the file. `consumer_loop` writes the virtual camera
first, then offers the frame with `recorder_submit()`. If the writer thread takes it,
the frame goes back to the pool once it is written. `fopen()`, `fwrite()` and
`fclose()` (which flushes, and is often the slow one) all happen on the writer thread.

The writer holds at most `recorder_max_queued` (3) frames. When the disk falls behind
and it is full, `recorder_submit()` refuses, the consumer returns the frame to the pool
itself, and the recording counts a **dropped frame**. That count is in the health file
as `dropped_frames`. A slow disk costs footage you can see is missing, instead of
freezing the live view. Before this, raw recording at 55MB/s to an SD card that could
not keep up blocked `fwrite()`, backed up the out ring, and the pilot's picture fell up
to a second behind.

### `record_format`

`"raw"` (the default) is described above. `"mjpeg"` writes the JPEG bytes the camera
sent instead: roughly an order of magnitude smaller, no encode cost, and one decode
closer to the sensor than the raw file. The bytes are kept per frame by
`jpeg_decode_frame()` (after the DHT patch, so a DHT-less camera still records a
playable stream). Play it back with:

```sh
ffplay -f mjpeg -framerate 30 -i FILE
```

Only an MJPEG source has those bytes. With YUYV or rpicam `yuv420` capture, the first
frame decides: the recording falls back to raw for the whole file and logs that it did.
The format is read when a recording starts; changing it mid-recording restarts the
recording.

### Size, and why this matters more than it looks

Raw is ~53 MB/s, ~190 GB/hour at 1280×720. On a real dive this filled a Pi's disk in
about eight minutes and took the whole machine down. `"record_format": "mjpeg"` is the
way out of that when the camera sends MJPEG. `downscale` applies (it happens during
capture decode, upstream of this tap), so `downscale: 2` quarters that.

`eeye` reports free space and estimated recording time when a recording starts:

```
Recording: 41000 MB free at /data/dive.raw (~780 seconds of raw video at this geometry)
```

Play back with the *output* dimensions, i.e. `capture / downscale`:

```sh
ffplay -f rawvideo -pix_fmt yuv422p -s 640x360 -r 30 -i FILE
```

**`~` is not expanded.** The path goes to `fopen()` with no shell involved, so
`~/out.raw` tries to create a directory literally named `~`. Use absolute paths.

For anything longer than a short clip, `hellion` records topside: the H.264 stream as
it crosses the tether, ~1.8 GB/hour at 4 Mbit/s — see
[hellion.md](hellion.md#recording).

### Failure handling

A write failure (almost always a full disk) stops the recording, which is not retried
until `record_path` changes, and — importantly — sets the health state to
`RECORDING_FAILED` with the reason. Without that last part the operator has no way to
know: the UI still shows the path they typed.

## Health reporting (`health.c`)

`eeye` republishes `<config_path>.health` every second, carrying what only the drone
knows.

```json
{
  "recording": {"state": "active", "path": "/data/dive.raw", "error": "",
                "bytes": 24166195200, "dropped_frames": 0,
                "disk_free_bytes": 27641176064},
  "camera": {"connected": true, "frames": 13110},
  "geometry": {"width": 1280, "height": 720},
  "auto_visibility": false,
  "image": {"p1": 3, "p50": 77, "p99": 240, "dark_pm": 12, "clip_pm": 4,
            "cast_u": 9, "cast_v": -31, "hist": [500, 0, "...", 7]}
}
```

`image` describes the **camera's** picture (the raw frame, before auto visibility and
the chain), measured every frame by `img_stats.c`: luma percentiles, the permille of
samples crushed below 16 or clipped at 250 and above, the mean chroma offset of
well-exposed pixels (the colour cast; water is positive U, negative V), and a 32-bin
luma histogram in permille. `null` until the first frame. `auto_visibility` is whether
it actually processed the latest frame. The page draws the histogram and turns these
into words; `hellion top` prints them.

`state` is one of `off`, `active`, `failed`.

`dropped_frames` counts frames the recorder skipped because the disk could not keep up
(see [Recording tap](#recording-tap)).

**Why this exists:** when a disk fills mid-dive, the recorder stops recording — but the operator's UI still shows the path they typed, so they carry
on believing they are recording and only discover the gap after surfacing.

Same writer contract as the config status file: **write a temp file in the same
directory, then `rename()`**, so a reader can never observe it half-written.

Paths are JSON-escaped — a path may legally contain a quote or a backslash, and one
unescaped byte would make the whole file unparseable to the reader that needs it most.

Best-effort throughout: a failure to write is silent and never fatal. Health reporting
must not be able to take down the pipeline it reports on.

**`hellion` attaches the file's age** when serving it (`/api/drone`, `/health`), because a health file
that stopped being updated means `eeye` is wedged or gone — which looks perfectly
healthy if you only read the contents. Confirmed in testing: after killing `eeye` the
relayed snapshot still said camera connected, frames frozen.
