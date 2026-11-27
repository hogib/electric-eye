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
| Cost of each transport case (`rov.sh run`, `all`) | all seven cases run in the dry run and deliver 30 fps; the three H.264 ones with a v4l2loopback device carrying H.264 as the camera (`CAM_H264=/dev/video10`), not a camera that encodes |
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
- **`fusion` and `udcp` are our code**, written from the papers because no public code
  from their authors was found. They were audited against the papers and the public
  ports on 2026-11-27 (below). If a figure for them looks wrong, suspect the
  implementation before the method.
- **Times of the Python methods** are of OpenCV's compiled code (`clahe`, `grayworld`,
  `he`) or of unoptimised numpy (`fusion`, `udcp`). The first group is a fair speed
  comparison; the second is not one.

## The audit of `fusion` and `udcp`

Both were read step by step against the papers and against the public ports
(2026-11-27; four ports of the 2018 fusion, one collection with UDCP). Verdict for
both: faithful, with deviations that are now either removed or stated in the
functions' comments.

**UDCP** follows eq. 10 of the 2013 paper as written (15×15 patches, no factor on the
dark channel, the brightest pixel among the top 0.1% for the water's colour). The paper
gives no floor for the transmission and no recovery formula; He et al.'s are used, and
a guided filter in place of matting. An outside check exists for this one: the UIEB
paper ran the authors' own code on the same 890 pictures and reports 11.03 dB and an
SSIM of 0.4999 (its Table II).

**Fusion** has no such check. The UIEB paper's "fusion-based" row (18.75 dB, 0.8162)
is the *2012* fusion method in UIEB's own reimplementation, and a quarter of UIEB's
references were chosen from that method's outputs; it says nothing about the 2018
method here. The 2018 paper leaves open the gamma of its first input and the Gaussian
of its sharpening, and its normalisation of the sharpened detail ("to the full range")
is sensitive to single pixels. On 60 of the 890 pictures (every 15th), PSNR in dB and
SSIM against the references:

| gamma | stretch to the full range (the paper's wording) | stretch between the 1st and 99th percentiles |
|---|---|---|
| 1.0 | 16.36, 0.772 | 16.81, 0.827 |
| 1.2 | 16.19, 0.774 | 16.73, 0.828 |
| 1.5 | 15.56, 0.766 | 16.28, 0.824 |
| 2.0 | 14.27, 0.740 | 15.24, 0.807 |

(The untouched pictures of that subset: 16.26, 0.724.) The bench runs gamma 1.2 with
the percentile stretch: the best setting that is still a gamma correction, so that the
baseline is the method at its best and not at our guess. `FUSION_GAMMA` and
`FUSION_NORM=minmax` in the environment give the others. Every public port read departs
from the paper where this implementation does not (the red compensation's formula, a
contrast weight that is not a Laplacian, histogram equalisation in the sharpening), so
none was substituted for it.

**UCIQE and UIQM**: UIQM here is the FUnIE-GAN repository's computation, with 0.114
for 0.144 and at each picture's own size (its script resizes to 256×256 first). Public
UCIQE versions differ in how CIELAB is scaled and in two of the three terms; this one
gives values of the size the literature reports. None of them is interchangeable with
another.

**Other methods through `--external`**: MLLE is distributed as MATLAB p-code, so it
needs MATLAB itself (Octave cannot run it). FUnIE-GAN's `test.py` writes input and
output side by side and resizes to 256×256, so it needs a small wrapper that saves the
output alone; its PyTorch weights are described by the authors as a sample model, the
TF-Keras ones as the official ones.

