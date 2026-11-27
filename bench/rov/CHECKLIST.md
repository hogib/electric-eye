# A bench session on the vehicle

What to do at the lab, in order, with `bench/rov/rov.sh`. It measures electric-eye's
three ways of sending video and four baselines on the vehicle's own Pi, camera and
tether. Allow about an hour.

**What it touches.** The Pi's camera, CPU and `~/eeye-bench`. It never talks to the
autopilot and never sends a MAVLink message, so arming state is not its business; keep
the vehicle disarmed and the props off or clear all the same. The Pi's installed `eeye`
and `hellion` services are stopped for the session and started again at the end
(`quiet` / `restore`). The laptop's installed electric-eye can stay running: the bench
uses its own ports (RTP 5710, pages 18080/18091) and its own frame bus directory.

## The vehicle, as last seen

From the lab visits of 2026-11-09 to 11-25; `rov.sh check` tells you what is true today.

| | |
|---|---|
| Pi | Raspberry Pi 5, 8 GB, Ubuntu 24.04 (not BlueOS), passive heatsink |
| Address | Pi `192.168.2.10`, login `bluerov2`; laptop `192.168.2.1` on the USB Ethernet adapter (NetworkManager profile `BlueROV`), over the Fathom-X tether |
| Camera | USB, MJPEG on `/dev/video0`, its own H.264 on `/dev/video2`: no V4L2 controls there (no keyframe request, no bitrate), a keyframe about every 30 frames, about 11 Mbit/s |
| sudo on the Pi | asks for a password: `quiet` and `restore` prompt you in your terminal |
| Clock | no RTC; it has been days off after a power cycle |
| Heat | 50 to 62 °C in normal use with the heatsink; four busy cores reach the soft limit (83 to 85 °C) within about a minute |
| Trouble seen | `unattended-upgrade` starts by itself some 25 minutes after boot and holds a core; stopping its timer does not end a run already going |

## Before leaving

1. Build and check what goes to the Pi, at home where there is a network:
   ```sh
   make && make -C bench
   bench/rov/rov.sh kit aarch64      # downloads v0.3.4's arm64 binaries, builds ee_run/bus_cat
   uv run bench/latency/g2g.py --selftest
   ```
   `kit` needs an aarch64 cross compiler (`aarch64-linux-gnu-gcc`, or `CROSS=` a
   Bootlin toolchain's prefix: the release binaries' compiler).
2. For the GStreamer cases the laptop needs `rtph264depay`, `rtpjitterbuffer` and
   `avdec_h264` (Arch: `gst-plugins-good gst-libav`). The Pi needs `gst-launch-1.0`
   with `rtph264pay`, `h264parse`, `jpegdec`, `x264enc`; `check` says which are there.
   Installing on the Pi is the lab's decision, not the bench's: cases whose tools are
   missing are skipped and say so.
3. For latency: a screen the camera can be pointed at. The laptop's own is enough.

## At the lab

```sh
bench/rov/rov.sh check
```
Read every line that starts with `!!`. No address on the laptop: bring the `BlueROV`
profile up. No ssh without a password: `ssh-copy-id bluerov2@192.168.2.10`, with the
Pi owner's say. `unattended-upgr` running: wait for it.

```sh
bench/rov/rov.sh deploy     # ~/eeye-bench on the Pi; no sudo, nothing installed
bench/rov/rov.sh quiet      # stops the Pi's eeye, hellion and apt timers; records what was running
bench/rov/rov.sh camera     # formats of every node -> camera.txt; a clip of real frames for `effects`
```
`camera.txt` answers the first open question of issue #3: which node has H.264, at
what sizes and rates, and whether there is any control for keyframes or bitrate. If the
nodes are not `/dev/video0` and `/dev/video2`, set `CAM_MJPEG=` and `CAM_H264=` for
everything after.

Point the camera at something with detail for the clip (not a blank wall): hysteresis
and edge counts depend on the scene.

