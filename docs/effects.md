# Effects

The chain runner, the operators, and the two optimizations that shape how they are
written.

## Two kinds of operator

The distinction runs through everything in `effect_chain.c`:

- **Point ops** — output pixel depends only on the *same* input pixel. `none`,
  `grayscale`, `invert`, `threshold`, `tint`, `light`. `is_point_op()` lists them.
- **Neighborhood ops** — output pixel depends on surrounding pixels. `blur`, `sobel`,
  `log`, `canny`.

Plus one that fits neither: **`contrast`**, which is a point op whose lookup table
depends on the *actual min/max luma present in this frame at this point in the chain*.
It cannot be composed ahead of real pixel data, so it gets its own branch.

## LUT fusion (point ops)

A run of consecutive point ops is folded into **one 256-entry lookup table per
channel**, built once, and applied in a single pass — instead of N full-frame passes.

This works because each point op is a pure function of one input byte, and Y is
independent of U/V: only invert/threshold ever touch Y, only grayscale/tint ever touch
U/V. So N point ops on the same channel compose exactly like function composition:

```c
lut_c[v] = stage_op(lut_c[v]);   // for every v, per stage
```

which mirrors what the unfused code would have done by re-reading whatever the previous
stage just wrote.

`touched_y` / `touched_u` / `touched_v` come back true only if some stage in the run
actually touches that channel, so a Y-only run (invert, threshold) never pays for a
no-op pass over U/V.

`apply_lut_plane()` has a **constant-LUT fast path**: if every entry maps to the same
value — which is exactly what plain `grayscale` produces for chroma — it `memset`s and
never reads the source at all.

**`apply_lut_plane` is scalar on purpose.** A per-pixel table lookup is cache-friendly
and beats a NEON gather. That was measured, not assumed.

## The buffer ping-pong (neighborhood ops)

A neighborhood op must read a buffer that no stage is still writing. With two chained
(blur then sobel) that takes a third buffer, which is what `spare` is for.

```c
ChainBuf dst = (cur == CHAIN_SPARE) ? CHAIN_WORK : CHAIN_SPARE;
```

`cur` tracks which buffer holds the chain's current state: `CHAIN_RAW` at the start,
then alternating between `CHAIN_WORK` and `CHAIN_SPARE`.

Point ops always write to `work` and set `cur = CHAIN_WORK`. They read from whichever
buffer is current and write straight into `work` — no separate copy-then-overwrite
step. Only an *untouched* plane needs an explicit copy, and only when `cur` is not
already `work`.

The first neighborhood op reads `raw` and writes `spare`. At the end, the rest of the
pipeline always reads `frame->planes`, and two cases leave the result elsewhere. Both
are fixed by moving pointers, not by copying a frame (1.8MB at 720p):

- **`cur` is `CHAIN_SPARE`** (e.g. a lone `blur` or `sobel`): the work and spare roles
  swap (`vf_swap_work_spare`).
- **`cur` is `CHAIN_RAW`** (an empty chain): work is pointed at raw itself. Nothing
  downstream writes work, so sharing the block is safe, and `vf_restore_roles()` at the
  top of the next chain separates them again before any stage can write.

