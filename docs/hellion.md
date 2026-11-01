# hellion

The tether relay: everything that moves video between machines, and the tools for
seeing what it is doing over SSH. `eeye` does the vision; `hellion` does the moving.
Source: `hellion/`, built by the same Makefile as `builddir/hellion`.

```sh
hellion [-c /etc/hellion/hellion.json]   # run
hellion top [-n name] [--once]           # live dashboard
hellion check [-n name]                  # health check: exit 0 ok, 1 degraded, 2 down
```

**Status:** pipelines with camera, frame bus and RTP sources; frame bus, v4l2loopback
and RTP sinks (H.264 to the other end of the tether, or to QGroundControl); the browser
page with preview, snapshots and `eeye`'s config (here or across the tether); `top`
and `check`. Recording and the ROV↔topside status exchange come next.

## Config

`/etc/hellion/hellion.json` (or `-c`). Read once at startup; strict like `eeye`'s — an
unknown key or bad value stops startup with the reason.

```json
{
  "name": "rov",
  "role": "rov",
  "eeye_config": "/var/lib/eeye/effects.json",
  "pipelines": [
    {"name": "uplink", "source": "v4l2:/dev/video0",
     "width": 1280, "height": 720, "format": "auto",
     "sinks": ["bus:camera", "v4l2:/dev/video11"]}
  ]
}
```

| Key | Default | |
|---|---|---|
| `name` | `"hellion"` | Instance name. Status lives in `<bus dir>/hellion.<name>`; `top -n` picks it |
| `role` | `"rov"` | `"rov"` or `"topside"`. Shown by `top` |
| `eeye_config` | `""` | `eeye`'s config on this machine, if it runs here: the page edits it, and `top` and `check` read its health beside it |
| `control` | `""` | Where to serve the page and API: a port (`"8080"`, every interface) or `"addr:port"`. `""` serves nothing |
| `peer` | `""` | The other `hellion`'s `control`, as `"host:port"`. Where `eeye`'s config and health come from when `eeye` is not on this machine |
| `preview` | `"pilot"` | The frame bus stream the page previews by default |
| `snapshot_dir` | `/var/lib/hellion/snapshots` | Where the page's snapshots are saved |
| `pipelines` | — | 1 to 8; see below |

A **pipeline** moves frames from one source to up to 8 sinks:

| Source | |
|---|---|
| `v4l2:<device>` | A V4L2 camera, MJPEG or YUYV, through `src/v4l2_in.c` |
| `rpicam` | A Pi camera module, through `rpicam-vid` (`src/rpicam_in.c`) |
| `bus:<stream>` | A frame bus stream; the size comes from the stream |
| `rtp:<port>` | H.264 over RTP arriving on UDP `<port>`, decoded by ffmpeg |

| Sink | |
|---|---|
| `bus:<stream>` | Publish to the frame bus ([bus.md](bus.md)) |
| `v4l2:<device>` | A v4l2loopback device. Optional and retried, like `eeye`'s ([output.md](output.md#optional-and-retried)) |
| `rtp:<host>:<port>` | Encode to H.264 with ffmpeg and send over RTP |