```sh
bench/rov/rov.sh effects    # every effect, 1/2/4 threads, on the Pi -> effects.txt
```
This is the table for the paper's "real-time" claim, and the replacement for the
x86-only figures in `docs/effects.md`. Timed by `ee_run`, which links eeye's own
sources; the cases with `eeye` in them also log eeye's own `effects:` line, which is the
shipped binary's figure.

```sh
bench/rov/rov.sh all 120    # every case, two minutes each, with a cool-down between
```
Cost on the Pi and decoded frame rate on the laptop for each case, into
`summary.md`. Watch for `!!` lines: a throttled run is marked, and should be repeated.

Then latency. Put the Pi's camera in front of the laptop's screen so the whole screen
is in the picture and fills most of it, in a dim room if the screen reflects. Then:

```sh
bench/rov/rov.sh run ee-a-h264 60 --g2g
bench/rov/rov.sh run ee-b 60 --g2g
bench/rov/rov.sh run ee-a-x264 60 --g2g
bench/rov/rov.sh run ffmpeg-copy 60 --g2g
bench/rov/rov.sh run gst-copy 60 --g2g
bench/rov/rov.sh run ffmpeg-x264 60 --g2g
bench/rov/rov.sh run gst-x264 60 --g2g
```
The screen shows a calibration (20 s) and then the running pattern. If calibration
fails it says which cell the camera did not see. Do not move the camera or the laptop
between cases. Run the set twice if there is time: the second pass shows how far the
figures repeat.

What the pilot actually sees in QGroundControl cannot be read by a program. For that
one, film a running millisecond stopwatch and QGroundControl's picture of it in one
photograph, twenty times, and note the differences by hand; it is the check on the
automatic figures, not a replacement for them.

Other things worth doing while connected, by hand:

- **Keyframe request** (`ee-a-h264` running): restart the laptop's side and time how
  long the picture takes to come back. `pi-hellion.log` says whether the camera took
  the control or the stream was restarted.
- **Cable pull**: unplug the tether for 5 s during `ee-b` and `ee-a-h264`; note the
  time to a picture after replugging. Pulling the laptop's USB adapter costs about 7 s
  by itself (it re-enumerates): pull at the Fathom-X, not at the laptop.
- **`install.sh --camera-h264`** has never run on a real Pi. It needs sudo and replaces
  the installed config (kept as `.bak`), so it is the lab's call; if it is run, run
  `sudo ./install.sh --camera-h264 off` afterwards unless they want to keep it.

## Before leaving the lab

```sh
bench/rov/rov.sh restore    # stops anything left, starts what `quiet` stopped, compares with what was found
bench/rov/rov.sh status
```
`restore` must end with "the Pi's services are as found". It leaves `~/eeye-bench` on
the Pi and says how to remove it.

Results are in `~/eeye-results/<date>-rov/`: `inventory.txt`, `camera.txt`,
`effects.txt`, one folder per case, `summary.md`. Nothing is uploaded anywhere.

## What these figures are and are not

- **CPU** is of one core (100 = one core busy), summed over the programs named for the
  case, sampled from `/proc` once a second by the same script for every case.
- **Latency** is from a screen refresh to the decoded frame on the laptop, through the
  camera's lens (`bench/latency/g2g.py` explains it). It has one display in the loop,
  as glass-to-glass has, but it is the pattern's display, not QGroundControl's. Each
  frame's figure is uncertain by about one refresh plus one exposure; use the median
  and percentiles.
- **The baselines** are plain ffmpeg and GStreamer pipelines with their low-latency
  settings, each received by its own stack. They are not BlueOS: this Pi does not run
  it. The published BlueOS figures are in `bench/README.md` for context, and are not
  measured here.
- A dry run without the vehicle: `ROV=local bench/rov/rov.sh ...` uses this machine and
  its webcam as the Pi. Both ends then share one CPU, so its numbers mean nothing; it
  shows that the session runs.
