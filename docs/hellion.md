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
page with preview, snapshots, recording and `eeye`'s config (here or across the
tether); the ROV↔topside status exchange; `top` and `check`; installed as a systemd
service by `install.sh`.

## Installing

```sh
sudo ./install.sh --role rov|topside --placement a|b [--topside-ip IP --rov-ip IP]
                  [--tether IFACE] [--reset-config]
```

Both ends get the same placement, and the same addresses. The addresses default to the
BlueROV's (topside 192.168.2.1, ROV 192.168.2.2); given, they replace those in the
example config as it is written (the previous file kept as `.bak`) — the `peer` topside,
the `rtp:` sink on the ROV — and are remembered in `/etc/hellion/install.conf`, which is
read as `KEY=value` data, never sourced. They must be IPv4 addresses: the release
binaries are static and cannot resolve names. What it sets up:

| | ROV | topside |
|---|---|---|
| **placement a** | `hellion` (camera → RTP) | `hellion` (RTP → bus `camera`; bus `pilot` → QGC, `/dev/video10`), `eeye` (`camera` → `pilot`) |
| **placement b** | `eeye` (camera → `pilot`), `hellion` (`pilot` → RTP) | `hellion` (RTP → QGC unchanged, bus `pilot`, `/dev/video10`) |

- **Binaries** are copied to `/usr/local/bin`, not run from the checkout: home
  directories are often 0700, which the service user cannot enter.
- **One service user, `eeye`, in group `video`**, runs both services. `hellion` writes
  `eeye`'s config and reads the `.status`/`.health` files beside it, and the two share
  the frame bus; the bus and status files are mode 0660, so anyone in `video` can run
  `hellion top`. The installing user is added to `video`.