| Pipeline key | Default | For |
|---|---|---|
| `width`, `height` | 1280×720 | Camera sources (asked of the camera) and RTP sources (what the decoder scales to). Even numbers |
| `format` | `auto` | Camera sources: `auto`, `mjpeg`, `yuyv`; `yuv420` for `rpicam` |
| `framerate` | 30 | Asked of cameras, and what RTP sinks stamp frames at (see [Timestamps](#timestamps)) |
| `relay` | `[]` | RTP sources: `["host:port", ...]` to forward the packets to unchanged, e.g. QGC |
| `encoder` | `libx264` | RTP sinks: any ffmpeg H.264 encoder, e.g. `h264_v4l2m2m` (Pi 4's hardware encoder) |
| `bitrate_kbps` | 4000 | RTP sinks |
| `gop` | 30 | RTP sinks: frames between keyframes — how soon a viewer that joins late gets a picture |

A pipeline cannot read the bus stream it publishes.

## The two placements

`eeye` runs on one side of the tether or the other; `hellion` runs on both, and the
same four example configs in `hellion/examples/` cover it (BlueROV addresses: topside
192.168.2.1, ROV 192.168.2.2, tether port 5610).

**A — `eeye` topside** (`a-rov.json`, `a-topside.json`): the ROV sends its camera up;
topside decodes it to `bus:camera`, `eeye` reads that and publishes `pilot`, and a
second topside pipeline encodes `pilot` for QGC on 5600 and writes `/dev/video10`.
Two encodes, one on each side.

**B — `eeye` on the ROV** (`b-rov.json`, `b-topside.json`): `eeye` runs next to the
camera and the ROV sends `pilot` up. Topside relays the packets unchanged to QGC on
5600 — no second encode, no extra latency for the pilot — and also decodes them to
`bus:pilot` and `/dev/video10`. This is where autonomy will want `eeye` to be.

**Ports:** keep the tether port away from 5600. Most RTP receivers also bind port+1
for RTCP (QGC does not, but ffplay and VLC do), so a tether on 5601 collides with a
viewer on 5600 on the same machine. The examples use 5610.

## RTP: how the video crosses the tether

ffmpeg does the H.264 and the RTP framing in a child process (`child_proc.c`); `hellion`
feeds it raw frames and **moves every packet itself**, through `udp_relay.c`:

- **Sending**, ffmpeg encodes to a local port; the relay forwards each packet to the
  destination.
- **Receiving**, the relay owns the tether port and forwards each packet, unchanged,
  to the local decoder and to every `relay` target.

That costs a copy per packet and buys two things. `hellion` sees every RTP header, so
`top` shows each link's **bitrate, packets and loss** (from RTP sequence numbers;
late packets are not counted as lost, and a restarted sender's new SSRC is not a gap).
And relay targets such as QGC keep getting packets while the decoder restarts.

**Restarts.** An encoder that exits, or stops taking frames for 2s, is restarted with
back-off (1s doubling to 30s; back to 1s after a 30s run). A decoder that exits is
restarted at once, and one that produces nothing for 5s while packets keep arriving is
presumed wedged and restarted.

**Colour range.** The frame bus is full range (0–255, what the effects assume); H.264
for QGC and every player is limited (16–235). The encoder converts full→limited and
the decoder limited→full, explicitly. The round-trip test checks it: uniform Y=40 and
Y=200 frames come back at 40.0 and 200.0 (without the conversion, 40 comes back 28).

**Late joiners.** SPS/PPS go out before every keyframe (`dump_extra`), whatever the
encoder, so QGC opened mid-dive gets a picture within one `gop`.

**Decoder damage.** With packet loss the H.264 decoder complains about every damaged
macroblock — about ten lines a second at 2% loss. `hellion` counts those instead of
logging them, shows the count next to the link's loss in `top`, and logs one summary
line every 10s. Anything else ffmpeg says is logged as is.

### Timestamps

Frames go to the encoder stamped at the pipeline's nominal `framerate`, not at their
arrival times. Arrival times were tried and measured (ffmpeg 9, libx264, 640×480):

| Input timestamps | Result |
|---|---|
| wall clock, default 1/25s time base | arrivals collide on the same tick; **one frame in six silently dropped** (link capped at 25fps) |
| wall clock, 1ms time base | no drops, but x264 budgets for 1000fps: **161 kbit/s of a 4000 kbit/s target** |
| wall clock + `-r 30` | refused by ffmpeg alongside `-fps_mode passthrough` |
| nominal `-framerate 30` | **4015 kbit/s, every frame delivered** |

The cost: a source running at another rate makes the stream's clock run fast or slow.
Live viewers do not care; a recording would play at the wrong speed. The RTP sink
measures the real rate and, if it is more than 10% off for 10s, logs what to set
`framerate` to.

### Latency

Measured on one x86 machine over loopback, frame bus to frame bus through both
`hellion`s (encode, RTP, relay, decode), with textured 640×480 frames at 4 Mbit/s:
**median 70ms at 30fps, 37ms at 60fps**, spread under 1ms. A bus-to-bus pipeline with
no encoding measures 0.7ms, so the time is in ffmpeg — and since it halves at double
the frame rate, it is about two frame periods of holding, not processing time. Which
two stages hold a frame is not yet established (a likely candidate is the H.264 parser
on the decode side waiting for the next frame to know the current one ended). Camera,
tether and the Pi's slower encode add to this on the real vehicle.

## How a pipeline runs

One **source thread** owns the source. When the source goes away — a camera unplugged,
a bus stream's writer stopped — it reconnects on its own: a missing device is checked
every 2s without logging each try, a bus stream every 500ms.

Each **sink runs on its own thread** behind a one-slot mailbox. Handing a frame over is
one copy; if the sink has not taken the previous frame yet, the newer one replaces it
and the sink's `dropped` count goes up. So a slow sink costs that sink frames, and never
holds up the source or the other sinks.

If the source reopens at a **different size**, the sinks are restarted at the new size
(a bus sink's readers see their stream replaced and reattach).

Frames from a bus source keep the publisher's `frame_no` and capture time; a camera
source numbers its own and uses the driver's timestamp.

## The page and its API

Open `http://<topside>:8080/` (the topside's `control`). One page: the live preview,
the effect chain and camera controls for `eeye`, snapshot and record buttons, and a
banner that turns the picture grey and says so when the video stalls or the link drops.
The page is compiled into the binary (`hellion/web/index.html`, embedded at build time
by `hellion/embed.c`), so there is nothing to install beside it.

| Request | |
|---|---|
| `GET /` | The page |
| `GET /stream?src=S` | MJPEG of frame bus stream `S` (default `preview`), at most ~15fps, quality 75; at most 4 viewers |
| `POST /snapshot?src=S` | Saves a JPEG (quality 92) of `S`'s next frame in `snapshot_dir` |
| `GET /config`, `POST /config` | `eeye`'s config — see below |
| `GET /health?src=S` | `link` (is `S` alive, how old its newest frame), `drone` (`eeye`'s health file, plus its age), `recorder` |
| `GET /api/streams` | The frame bus streams on this machine |
| `GET /api/drone` | `eeye`'s health file here, with `age_s` |
| `GET /api/status` | This `hellion`'s pipelines as JSON, for scripts |

The preview is **any frame bus stream** on the topside machine, chosen in the page:
`pilot` (what the pilot sees), `camera`/`raw` (before effects) where they exist. Each
viewer reads the bus directly, so viewers cost the pipeline nothing.

**`eeye`'s config.** Where `eeye` runs on this machine (`eeye_config` set), a POST
writes the file the way `eeye`'s watcher expects (temp file + rename) and waits up to
3s for `eeye`'s verdict in `<config>.status`, comparing its mtime to the nanosecond.
The answer is `{"written":true,"eeye_accepted":true|false|null}` (`null`: `eeye` never
answered — not running, or watching another path). **If `eeye` rejects it, `hellion`
puts the previous file back** (`"restored":true`): `eeye` keeps running the old config
either way, but a rejected file left on disk would stop `eeye` starting next time.
Where `eeye` is not on this machine, `/config` and the `drone` part of `/health` are
proxied to `peer`, which serves the same paths.

**The HTTP layer** (`hellion/http.c`) is deliberately strict, because it is the one
part of `hellion` that parses bytes from the network: request head at most 8KB, body
at most 64KB, Content-Length bodies only (Transfer-Encoding is refused, not
half-supported), one request per connection, CRLF line endings only, no header
folding, a deadline on every read and write, at most 32 connections. The parsers are
pure functions, unit-tested case by case (`tests/test_http.c`) and fuzzed with
libFuzzer under ASan and UBSan (`make fuzz-http`; 5M inputs in two minutes, clean).

## `hellion top`

The dashboard for an SSH session: plain ANSI, no curses, `q` quits. Without `-n` it shows
every instance running on the machine.

```
hellion top  2026-11-01 18:22:07  (q quits)

HELLION "rov"  role rov  pid 3975380  up 6s  responding
 pipeline cam  640x480
  source v4l2:/dev/video20          running      30.0 fps       158 frames
  sink   bus:camera                 publishing   30.0 fps       158 frames      0 dropped
  sink   v4l2:/dev/video10          writing      30.0 fps       158 frames      0 dropped

FRAME BUS  /dev/shm
  camera         640x480   alive    30.0 fps  frame 165       last 0.03s ago
  pilot          640x480   alive    30.0 fps  frame 159       last 0.03s ago

EEYE  /var/lib/eeye/effects.json
  health 1s old  camera connected  640x480  150 frames  recording off
  last config load accepted  6s ago

LOG  "rov", last 5 of 5 lines
  18:22:01 hellion: pipeline "cam": v4l2:/dev/video20 connected at 640x480
```

- **Instances**: whether each `hellion` is responding (its heartbeat is under 2s old).
  A wedged one still shows its last state, marked NOT RESPONDING.
- **Pipelines**: each source and sink with its state, fps, frame count, drops, and the
  latest thing it had to say. A source that is open but silent shows as `idle — no
  frames for Ns` before the camera code gives up on it (~10s). RTP endpoints get a
  `link in`/`link out` line: bitrate, loss over the last second (amber above 0, red
  from 1%), and totals — or `no packets for Ns`.
- **Frame bus**: every stream in the directory, read straight from the bus, so it works
  even when no `hellion` runs — it is also the quickest check that `eeye` is publishing.
- **eeye**: from the `.health` and `.status` files beside `eeye_config`.
- **Log**: the last lines each instance printed, shared out among the instances.

`--once` prints a single snapshot without the interactive screen (and is what runs when
stdout is not a terminal).

## `hellion check`

One pass over the same information, as a verdict. It reads twice, 1.2s apart, so "frames
are flowing" means counters that actually moved.

```
[ OK ] hellion "rov" (rov) responding
[ OK ] pipeline cam: source v4l2:/dev/video20 (640x480, 30 frames in 1.2s)
[WARN] pipeline pilot-out: sink v4l2:/dev/video99 is waiting: device missing or busy; retrying (see log)
[ OK ] bus stream camera: 640x480, last frame 0.03s ago
Degraded: see the WARN lines.
```

Exit **0** healthy, **1** degraded (a sink waiting or failing, a stream without recent
frames, a receiving link losing 1% or more of its packets), **2** down (no `hellion`
responding, or a pipeline with no video at all).

## Status file and logs

`hellion` writes its status to `<bus dir>/hellion.<name>` (shared memory, 0660) four
times a second under a seqlock, the same scheme as the frame bus. `top` and `check` only
read it. It is internal to `hellion` — `top` and the daemon are one binary — and carries
a version and size so a mismatched build is reported instead of misread.

Everything `hellion` prints, everything the shared modules print, and the stderr of every
child process go through a pipe that a thread reads: every byte passes on to the
original stdout (the journal, under systemd), and the last 64 lines are kept,
timestamped, for `top`.
