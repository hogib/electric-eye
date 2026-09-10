# Extending

Recipes for the common changes, each listing every file it touches. If you are here to
modify one thing, start with the matching section.

## Add an effect

Worked example: the `canny` operator, which is the most recent and touched everything.

**1. Implement the kernel** — `src/conv.c` (neighborhood) or `src/point_opps.c` (point
op), declared in the matching header.

Follow the existing signature shape so the chain can dispatch uniformly:

```c
void my_effect(const uint8_t *const src_planes[3], uint8_t *const dst_planes[3],
               uint32_t width, uint32_t height, const size_t stride[3],
               uint8_t param_a, uint8_t param_b);
```

`src` and `dst` must not alias for neighborhood ops. If you write a luma-only edge map,
`memset` both chroma planes to 128 — see [effects.md](effects.md).

If you need scratch, follow the `realloc`-grown file-static pattern (`blur_scratch`,
`canny_mag`). Return without writing `dst` on allocation failure rather than crashing.
If you allocate tightly packed at `width * height`, **check `stride[0] == width`** and
bail otherwise, as `log_edges` and `canny_edges` do.

**2. Add the enum and parameters** — `src/config.h`:

```c
typedef enum { ..., EFFECT_MY_EFFECT, } EffectType;
```

and fields on `EffectStage`, documented with their range and neutral value.

**3. Wire the parser** — `src/config.c`, four places:

- `effect_to_string()` — used for error messages
- `effect_from_string()` — the name accepted in config
- the `PARAM_*` bitmask enum, `params_for_effect()`, `param_name()`
- the key-parsing chain in `parse_effect_stage`, setting `seen_params |= PARAM_...`

If a parameter's zero value would be wrong (as with `light_level` and the Canny
thresholds), seed a default near the top of `parse_effect_stage`.

**4. Dispatch it** — `src/effect_chain.c`. For a neighborhood op, add it to the
condition and the if-chain:

```c
if (stage->effect == EFFECT_BLUR || stage->effect == EFFECT_SOBEL ||
    stage->effect == EFFECT_LOG  || stage->effect == EFFECT_CANNY) {
```

For a point op, add it to `is_point_op()` **and** compose it into the LUTs in
`fuse_point_op_run()`. If it cannot be expressed as a per-byte function, it is not a
point op — give it its own branch like `contrast`.

**5. Expose it in the UI** — `topside/web_ui.py`, `EFFECT_FIELDS`. This table must
match `params_for_effect()` exactly, since the parser rejects a parameter on the wrong
effect. The JS sends only the keys listed for the selected effect.

**6. Tests** — `tests/test_<name>.c`, registered in `test_extra_sources`. Also extend
`tests/test_config.c`: the effect name should parse, and its parameters should be
rejected on other effects and vice versa.

**7. Document** — the parameter table in `README.md` and in
[config.md](config.md).

## Add a config key

**Hot-reloadable** (like `stream_quality`): add the field to `Config`, a default in
`config_defaults`, and a branch in `parse_config`. Readers pick it up automatically
through `config_current()`.

**Startup-only** (like `downscale`): the same, plus:

- add it to the change-detection warning in `config_publish()`, so a live change is
  logged rather than silently ignored
- thread it from `startup_config` in `main()` into whatever arg struct needs it
- **add it to `carriedGeometry` in `topside/web_ui.py`** — the UI rewrites the whole
  config file, so a key it does not echo back is deleted on the next Apply

Add tests for the default, a round trip, out-of-range rejection, and a typo'd variant.

## Add a capture backend

Worked example: `rpicam_in.c`.

1. **A header with the same lifecycle shape** as `v4l2_in.h`: open, capture, close, and
   an `available()` probe if `auto` should be able to find it.
2. **Add a `CaptureSource` value** in `config.h` and its parse branch in `config.c`.
3. **Extend `CaptureHandle`** in `video_threads.c` — one pointer per backend, exactly
   one non-NULL — and the three helpers (`capture_open`, `capture_frame`,
   `capture_close`).
4. **Decide about negotiation.** `v4l2_in_negotiate_size` is V4L2-specific; a backend
   without a mode list should be skipped in `main()`, as rpicam is.
5. **Decide about camera controls.** Live ioctls, or spawn-time arguments requiring a
   restart? See `capture_controls_are_live()`.

Reuse `jpeg_decode.c` if the source is MJPEG — it already handles 4:2:2, 4:2:0, DHT
injection, and scaled decode.

## Remove the JPEG round-trip (Pi camera)

