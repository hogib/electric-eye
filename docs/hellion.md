# hellion

The tether relay: everything that moves video between machines, and the tools for
seeing what it is doing over SSH. `eeye` does the vision; `hellion` does the moving.
Source: `hellion/`, built by the same Makefile as `builddir/hellion`.

```sh
hellion [-c /etc/hellion/hellion.json]   # run
hellion top [-n name] [--once]           # live dashboard
hellion check [-n name]                  # health check: exit 0 ok, 1 degraded, 2 down
```

**Status:** pipelines, sources (camera, rpicam, frame bus), sinks (frame bus,
v4l2loopback), `top` and `check` are built. RTP over the tether, QGroundControl
output, recording, the browser page and config relay come next (see the plan in the
commit history).

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
| `eeye_config` | `""` | `eeye`'s config on this machine, if it runs here: `top` and `check` read its health beside it |
| `pipelines` | — | 1 to 8; see below |

A **pipeline** moves frames from one source to up to 8 sinks:

| Source | |
|---|---|
| `v4l2:<device>` | A V4L2 camera, MJPEG or YUYV, through `src/v4l2_in.c` |
| `rpicam` | A Pi camera module, through `rpicam-vid` (`src/rpicam_in.c`) |
| `bus:<stream>` | A frame bus stream; the size comes from the stream |

| Sink | |
|---|---|
| `bus:<stream>` | Publish to the frame bus ([bus.md](bus.md)) |
| `v4l2:<device>` | A v4l2loopback device. Optional and retried, like `eeye`'s ([output.md](output.md#optional-and-retried)) |

Camera sources take `width`/`height` (default 1280×720, even numbers) and `format`
(`auto`, `mjpeg`, `yuyv`; `yuv420` for `rpicam`). A pipeline cannot read the bus stream
it publishes.

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
  frames for Ns` before the camera code gives up on it (~10s).
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
frames), **2** down (no `hellion` responding, or a pipeline with no video at all).

## Status file and logs

`hellion` writes its status to `<bus dir>/hellion.<name>` (shared memory, 0660) four
times a second under a seqlock, the same scheme as the frame bus. `top` and `check` only
read it. It is internal to `hellion` — `top` and the daemon are one binary — and carries
a version and size so a mismatched build is reported instead of misread.

Everything `hellion` prints, everything the shared modules print, and the stderr of every
child process go through a pipe that a thread reads: every byte passes on to the
original stdout (the journal, under systemd), and the last 64 lines are kept,
timestamped, for `top`.
