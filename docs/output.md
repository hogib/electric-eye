# Output

Four things happen to a finished frame, all in `consumer_loop`: it goes to the virtual
camera, and optionally to the recording tap, the preview stream, and the health
snapshot.

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

**A failed write is fatal to the run.** Unlike a missing camera, a loopback device that
went away or rejected a frame is not something retrying fixes, so `consumer_loop` stops
the whole pipeline rather than spinning on a dead device. Recovery is the systemd
unit's `Restart=`.

### Loading the module

`virtual_cam_ensure_loaded()` runs `modprobe` (via fork/exec, not `system()`, so a
caller-controlled `card_label` is never shell-interpreted).

Two non-obvious behaviors:

- **`modprobe` succeeding does not mean `/dev/videoN` exists yet.** udev creates the
  node asynchronously and can lag module insertion by tens of milliseconds. The code
  polls for the path (up to 2s, 50ms steps) rather than letting that race surface as a
  confusing `ENOENT` deep inside `v4l2_out_open()`.
- **Loading a module needs `CAP_SYS_MODULE`**, which a plain user — even one in the
  `video` group — does not have. `eeye.service` grants exactly that one capability via
  `AmbientCapabilities` rather than running the whole daemon as root.

If the module was already loaded before this process started, `eeye` leaves its
parameters alone and will not unload it on exit. It only unloads a module it loaded
itself, so it cannot pull the device out from under another consumer.

### The Secure Boot diagnostic

A failed `modprobe` is commonly misread as a permissions problem. On Secure Boot
systems the far more likely cause is that `v4l2loopback-dkms` built and signed the
module fine, but the signing key was never enrolled — the kernel then refuses to load
any unenrolled out-of-tree module with **the exact same EPERM** a missing
`CAP_SYS_MODULE` produces.

Distinguishing them would ideally mean reading dmesg for the lockdown message, but
`kernel.dmesg_restrict` (the Debian/Ubuntu default) blocks that for anything short of
root or `CAP_SYSLOG` — exactly the case `eeye.service` runs under. So the code shells
out to `mokutil`, which reads EFI variables and needs no special capability, and prints
MOK enrollment guidance instead of "run as root".

## Preview stream (`stream_server.c`)

A deliberately dumb live-preview tap: JPEG-encodes frames and writes each one,
length-prefixed, to whichever single client is connected over plain TCP.

**Wire format**, repeated per frame: a 4-byte **big-endian** length, then that many
bytes of one complete JPEG (SOI…EOI). No HTTP, no framing beyond that header — serving
it to a browser is the other end's problem.

### Things that will catch you

**Single viewer, last connection wins.** A new connection *replaces* the existing
client. If two things connect (say, the web UI and a diagnostic script), they will kick
each other off in a reconnect loop that looks exactly like a flaky link. This is a real
trap — it caused an hour of misdiagnosis once.

**It runs on its own thread.** `consumer_loop`, which feeds the virtual camera, only
calls `stream_server_offer_frame()`: that copies the frame into a one-slot mailbox and
returns. The stream thread encodes and sends. If it is still busy with the previous
frame, the newer one replaces whatever is waiting, since a live preview wants the
latest frame, not every frame. So a viewer on a bad tether can only make the preview
stale; it cannot stall the pilot's local feed. The encode moved off the consumer thread
with it.

**It costs nothing when nobody is watching.** `stream_server_offer_frame()` checks an
atomic "viewer connected" flag before copying anything, so an unwatched stream is one
idle listening socket and a sleeping thread. That is why `stream_frame_interval: 0` is
a safe default.

**A slow viewer gets dropped when one frame takes more than 200ms** (`send_timeout_ms`)
to send. The deadline covers the whole frame: the client socket is non-blocking and
every `sendmsg()` waits only for whatever is left of it. The earlier `SO_SNDTIMEO`
bounded each `send()` call instead, and restarted on every partial send, so a viewer
draining slowly could hold the consumer thread far past 200ms. In the test suite a
viewer that never reads is dropped after ~270ms, while each offer returns in under a
millisecond.

That deadline interacts with frame size in a way worth doing the arithmetic on. At
100 Mbit/s:

| `stream_quality` | Frame size | Push time | Margin vs 200ms |
|---|---|---|---|
| 100 | 391 KB | 32 ms | 6× |
| 60 | 38 KB | 3.1 ms | 64× |

Quality 100 is the pathological end of JPEG — size explodes for no visible gain over
~85. Measured on real hardware: dropping 100 → 60 cut bandwidth 10× and took delivered
rate from unmeasurable to exactly 10.0 fps.

**Dual-stack.** The listening socket is `AF_INET6` with `IPV6_V6ONLY` off, so one fd
serves both families. This is not cosmetic: on a direct tether the only zero-config
addressing is IPv6 link-local, and an IPv4-only listener is simply unreachable that
way. `SO_REUSEADDR` is set so a restart does not fail to rebind during `TIME_WAIT`.

Each frame goes out as one `sendmsg()` of the length prefix and the JPEG together.
Nagle is disabled as well, so the tail of a frame is not held back waiting for an ACK.

### The raw bypass

`stream_raw: true` sends the **untouched** camera frame to the preview instead of the
processed one, leaving the virtual camera and recording tap on the full chain.

It answers a question you cannot otherwise answer from topside — "is that thing in the
water real, or is it my filter?" — without dismantling the chain and putting it back.

Implemented by pointer: `raw_data` shares the frame's layout exactly, so a shallow
`VideoFrame` view with the raw planes is what gets offered, and the mailbox copies from
those planes instead of the processed ones.

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

For anything longer than a short clip, `tools/eeye-record` topside writes H.264 at
~1.2 GB/hour instead — see [topside.md](topside.md).

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
  "geometry": {"width": 1280, "height": 720}
}
```

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

**`config_agent.py` attaches the file's age** when relaying it, because a health file
that stopped being updated means `eeye` is wedged or gone — which looks perfectly
healthy if you only read the contents. Confirmed in testing: after killing `eeye` the
relayed snapshot still said camera connected, frames frozen.