The largest CPU item on that path: `rpicam-vid` software-encodes MJPEG and `eeye`
immediately decodes it — 1.74 ms/frame decode alone on x86, likely 5–9 ms on a Pi, plus
the encode.

**Option A — `rpicam-vid --codec yuv420`.** Take the ISP's output directly, no JPEG
either way. What has to be handled:

- **Stride padding**: Y rows padded to a multiple of 64 bytes, U/V to 32. The stream
  carries no framing and no geometry, so frame size must be computed from the padding
  rule. Getting this wrong is the documented cause of upstream "corrupt yuv420 output"
  reports.
- **4:2:0 → I422**: needs chroma line-doubling. **That code already exists** —
  `decode_420_to_i422` in `jpeg_decode.c`, already tested.
- **Frame boundaries** become "read exactly N bytes", simpler than the JPEG marker walk
  but no longer self-synchronising. A desync stays desynced, so sanity-check frame size.

**Option B — link libcamera directly.** Removes the subprocess entirely; drags a C++
dependency into a C project. Only worth it if A proves insufficient.

**Both are moot if you move to a USB camera with YUYV**, which removes the round-trip
completely — YUYV capture is a deinterleave with no decode at all. Note the consequence:
with no decode, the effect chain becomes the dominant cost, so SIMD work matters *more*
after that switch, not less.

## Vectorize a kernel

The remaining candidate is Canny's non-maximum suppression — 33% of Canny, still scalar.

It selects 2 of 8 neighbours **by direction**, which is data-dependent addressing.
Branchless approach: load all four candidate neighbour pairs, compute all four
comparisons, select with `vbslq_u8` against masks from the direction plane. That is 8
loads instead of 2 to avoid a branch that is already well-predicted on real images,
where direction runs long along an edge.

**Measure before keeping it.** It may lose. The fused gradient+NMS idea did.

Pattern to follow (`sobel_row16`, `laplacian4_half8`, `canny_gradient8`):

- Guard with `#if defined(GS_NEON_AARCH64)`, leaving the scalar loop as the tail
- Handle the left edge scalar-first, vectorize the interior, scalar tail for the
  remainder **and** the right edge
- Keep intermediate widths generous enough that nothing saturates before the deliberate
  clamp — that is what makes bit-exactness achievable rather than approximate
- Reproduce the scalar predicate exactly, including how it treats zero

Then verify to the bar in [testing.md](testing.md). Budget more time for verification
than for the kernel; a SIMD path that is subtly wrong only at certain widths is a nasty
bug to chase.

## Change the frame format

`VideoFrame` is I422 throughout, and that assumption is widespread: the decoder targets
it, the YUYV deinterleave assumes it, `v4l2_out` negotiates `V4L2_PIX_FMT_YUV422P`, and
the chroma-neutralizing `memset(128)` in every edge operator assumes half-width planes.

If you genuinely need another layout, expect to touch `video_frame.c`, `jpeg_decode.c`,
both capture backends, `v4l2_out.c`, every operator in `conv.c` and `point_opps.c`, and
the geometry validation in `config.c`. There is no abstraction layer to change instead.

## Things deliberately not done

Worth knowing before you propose them, along with why:

| Idea | Why not |
|---|---|
| Fuse Canny's gradient + NMS | Measured **slower**: 0.94 ms vs 0.68 ms, recomputing each gradient 3× |
| Parallel (bounded-sweep) hysteresis | 2× slower on realistic content, and only approximates connectivity |
| NEON for `apply_lut_plane` | A cache-friendly table lookup beats a NEON gather |
| MMAP for `v4l2_out` | v4l2loopback shares one buffer with `write()`; saves no copy |
| Build v4l2loopback from source at install | Loses DKMS auto-rebuild on kernel upgrade and signing; does not address the MOK failure mode at all |
| Prebuilt release binaries | The kernel module makes a toolchain mandatory anyway |
| Subdirectories / namespaces in `src/` | 16 files, ~7.8k lines; the prefix convention already does the job |
| C++ for `constexpr` | Already using C23 `constexpr`; nothing input-independent left to fold |

## Known gaps

- **Non-maximum suppression is scalar** — the one real SIMD candidate left.
- **NEON speedup is unmeasured on hardware.** Correctness verified; benefit is not.
- **The rpicam camera-control mapping is unverified on real CSI hardware.** Only its
  argument construction is tested.
- **Recordings carry no metadata** — an `.mp4` has no record of the chain, geometry, or
  camera settings that produced it. A sidecar `.json` would be cheap.
- **No saved config presets** — every dive means rebuilding the chain by hand.
- **Framerate and both device paths are compile-time constants.**