- **Configs:** `/etc/hellion/hellion.json` from `hellion/examples/<placement>-<role>.json`;
  on the machine running `eeye`, `/var/lib/eeye/effects.json` with the source for the
  placement (`capture_source: "bus"` topside in A) and `output_device: ""` (the
  loopback device is `hellion`'s). Existing configs are kept unless `--reset-config`,
  which keeps the old ones as `.bak`.
- **Units:** `eeye.service` and `hellion/hellion.service`, as installed. A placement in
  which `eeye` does not run on this machine *removes* an `eeye.service` left by an
  earlier install: on the ROV in placement A it would hold the camera `hellion` needs.
  Both units use `KillMode=mixed`, so a stop signals the main process alone and
  `hellion` can finish a recording properly instead of having its ffmpeg killed with it.
- **v4l2loopback** (`/dev/video10`) is loaded at boot topside only.
- **Packages** (`apt-get`): a compiler, `pkg-config`, `libturbojpeg0-dev`, `ffmpeg`, and
  `v4l2loopback-dkms` topside. Without `apt-get` it lists what to install and you
  re-run with `--no-packages`.
- **Re-running** is the update path. The role and placement are remembered in
  `/etc/hellion/install.conf`, and the services are restarted on the new binaries.

The installer was tested in a sandbox (a user namespace with the system directories
replaced and the system commands stubbed) for every role and placement, a re-run, and
a switch of placement; not yet on a real Debian or Raspberry Pi OS machine.

### The tether's network

BlueROV addressing is fixed: topside **192.168.2.1**, ROV **192.168.2.2**, both /24, and
the examples use it. The ROV's network belongs to BlueOS, and `install.sh` never touches
it. Topside, `--tether IFACE` creates a NetworkManager profile, `hellion-tether`, giving
`IFACE` 192.168.2.1/24 — and an IPv6 link-local address, which is not decoration:

**NetworkManager needs at least one address family to succeed**, or it tears the
connection down — removing the address — and retries. On a bare cable nothing answers
DHCP or sends router advertisements, so a profile whose families all wait for one cycles
through 75-second outages while the link itself never drops (`carrier_changes` in
`/sys/class/net/<iface>/` does not increment, and `ip link` says `UP` throughout: check
for an *address*, not for "up"). A manual IPv4 address avoids that on its own; link-local
IPv6 keeps the profile up even if someone later switches IPv4 back to DHCP.

`install.sh` does not activate the profile if the interface is connected right now: the
script is often run over SSH, quite possibly through that interface. It applies on the
next replug or reboot, or at once with `nmcli connection up hellion-tether`.

### Running it by hand

For development, from the checkout:

```sh
make                                              # builddir/eeye and builddir/hellion
builddir/hellion -c hellion/examples/b-topside.json
```

`record_dir` and `snapshot_dir` default to `/var/lib/hellion/...`; set them in the config
when running as yourself. `EEYE_BUS_DIR` moves the frame bus out of `/dev/shm`, so a test
setup does not meet the installed services' streams.

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
| `record_dir` | `/var/lib/hellion/recordings` | Where recordings are saved |
| `record_from` | first `rtp:` source | The pipeline whose arriving stream is recorded |
| `camera_controls` | `""` | `"peer"` (topside, placement A): send the page's camera controls on to the peer, which has the camera. Needs `peer` |
| `camera_file` | `/var/lib/hellion/camera.json` | With a camera source: where the last camera controls are kept, and put back from on start |
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

### Camera controls in placement A

The camera is on the ROV under `hellion`; `eeye`, and with it the config the page
edits, is topside. So the topside `hellion` (`"camera_controls": "peer"`, in
`a-topside.json`) sends the `"camera"` object of every config `eeye` accepts to the
ROV's `POST /camera`, and adds the answer to the page's response as `"camera"`:
applied, kept for when the camera opens, or an error naming what failed (the page shows
that in red — the sliders would otherwise show values the camera is not using).

On the ROV, the controls go to every camera source:

- **V4L2**: the source thread applies them between frames (a generation counter it
  checks per frame), and again on every open, since a replugged camera comes back at
  its factory defaults.
- **rpicam**: `rpicam-vid` takes them only on its command line, so a change restarts
  it (~1s, logged), as in `eeye`.
- They are written to `camera_file` first and put back on start, so a restarted ROV
  `hellion` keeps the pilot's exposure. Defaults as in `eeye`: `keep_framerate` on,
  nothing else touched.

Tested with a laptop webcam as the ROV: manual exposure, gain and brightness reached
the camera's registers 99–160 ms after the request; reset by hand while the ROV's
`hellion` was down, they came back when it restarted; with it down, the page's
request still applied the effects and reported "Connection refused" for the camera.

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
presumed wedged and restarted. That clock only runs while packets arrive: a decoder
started during an outage is waiting, not wedged, and gets its 5s from when the video
returns (an earlier version killed it the moment packets resumed, and the picture came
back ~8s late after a cable pull; it now returns ~1.1s after the packets do).

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
| `POST /record` | `{"action":"start"}` / `{"action":"stop"}` — see [Recording](#recording) |
| `GET /api/status` | This `hellion`'s pipelines as JSON, for scripts |
| `POST /camera` | Camera controls (a `"camera"` object, as in `eeye`'s config) for this `hellion`'s camera sources: kept in `camera_file`, applied, answered once the camera has them — `{"applied":true,"controls":N}`, or `applied:false` with why. 409 without a camera source |
| `GET /camera` | The camera controls kept here |
| `POST /api/peer` | The status exchange between the two ends — see [ROV ↔ topside status](#rov--topside-status) |

**Auto visibility** is the first thing on the page: a full-width button (and the `A`
key) that writes `"auto_visibility"` into `eeye`'s config like any other change —
locally, or through the ROV in placement B — and turns back if `eeye` refuses. The
button shows what was asked for; an **AUTO VISIBILITY** badge on the picture shows
what `eeye`'s health file says it is doing, so the two can never be confused. Under it,
a histogram of the camera's raw picture and notes in words (crushed blacks, blown
highlights, a colour cast, low contrast), from the `image` block of `eeye`'s health
(see [output.md](output.md#health-reporting-healthc)).

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

## Recording

Topside, from the page's **Record** button (`POST /record` with
`{"action":"start"}` / `{"action":"stop"}`): the H.264 stream **exactly as it arrives
over the tether**, copied into an MP4 by an ffmpeg child — no decode, no re-encode, no
quality lost, almost no CPU. At the default 4 Mbit/s that is ~1.8 GB/hour.

- **What it records.** `record_from`, the first pipeline with an `rtp:` source by
  default. In placement B that is the pilot's picture as the ROV sent it; in placement
  A it is the ROV's camera, *before* effects.
- **How.** Starting attaches a local port to that pipeline's UDP relay, so the recorder
  gets the same packets as the decoder and QGC and takes nothing from them.
- **Fragmented MP4.** Every keyframe closes a fragment that plays on its own, so a
  recording cut off by a crash or a power loss **plays up to its last keyframe** (tested:
  `kill -9` mid-recording left a file that plays to within a second of the kill). An
  earlier recorder used `+faststart` "so the file still plays if the boat loses power";
  that does the opposite — it writes the index only at the end, so an interrupted file
  has none.
- **Stopping** sends ffmpeg SIGINT while packets are still flowing (it only notices the
  signal when its next read returns) and takes about half a second; during an outage,
  when no read will return, about a second, and the file is still complete.
- **Refused** when under 500 MB is free in `record_dir`, when already recording, or
  before the pipeline is receiving. One recording at a time.
- **If the stream stops** (cable pulled, ROV restarting), ffmpeg's RTP input gives up
  after ~20s and finalizes the file, but the recording is only **paused**: the page and
  `top` show *waiting for video*, and when packets return it continues into
  `<name>-part2.mp4`, then `-part3.mp4`, each playable on its own. A new file rather
  than an append, because the encoder on the ROV may have restarted with new
  parameters. Stop works while paused. The elapsed time counts from the first part.

The page shows elapsed time and size while recording. `eeye`'s own raw recording
(`record_path`, full quality, before effects, enormous) is separate and still on its
config.

## ROV ↔ topside status

Once a second the side with `peer` set POSTs a few lines describing itself to
`/api/peer` on the other side, and gets the other side's lines back. Both keep what they
received, so **`hellion top` on either machine shows the far end** — on the ROV that
includes what the topside actually receives (bitrate and loss), which the ROV has no
other way to know:

```
HELLION "rov"  role rov  pid 3999433  up 2m  responding
 pipeline pilot-up  640x480
  source bus:pilot                  running      30.0 fps      3412 frames
  sink   rtp:192.168.2.1:5610       sending      30.0 fps      3411 frames      0 dropped  libx264 4000k
           link out                    4066 kbit/s    0.0% loss  ...
 peer 192.168.2.1  in contact
   | topside (topside), pid 3999434
   | pilot-in: rtp:5610 running 30 fps, 4036 kbit/s 0.0% loss
   |   -> bus:pilot-top publishing 30 fps
   | recording dive-20261101-191900.mp4, 16s, 2 MB
```

Plain text lines, not JSON: they are for people, nothing parses them, and the receiver
keeps only printable ASCII (16 lines of 111 characters at most). `hellion check`
warns when a configured peer has not answered for 5s, with the reason.

## Child processes and crashes

Every ffmpeg `hellion` runs is in its own process group, closes inherited descriptors,
and is stopped in order (see `src/child_proc.c`). If `hellion` itself dies, its children
do not outlive it for long: encoders read their frames from `hellion` and see EOF;
decoders and the recorder lose their packets and ffmpeg's RTP input gives up after
~20s (finalizing the recording). The decoder's and recorder's stderr go through
`hellion`, which drops the H.264 parser's per-frame complaints (counted for the
decoder, see [Decoder damage](#rtp-how-the-video-crosses-the-tether)) and logs anything
else.

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
  auto visibility ON  camera picture: luma 44/101/147 (1/50/99%)  0.0% black  0.0% clipped  cast 20/-35
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
frames, a receiving link losing 1% or more of its packets, a peer not answering),
**2** down (no `hellion` responding, or a pipeline with no video at all).

## Status file and logs

`hellion` writes its status to `<bus dir>/hellion.<name>` (shared memory, 0660) four
times a second under a seqlock, the same scheme as the frame bus. `top` and `check` only
read it. It is internal to `hellion` — `top` and the daemon are one binary — and carries
a version and size so a mismatched build is reported instead of misread.

Everything `hellion` prints, everything the shared modules print, and the stderr of every
child process go through a pipe that a thread reads: every byte passes on to the
original stdout (the journal, under systemd), and the last 64 lines are kept,
timestamped, for `top`.