See [architecture.md](architecture.md#videoframe) for how the roles and allocations
relate.

### Chroma nobody will see

Before running, the chain works out back to front whether each stage's chroma is ever
looked at (`compute_chroma_liveness`). `sobel`, `log`, `canny` and `grayscale`
overwrite chroma without reading it; `invert`, `threshold`, `contrast` and `none` pass
it through; `tint` and `light` read it. A `blur` whose chroma is dead blurs luma only
(`gaussian_blur_luma`).

That is the shipped `blur → sobel` chain: the two chroma planes hold as many pixels as
luma, and sobel resets them to 128 anyway, so it is about half the blur's work. The
output is byte-identical, which is what the tests check.

## The operators

### `blur` — 5-tap separable Gaussian

`[1,4,6,4,1]/16`, applied horizontally then vertically: 10 taps per pixel instead of a
2D kernel's 25. The weights sum to a power of two, so normalizing is a rounded right
shift — integer-exact, no float, no accumulated rounding error.

`blur_strength` repeats the pass. 0 and 1 both mean one pass.

**One sweep per pass, one OpenMP region per call.** Each thread owns a band of rows.
Horizontal results go into a private 5-row rolling window (a few KB, in L1), and each
output row is written as soon as its five inputs exist, instead of writing the whole
horizontal result to memory and reading it back. All planes and passes of one
`gaussian_blur` run inside a single `#pragma omp parallel`, where the old structure
opened a region per horizontal pass and per vertical pass for each plane (six for the
default chain). Repeated passes blur in place: each thread first blurs the two rows on
either side of its band, a barrier lets everyone start writing, and from then on a
thread only reads rows inside its own band that it has not yet written. The arithmetic
is unchanged, so the result is bit-identical.

The horizontal NEON loop loads 16 pixels' 5-tap window with two loads and `vextq_u8`
instead of five overlapping loads.

**Half-resolution fast path** at `halfres_blur_pass_threshold = 4`: downsample 2×2, run
the repeated passes at a quarter the pixels, upsample back. Below 4 the fixed
downsample/upsample cost eats most of the savings, and a low pass count is the "just
soften this a little" case most likely to be scrutinized. Measured ~3.3× faster at
strength 255. For an odd-width plane (the chroma of a frame whose width is 2 mod 4, e.g.
1366 → 683), the upsample replicates the last column; it used to leave that column
unwritten, showing stale bytes from the buffer's previous use.

Note the visible effect grows roughly with the square root of the pass count while cost
is linear, which is why the web UI caps the slider at 20.

### `sobel` — gradient magnitude

Standard 3×3 kernels. Uses the **L1 norm** `|Gx| + |Gy|` rather than
`sqrt(Gx² + Gy²)` — visually near-identical, no float, no sqrt. That is what realtime
edge detectors use in practice.

`sobel_threshold` clamps magnitudes below it to 0, *after* the 255 clamp.

Borders **replicate** rather than zero-pad. Zero-padding a bright edge reads as a false
hard line running along the frame boundary that has nothing to do with the scene.

### `log` — Laplacian of Gaussian, zero-crossings

Smooth, then take the second derivative, then mark where it crosses zero. Produces thin,
closed 1px contours rather than Sobel's thicker gradient ridges.

`log_strength` is Gaussian passes, so it sets sigma (which grows as its square root).
It reuses the blur's full-resolution path (`blur_luma_into`), so it inherits an
already-verified NEON kernel rather than introducing a second smoothing path.

`log_threshold` is a **slope test**, not a plain sign comparison: comparing signs alone
marks a crossing wherever the response merely grazes zero, which in a smooth region is
just sensor noise. The magnitude of the jump across the crossing has to clear the
threshold too.

Each pixel is compared against its **right and lower neighbour only**. Checking all four
would mark both sides of every crossing, doubling every contour's width and losing the
thin-line property that is the entire reason to use this operator.

### `canny` — the selective one

Four stages: Gaussian smooth, gradient with quantized direction, non-maximum
suppression, hysteresis. Weak edges survive only where they connect to a strong one.

Three decisions here were settled by measurement and are worth not re-litigating:

**The Gaussian is mandatory, not a quality knob.** `canny_strength` of 0 means 1 pass.
Hysteresis cost is content-dependent: on unsmoothed noise it measured **10.81 ms**
against 0.48 ms on structured content. One blur pass costs 0.07 ms and drops that to
0.51 ms — a 21× reduction. Skipping it to save 0.07 ms risks 10 ms.

**Gradient and NMS are two separate passes, not fused.** Fusing keeps a 3-row window in
cache but recomputes each gradient up to three times: **0.68 ms for two passes against
0.94 ms fused**, at byte-identical output. (An earlier unverified run showed the fused
version at 0.01 ms — that was dead-code elimination, caught by checksumming both.)

**Hysteresis is a serial stack flood fill, not a parallel sweep.** A bounded sweep caps
the worst case (4.31 ms vs 15.53 ms on pure noise) but is **twice as slow on realistic
content** (0.93 ms vs 0.46 ms) and only approximates connectivity rather than computing
it. Since the mandatory blur already tames the noise case, the flood fill wins on the
input Canny actually receives.

What surrounds the fill no longer runs serially. Non-maximum suppression writes a
label straight into the output (255 seed, 1 candidate, 0) and collects each row's
seeds into that row's own region of the stack, in parallel; the fill then promotes
neighbours labelled 1 (one test, where it was two), and a parallel pass turns the
unreached 1s into 0s. That removed a serial memset of the output, a serial scan of the
whole frame for seeds, and the separate suppression plane. Output is byte-identical
(504 combinations of photo, strength and thresholds, at 1, 3 and 4 threads, on x86 and
under qemu for both ARM NEON builds). On 4 x86 threads, 720p photos: 4.19→3.82,
3.80→3.64 and 4.20→3.41 ms; on one thread it is ~5% slower, since the labelling and
the clean-up add work that only pays across cores — and `eeye` always uses them all.

Direction is quantized to four sectors using **integer comparisons only** — `|gy|*10`
against `|gx|*4` and `|gx|*24`, the integer form of tan(22.5°) and tan(67.5°). No
`atan2`, no float, not even a divide.

NMS uses an **asymmetric comparison** (`m >= a && m > b`), not `>=` on both sides. A
step edge produces a symmetric ridge where the two pixels straddling it have *equal*
magnitude, so `>=` on both keeps both and the contour comes out 2px wide — defeating
the stage entirely. This was caught by a test, not by eye.

### `contrast` — auto min/max luma stretch

No parameters. Scans for the actual min and max luma present and stretches to full
range, recomputed fresh every frame from whatever the chain has produced so far.

Touches only Y, so the chain copies the untouched U/V planes rather than bringing the
whole frame into `work` first.

### Auto visibility (`auto_vis.c`) — not a chain stage

Switched by the config's `auto_visibility`, not placed in `chain`: it is a mode, run
once ahead of the chain by `apply_effect_chain_auto`, reading `raw` and writing `work`
so the chain continues from its output (a neighborhood op after it reads the corrected
picture, not raw). It needs state across frames — the smoothed corrections — which no
stage has, so it lives in an `AutoVis` owned by the effects thread.

Steered by `img_stats.c`, which measures the raw frame every frame (every second pixel
of every second row: the luma histogram, and the mean chroma of well-exposed pixels)
whether or not auto visibility is on, since the operator gets the same numbers through
the health file.

Per frame: targets from the statistics (stretch percentiles, a gamma for the median,
the cast to remove beyond a deadband), smoothed toward with a time constant on
*capture* time, so the response is the same at 15fps as at 60; a gap of over a second
restarts from the current frame. Then: a read-only histogram of the *source* luma per
CLAHE tile (one tile per parallel iteration, four interleaved counters so flat water
does not serialise on one bin); each tile's mapping built from that histogram pushed
through the global LUT — the corrected picture's histogram, without writing the
corrected picture — clipped, smoothed, and then composed with the global LUT so one
table does both; one luma pass mapping each source pixel through its two nearest
tile-rows' tables, blended once per row so a pixel costs two lookups instead of four;
and a chroma pass that removes the cast in proportion to the luma under each chroma
sample and lifts saturation. All in one OpenMP parallel region. The output is
byte-identical to the straightforward version it replaced (same hash over every frame
of the synthetic sets, on x86 and under qemu for both ARMs), at 2.2× the speed on one
thread and 2.7× on four: 0.55ms per 720p frame on four x86 cores, down from 1.5ms.

Every tuning constant is at the top of `auto_vis.c` with the reason for its value; the
values are starting points from synthetic frames and a webcam, to be re-tuned on real
footage. `tests/test_auto_vis.c` checks behaviour, not pixels: cast removed, range and
edges restored, a clean scene's background left its colour, no flicker on a still
scene, smooth following, no fade-in, noise bounded (≤4.5×; 3.9× measured), and prints
the per-frame cost. `AUTO_VIS_DUMP=<dir>` writes its frames out for a look.

### `light` — brightness and saturation together

One dial over luma offset and chroma scale-around-128. Unlike `contrast` it is a fixed
function of the level rather than of frame content, which is what lets it fold into the
LUT pass.

**`light_level` defaults to 128, not 0** — the only parameter that does. Every other
optional field's zero-init already means "no change"; 0 here would mean "fully dark and
desaturated". Special-cased in `parse_effect_stage`.

## Measured costs

On a real 1280×720 camera frame, x86 dev laptop, against a 33.3 ms budget at 30fps.
These predate the fused blur, the chroma skip and NEON NMS, and none of those has been
timed on a Pi yet. The stats log now prints average and worst effects time per frame
every 5s (`effects: N frames, avg X ms, max Y ms`), which is the number to collect there.

| Operator | Cost |
|---|---|
| `sobel` | 0.36 ms |
| `log` (strength 1) | 1.80 ms |
| `canny` (strength 1) | 2.85 ms |

Canny per stage:

| Stage | ms | Share | SIMD |
|---|---|---|---|
| gaussian blur | 0.620 | 20.3% | NEON |
| gradient + direction | 0.951 | 31.1% | NEON |
| non-max suppression | 1.014 | 33.2% | NEON (now; timing predates it) |
| hysteresis | 0.435 | 14.2% | serial by design |
| chroma memset | 0.036 | 1.2% | — |

Use a **real frame** for any measurement here. Synthetic noise inflates hysteresis
several-fold and will mislead you.

## SIMD status

| Kernel | NEON |
|---|---|
| `sobel_edges` | yes |
| `gaussian_blur` | yes |
| `gs_contrast_normalize` (min/max scan) | yes |
| `log_edges` (Laplacian + zero-crossing) | yes |
| `canny_edges` (gradient + direction) | yes |
| `canny_edges` (NMS) | yes |
| `apply_lut_plane` | no, deliberately |
| YUYV unpack | yes |
| YUYV / yuv420 downscale | no (integer shifts, rarely used) |

Every NEON block in `conv.c` builds on **ARMv7 as well as AArch64** (`GS_NEON`): none of
its intrinsics is AArch64-only. But it is compiled in only when the compiler defines
`__ARM_NEON`, and the compilers 32-bit Pi OS and Debian armhf ship do not by default — a
plain `make` there ran everything scalar (~3.5–6× the instructions for sobel, blur and
LoG, counted under qemu). The `Makefile` now adds `-march=armv7-a -mfpu=neon-vfpv4
-mfloat-abi=hard` on 32-bit hard-float ARM when NEON is not already on (`NEON=0` to
opt out), the same flags as the release armv7 build; such a binary needs a Pi 2 v1.1 or
newer. `point_opps.c` keeps its separate ARMv7 branch for the min/max
reduction, which does need AArch64-only instructions.

**NMS** selects 2 of 8 neighbours by direction, which is data-dependent addressing. It is
branchless: all four candidate pairs come from the same 3×3 window, which is eight
overlapping row loads shared by every lane, and `vbslq_u8` picks each lane's pair by its
direction. It is bit-exact against the scalar loop, which stays as the tail.

**The NEON speedup itself is unmeasured.** Correctness is verified bit-exact; the
benefit needs a run on the Pi. qemu says nothing about real NEON throughput.

Every NEON kernel here has a fixed verification bar. See [testing.md](testing.md).

## Chroma handling in edge operators

`sobel`, `log` and `canny` all produce a luma-only edge map and `memset` both chroma
planes to 128 (neutral). Carrying the source's colour through would tint white contours
with hues that no longer correspond to anything in the output.

If you add an edge operator, follow that contract — there is a test for it in each of
their suites.
