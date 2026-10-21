# Testing

14 suites, ~190 tests, run with `meson test -C builddir`.

## The suites

| Suite | Covers |
|---|---|
| `config` | Parsing, validation, every key, the defaults |
| `ring_buffer` | FIFO order, capacity, wraparound, concurrent pushers |
| `jpeg_framing` | `find_jpeg_end` — frame boundaries in a pipe stream, resumed scans, rpicam yuv420 unpacking |
| `jpeg_decode` | 4:2:2 and 4:2:0 decode, line-doubling, downscale |
| `negotiation` | Resolution ranking, the divisibility filter, YUYV downscale |
| `log_edges` | LoG behavior |
| `canny` | Canny behavior |
| `effect_chain` | LUT fusion against an independent reference; chroma skipping, role swaps |
| `recorder` | Recording thread: file contents, ordering, a stalled disk (FIFO) never blocking |
| `stream_server` | Preview thread: complete JPEGs, a viewer that never reads gets dropped, offers never block |
| `camera_ctrl` | Range scaling, unit conversion, rpicam argv |
| `health` | Health file contents and escaping |
| `topside` | Python: link staleness, health relay, recorder |
| `record` | Python: duration parsing, ffmpeg command construction |

## The include-the-TU pattern

C tests `#include` the `.c` file they exercise rather than linking against it:

```c
#include "config.c"
#include "test_harness.h"
```

That reaches file-static functions — `parse_config`, `find_jpeg_end`, `consider_size`,
`scale_to_range` — without widening their visibility in the shipping build just to make
them testable.

Extra translation units a test needs are listed in `test_extra_sources` in
`meson.build`. If a test fails to link with undefined references, that map is where to
add them.

## The harness

`tests/test_harness.h`, ~70 lines of macros. No framework — the tests should need
nothing installed on a Pi before they can be trusted.

```c
CHECK(cond);
CHECK_EQ_INT(actual, expected);       // prints both values
CHECK_MEM_EQ(actual, expected, n);    // prints the first differing byte index
RUN_TEST(fn);
TEST_MAIN_END();                      // exit code from failure count
```

A failing check reports file and line and **keeps going**, so one run shows every
failure rather than only the first. `CHECK_MEM_EQ` reporting the first differing index
matters with image planes — "buffers differ" is not a diagnostic.

## Verifying SIMD: the bit-exactness bar

Every NEON kernel must be **byte-identical** to its scalar twin. Not "close" — identical.

The method builds two objects from the same source:

```sh
# 1. A scalar twin: same file, NEON detection defeated.
#    (insert `#undef GS_NEON_AARCH64` after the arch-detection block)

# 2. Two objects, the reference with its public symbols renamed so both
#    can live in one binary.
aarch64-linux-gnu-gcc -O2 -I src -c src/conv.c -o conv_neon.o
aarch64-linux-gnu-gcc -O2 -I src -c conv_scalar.c -o conv_scalar.o \
    -Dlog_edges=log_edges_ref -Dcanny_edges=canny_edges_ref \
    -Dsobel_edges=sobel_edges_ref -Dgaussian_blur=gaussian_blur_ref

# 3. Link both, run under emulation.
aarch64-linux-gnu-gcc -O2 -I src -o neon_verify neon_verify.c \
    conv_neon.o conv_scalar.o src/video_frame.c -lm
qemu-aarch64-static -L /usr/aarch64-linux-gnu ./neon_verify
```

Without a cross toolchain installed, `zig cc` works as one and bundles its own libc:
`zig cc -target aarch64-linux-musl -mcpu=cortex_a76` (AArch64) or
`zig cc -target arm-linux-musleabihf -mcpu=cortex_a53` (ARMv7 with NEON), run with plain
`qemu-aarch64` / `qemu-arm`. The kernels are also guarded for ARMv7 now, so verify both.

**Check the guard actually fires.** Before trusting a zero-mismatch run, plant a bug in
the NEON path and confirm the harness catches it, or check
`-dM -E src/conv.c | grep GS_NEON`. A guard that silently compiles the NEON path out
makes the comparison scalar against scalar, which passes every time.

The renaming trick is what makes this cheap: no separate reference implementation to
write and keep in sync, so the twin cannot drift from the thing it validates.

### What to vary

Coverage matters more than trial count. The last run was 1600 trials for `log_edges` and
900 for `canny_edges`, zero mismatches, across:

- **Ten frame sizes**, deliberately including **non-multiples of 16** (65×48, 79×33,
  31×9). This is the important one — a vectorized loop handles a remainder tail in
  scalar code, and that boundary is where SIMD bugs live.
- **Five content patterns**: random, flat, all-max, checkerboard, ramp. Degenerate
  inputs catch saturation and sign bugs that random content misses.
- **The parameter space**: strengths and thresholds, including 0 and the extremes.

Poison the destination (`memset(0xA5)`) before each call so an unwritten byte cannot
pass as a match.

### Bit-exactness is necessary, not sufficient

It proves the two agree, **not that either is right**. So also cross-compile the
project's own test suites and run them under emulation with NEON active:

```sh
aarch64-linux-gnu-gcc -std=gnu23 -O2 -I src -I tests -o t_canny \
    tests/test_canny.c src/conv.c src/video_frame.c -lm
qemu-aarch64-static -L /usr/aarch64-linux-gnu ./t_canny
```

Those assert documented properties — thin contours, no edges on a flat field, neutral
chroma — rather than agreement with a twin.

### What emulation cannot tell you

**Speed.** qemu says nothing about real NEON throughput. Correctness is establishable
here; the benefit is not, and needs a run on the Pi. Do not report a speedup you have
not measured on hardware.

## Testing habits worth keeping

**Check that a test can fail.** After writing a regression test, run it against the
broken version and confirm it fails. When the charset tests were added, four of five
failed against the pre-fix file — the fifth passed, because the bytes were always valid
UTF-8 and only the *declaration* was missing. Its docstring now says so rather than
implying coverage it does not have.

**Measure on real frames, not synthetic noise.** Canny costs 2.85 ms on a real 720p
frame and 10.46 ms on uniform random noise, because hysteresis is content-dependent.
Benchmarking on noise would send you optimizing the wrong thing.

**Make sure the compiler cannot delete the work.** A benchmark once reported a fused
kernel at 0.01 ms — dead-code elimination, because nothing consumed the output.
Checksum the result and print it. When both variants were forced to produce observable
output the conclusion inverted.

**Verify output, not exit codes.** The recorder's stop path returned `{"ok": true}`
while leaving an unplayable file. `ffprobe` the artifact.

**Prefer a real device where one exists.** The camera-control ranges were verified by
setting values through `eeye` and reading them back with `v4l2-ctl` — brightness 100 →
64 on a −64..64 device. Restore the camera to defaults afterwards.

## Adding a suite

1. Write `tests/test_<name>.c` using the harness.
2. Add `'<name>': [...]` to `test_extra_sources` in `meson.build`, listing any extra
   `.c` files it needs.
3. `meson setup --reconfigure builddir && meson test -C builddir`.

Python suites are registered separately near the bottom of `meson.build` and run via
`find_program('python3')`. They fall back to IPv4 loopback where IPv6 loopback is
unavailable, so they run in containers rather than being silently skipped.
