# The comparison bench

What a referee would ask of "real-time image enhancement and low-latency video
transport", and the tools that answer it. Three parts:

| Part | Where it runs | Answers |
|---|---|---|
| [`rov/`](rov/CHECKLIST.md) | the vehicle's Pi and the laptop, over the tether | what each effect costs on the Pi; what each way of sending video costs there; latency through the lens; against plain ffmpeg and GStreamer pipelines |
| [`latency/`](latency/g2g.py) | the laptop | the latency of any video path whose decoded frames can be piped to it |
| [`quality/`](quality/quality.py) | the laptop | eeye's auto visibility against published enhancement methods, on stills and on video |

Nothing here is part of `make check`, and nothing here is installed by `install.sh`.

```sh
make && make -C bench                        # ee_run and bus_cat, next to eeye and hellion
uv run bench/latency/g2g.py --selftest       # the latency tool against a simulated camera
ROV=local bench/rov/rov.sh check             # a dry run of a vehicle session on this machine
```

## Status

Built and run on the development laptop only. **No figure in the paper should come
from a run on this laptop except the quality scores**, which do not depend on the
machine. The vehicle session has been dry-run with a webcam in place of the Pi
(`ROV=local`), which shows that it runs, not what it will measure.

| | State |
|---|---|
| Effects timing (`rov.sh effects`) | runs; needs the Pi |
| Cost of each transport case (`rov.sh run`, `all`) | electric-eye's and ffmpeg's cases run in the dry run; the H.264 cases need a camera that encodes; the GStreamer cases have not run anywhere (the laptop lacks `gst-plugins-good` and `gst-libav`) |
| Latency through the lens (`g2g.py`) | passes its self-test against a simulated screen and camera; has not yet looked at a real screen through a real camera |
| Quality on stills and video (`quality.py`) | runs on synthetic pictures; needs a real dataset (below) |
| Transport under packet loss and delay | **not built**: it needs `tc netem`, so root, and a design for what is compared |
| A learned baseline (FUnIE-GAN or similar) | **not wired in**: the `--external` hook is there, the model is not |

## What each tool measures

**`ee_run`** (`ee_run.c`) is eeye's effects offline: frames from a file through
`img_stats`, auto visibility and the effect chain, the same functions linked from
`src/`, with the time each frame took. It is what makes the effects comparable with
other methods on a dataset, and timeable on the Pi without a camera in the loop.
`--still` treats every frame as its own picture; without it the file is a video, and
auto visibility smooths over it as it does live.

**`bus_cat`** (`bus_cat.c`) writes a frame bus stream's luma to stdout, for `g2g.py`.

**`rov/psample.py`** samples named programs from `/proc` once a second: CPU, memory,
temperature, clock, the Pi's throttle flags. One method for electric-eye and for the
stacks it is compared with. `hellion bench` gives more detail for electric-eye
(threads, frame rates, the link) but follows only `eeye` and `hellion`.

**`latency/g2g.py`** shows a pattern that encodes each screen refresh's number, reads
the number back out of the decoded video, and reports the distribution of the time
between the two. The whole path is in it: screen, lens, sensor, camera, ROV software,
tether, topside software. Its header explains the method, its resolution, and how it
differs from a photodiode on the pilot's display.

**`quality/quality.py`** scores every method on a folder of stills (UCIQE, UIQM, PSNR
and SSIM against references, ORB keypoints, time) and on a clip at 1280×720 (flicker,
features matched between frames, time per frame).

## Baselines, and why these

Chosen from a survey of published systems with code that runs (2026-11-27). Two
findings shape the bench:

- **No published enhancement method reports CPU-only 720p timing on a Pi.** Classical
  methods are timed in MATLAB on a desktop (in the UIEB paper's table, at 1280×720:
  fusion 1.84 s, UDCP 9.90 s), learned ones on a GPU at 256×256. So every method is
  timed again here, on one machine, at one size; published times are not copied into
  a table next to ours.
- **No controlled glass-to-glass measurement of the BlueOS camera stack, or of a plain
  RTP pipeline on a Pi, has been published.** The figures in circulation are forum
  posts and vendor claims (for BlueOS's `mavlink-camera-manager`, a developer's "128
  to 172 ms at 1080p30" on a Pi 3). They are context, not a baseline to beat on paper;
  the baseline is what is measured on the same vehicle.

Enhancement:

| Method | Why | Here as |
|---|---|---|
| CLAHE, gray-world, both (OpenCV) | the fairest: classical, compiled, what an operator would otherwise do | `clahe`, `grayworld`, `grayworld-clahe` |
| Histogram equalisation | the naive floor | `he` |
| Colour balance and fusion (Ancuti et al., IEEE TIP 2018) | the standard classical quality reference (UIEB rates it the best classical method) | `fusion`, this bench's implementation from the paper |
| UDCP (Drews et al., 2013) | the standard physical-model reference | `udcp`, this bench's implementation from the paper |
| MLLE (Zhang et al., IEEE TIP 2022) | the closest published method in spirit: statistics-driven, classical, CPU | not included: MATLAB, "academic research purpose only"; run it through `--external` with Octave or MATLAB |
| FUnIE-GAN (Islam et al., RA-L 2020) | the standard learned "real-time on a robot" baseline; the paper gives 7.9 fps on an i3 CPU at 256×256 | not included: `--external`, see below |

Transport, on the vehicle (`rov/rov.sh`):

| Case | What it is | Compared with |
|---|---|---|
| `ffmpeg-x264`, `gst-x264` | MJPEG from the camera, encoded with x264 `ultrafast`/`zerolatency` at 4 Mbit/s, sent as RTP | `ee-a-x264`, `ee-b`: the same encoder under hellion |
| `ffmpeg-copy`, `gst-copy` | the camera's own H.264 sent as RTP | `ee-a-h264`: hellion's own packetizer |

Each stack is received by itself with its low-latency settings (ffmpeg with the flags
hellion gives its decoder; GStreamer with `rtpjitterbuffer latency=0`). BlueOS is not a
case because this vehicle's Pi does not run it; where one does, its UDP stream on 5600
can be measured with `g2g.py` and the `gst-*` receiver unchanged.

## Data for the quality part

None is in the repository: the standard sets are not ours to redistribute.

| Set | For | Get it from | Then |
|---|---|---|---|
| UIEB (890 pictures with references, 60 without) | `images`, with PSNR/SSIM | <https://li-chongyi.github.io/proj_benchmark.html> (non-commercial, academic use) | `--data raw-890 --ref reference-890` |
| U45, RUIE | `images`, no references | their GitHub repositories | `--data DIR` |
| UVEB (video pairs, CVPR 2024) | `video` | <https://github.com/yzbouc/UVEB> | `--clip FILE` per clip |
| The vehicle's own footage | `video` | a recording from a dive, or `rov.sh camera`'s clip for a first look | `--clip FILE` |

References in UIEB are the best of twelve methods' outputs as chosen by people, not
ground truth: a high PSNR means "like what people preferred", no more.

```sh
uv run bench/quality/quality.py images --data ~/data/UIEB/raw-890 --ref ~/data/UIEB/reference-890 --out ~/eeye-results/quality-uieb
uv run bench/quality/quality.py video --clip dive.mp4 --out ~/eeye-results/quality-dive --save
```

### Another method (`--external`)

Any method that can turn a folder of PNGs into a folder of PNGs with the same names:

```sh
uv run bench/quality/quality.py images --data RAW --out OUT \
    --external 'funie=python /path/to/FUnIE-GAN/PyTorch/test.py --data_dir {in} --sample_dir {out} --weights_path funie_generator.pth' \
    --external 'mlle=octave --eval "mlle_folder(\"{in}\", \"{out}\")"'
```

(The commands are sketches: each project's script has its own arguments.) Its time is
the whole command's over the number of pictures, start-up included, so quote it only
with that said; time a learned model properly with its own loop.

## Reading the results honestly

- **UCIQE and UIQM reward saturation and contrast**, whether or not the picture is
  better; the UIEB paper found their rankings disagree with people's. They are in the
  table because every paper has them. PSNR/SSIM against references, keypoints, and the
  video measures are the ones to argue from.
- **Flicker** is given three ways because the plain figure also rises with contrast
  and with motion in the scene; `flicker_rel_vs_input` is the one that isolates what
  the method adds.
- **`fusion` and `udcp` are our code**, written from the papers because no official
  code exists. If a figure for them looks wrong, suspect the implementation before the
  method.
- **Times of the Python methods** are of OpenCV's compiled code (`clahe`, `grayworld`,
  `he`) or of unoptimised numpy (`fusion`, `udcp`). The first group is a fair speed
  comparison; the second is not one.
