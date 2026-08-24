#include "conv.h"
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

// Same architecture detection as point_opps.c (duplicated rather than
// shared via a header, matching that file's existing convention).
#if defined(__aarch64__)
#include <arm_neon.h>
#define GS_NEON_AARCH64 1
#elif defined(__arm__) && defined(__ARM_NEON)
#include <arm_neon.h>
#define GS_NEON_ARM32 1
#endif

/*
 * Sobel gradient magnitude for one pixel, given its 3x3 neighborhood.
 * Intermediates are int32_t: Gx/Gy each range +-1020 (4 * 255), well past
 * what int8_t/uint8_t hold, and the sum of their magnitudes needs headroom
 * past that before the final clamp.
 */
// threshold: magnitudes below this are clamped to 0, applied after the
// 255 clamp -- so e.g. threshold=0 (the default; see EffectStage in
// config.h) keeps every magnitude exactly as computed.
static inline uint8_t sobel_magnitude(int32_t p00, int32_t p01, int32_t p02,
                                      int32_t p10, int32_t p12, int32_t p20,
                                      int32_t p21, int32_t p22,
                                      uint8_t threshold) {
  int32_t gx = (p02 + 2 * p12 + p22) - (p00 + 2 * p10 + p20);
  int32_t gy = (p20 + 2 * p21 + p22) - (p00 + 2 * p01 + p02);

  // |Gx| + |Gy| (L1) instead of sqrt(Gx^2 + Gy^2) (L2, the "true" gradient
  // magnitude): visually near-identical, no float, no sqrt -- this is what
  // realtime edge detectors use in practice.
  int32_t mag = (gx < 0 ? -gx : gx) + (gy < 0 ? -gy : gy);
  uint8_t clamped = (uint8_t)(mag > 255 ? 255 : mag);
  return clamped < threshold ? 0 : clamped;
}

#if defined(GS_NEON_AARCH64)
// 8-lane half of sobel_magnitude: same coefficients (1,2,1)/(1,2,1), same
// parameter order, operating on a pre-widened 16-bit half of a 16-lane
// chunk so gx/gy (range +-1020) can go negative without overflow.
//
// Verified bit-exact against sobel_magnitude() above by exhaustive test
// (500 trials -- all-zero, all-max, checkerboard, and random content, at a
// width not divisible by 16 so the scalar remainder tail below is also
// exercised) cross-compiled for aarch64 and run under qemu-user emulation;
// this file has not been built or run on real hardware.
static inline uint8x8_t sobel_half8(uint16x8_t p00, uint16x8_t p01,
                                    uint16x8_t p02, uint16x8_t p10,
                                    uint16x8_t p12, uint16x8_t p20,
                                    uint16x8_t p21, uint16x8_t p22) {
  uint16x8_t sum_gx_pos = vaddq_u16(p02, p22);
  sum_gx_pos = vmlaq_n_u16(sum_gx_pos, p12, 2); // p02 + 2*p12 + p22
  uint16x8_t sum_gx_neg = vaddq_u16(p00, p20);
  sum_gx_neg = vmlaq_n_u16(sum_gx_neg, p10, 2); // p00 + 2*p10 + p20
  // Both sums are non-negative (max 4*255=1020, well inside uint16 range),
  // so reinterpreting as signed before subtracting changes no values --
  // it's what lets the subtraction go negative correctly.
  int16x8_t gx = vsubq_s16(vreinterpretq_s16_u16(sum_gx_pos),
                           vreinterpretq_s16_u16(sum_gx_neg));

  uint16x8_t sum_gy_pos = vaddq_u16(p20, p22);
  sum_gy_pos = vmlaq_n_u16(sum_gy_pos, p21, 2); // p20 + 2*p21 + p22
  uint16x8_t sum_gy_neg = vaddq_u16(p00, p02);
  sum_gy_neg = vmlaq_n_u16(sum_gy_neg, p01, 2); // p00 + 2*p01 + p02
  int16x8_t gy = vsubq_s16(vreinterpretq_s16_u16(sum_gy_pos),
                           vreinterpretq_s16_u16(sum_gy_neg));

  // mag = |gx| + |gy|, always >= 0 and <= 2040 -- vqmovun_s16 saturates
  // anything past 255 down to it in the same instruction as the narrow,
  // which is exactly sobel_magnitude's `mag > 255 ? 255 : mag`.
  int16x8_t mag = vaddq_s16(vabsq_s16(gx), vabsq_s16(gy));
  return vqmovun_s16(mag);
}

// 16-lane Sobel: widens each of the 8 neighbor vectors into low/high
// 8-lane halves, runs sobel_half8 on each half (unchanged, unthresholded --
// keeps that already-verified kernel untouched), recombines, then applies
// threshold as a post-step: mask = (mag >= threshold) ? 0xFF : 0x00,
// result = mag & mask. For any mag/threshold in [0,255] that's exactly
// `mag < threshold ? 0 : mag` -- mag & 0xFF is mag itself, mag & 0 is 0 --
// matching sobel_magnitude()'s scalar formula bit-for-bit. Parameter
// order/names match sobel_magnitude exactly for easy side-by-side
// comparison.
static inline uint8x16_t sobel_row16(uint8x16_t p00, uint8x16_t p01,
                                     uint8x16_t p02, uint8x16_t p10,
                                     uint8x16_t p12, uint8x16_t p20,
                                     uint8x16_t p21, uint8x16_t p22,
                                     uint8x16_t threshold_vec) {
  uint8x8_t lo = sobel_half8(
      vmovl_u8(vget_low_u8(p00)), vmovl_u8(vget_low_u8(p01)),
      vmovl_u8(vget_low_u8(p02)), vmovl_u8(vget_low_u8(p10)),
      vmovl_u8(vget_low_u8(p12)), vmovl_u8(vget_low_u8(p20)),
      vmovl_u8(vget_low_u8(p21)), vmovl_u8(vget_low_u8(p22)));
  uint8x8_t hi = sobel_half8(
      vmovl_u8(vget_high_u8(p00)), vmovl_u8(vget_high_u8(p01)),
      vmovl_u8(vget_high_u8(p02)), vmovl_u8(vget_high_u8(p10)),
      vmovl_u8(vget_high_u8(p12)), vmovl_u8(vget_high_u8(p20)),
      vmovl_u8(vget_high_u8(p21)), vmovl_u8(vget_high_u8(p22)));
  uint8x16_t mag = vcombine_u8(lo, hi);
  uint8x16_t mask = vcgeq_u8(mag, threshold_vec);
  return vandq_u8(mag, mask);
}
#endif // GS_NEON_AARCH64

// Reads one pixel, clamping out-of-bounds coordinates to the nearest valid
// row/column (clamp-to-edge / replicate padding). Used only at the 1px
// frame border, where a full 3x3 neighborhood runs off the edge.
static inline uint8_t clamped_sample(const uint8_t *y_plane, size_t stride,
                                     uint32_t width, uint32_t height,
                                     int32_t x, int32_t y) {
  if (x < 0)
    x = 0;
  else if (x >= (int32_t)width)
    x = (int32_t)width - 1;

  if (y < 0)
    y = 0;
  else if (y >= (int32_t)height)
    y = (int32_t)height - 1;

  return y_plane[(size_t)y * stride + (size_t)x];
}

// Sobel at (x, y) via clamped sampling. Only for the border: it re-derives
// all 8 neighbor coordinates and re-checks bounds on every call, which is
// wasted work in the interior where bounds are already known to be safe.
static inline uint8_t sobel_at_clamped(const uint8_t *y_plane, size_t stride,
                                       uint32_t width, uint32_t height,
                                       int32_t x, int32_t y,
                                       uint8_t threshold) {
  return sobel_magnitude(
      clamped_sample(y_plane, stride, width, height, x - 1, y - 1),
      clamped_sample(y_plane, stride, width, height, x, y - 1),
      clamped_sample(y_plane, stride, width, height, x + 1, y - 1),
      clamped_sample(y_plane, stride, width, height, x - 1, y),
      clamped_sample(y_plane, stride, width, height, x + 1, y),
      clamped_sample(y_plane, stride, width, height, x - 1, y + 1),
      clamped_sample(y_plane, stride, width, height, x, y + 1),
      clamped_sample(y_plane, stride, width, height, x + 1, y + 1), threshold);
}

void sobel_edges(const uint8_t *const src_planes[3], uint8_t *const dst_planes[3],
                 uint32_t width, uint32_t height, const size_t stride_arr[3],
                 uint8_t threshold) {
  if (!src_planes[0] || !dst_planes[0])
    return;

  const uint8_t *raw_y = src_planes[0];
  uint8_t *out_y = dst_planes[0];
  size_t stride = stride_arr[0];
#if defined(GS_NEON_AARCH64)
  uint8x16_t threshold_vec = vdupq_n_u8(threshold);
#endif

  // Interior: every pixel here has a full 3x3 neighborhood, so read directly
  // through pointers instead of through clamped_sample's bounds checks. This
  // is the hot loop -- every pixel except a 1px frame border.
  //
  // raw_y is never written by this function (or anything else once the
  // producer has filled it), so overlapping reads of the same row by
  // adjacent thread chunks -- row y+1 is both "row_below" for chunk N and
  // "row_above" for chunk N+1 -- are safe with no synchronization. That is
  // the property the raw/work split exists to buy.
  // OpenMP requires the loop directly under #pragma omp parallel for to be
  // in canonical form: `var < loop-invariant`. `y + 1 < height`, though
  // equivalent, puts the loop variable inside an expression on the
  // left-hand side and is rejected at compile time once OpenMP is actually
  // parsing it (as opposed to silently ignoring the pragma, which is what
  // let this slip through before OpenMP was wired into the build).
#pragma omp parallel for
  for (uint32_t y = 1; y < height - 1; ++y) {
    const uint8_t *row_above = raw_y + (size_t)(y - 1) * stride;
    const uint8_t *row = raw_y + (size_t)y * stride;
    const uint8_t *row_below = raw_y + (size_t)(y + 1) * stride;
    uint8_t *out_row = out_y + (size_t)y * stride;

#if defined(GS_NEON_AARCH64)
    // A chunk starting at x covers columns [x, x+15]; the rightmost lane
    // also needs its +1 neighbor, so the last byte actually read is at
    // x+16 -- the loop bound reflects that, one further in than a plain
    // per-pixel NEON loop (e.g. color_tint's) would need.
    uint32_t x = 1;
    for (; x + 16 < width; x += 16) {
      uint8x16_t result = sobel_row16(
          vld1q_u8(&row_above[x - 1]), vld1q_u8(&row_above[x]),
          vld1q_u8(&row_above[x + 1]), vld1q_u8(&row[x - 1]),
          vld1q_u8(&row[x + 1]), vld1q_u8(&row_below[x - 1]),
          vld1q_u8(&row_below[x]), vld1q_u8(&row_below[x + 1]), threshold_vec);
      vst1q_u8(&out_row[x], result);
    }
    for (; x + 1 < width; ++x) {
      out_row[x] = sobel_magnitude(row_above[x - 1], row_above[x],
                                   row_above[x + 1], row[x - 1], row[x + 1],
                                   row_below[x - 1], row_below[x],
                                   row_below[x + 1], threshold);
    }
#else
    for (uint32_t x = 1; x + 1 < width; ++x) {
      out_row[x] = sobel_magnitude(row_above[x - 1], row_above[x],
                                   row_above[x + 1], row[x - 1], row[x + 1],
                                   row_below[x - 1], row_below[x],
                                   row_below[x + 1], threshold);
    }
#endif
  }

  // Border: no full neighborhood exists past the frame edge. Replicate
  // rather than zero-pad -- zero-padding a bright edge would read as a false
  // hard line running along the frame boundary that has nothing to do with
  // the actual scene.
  for (uint32_t x = 0; x < width; ++x) {
    out_y[x] =
        sobel_at_clamped(raw_y, stride, width, height, (int32_t)x, 0, threshold);
    out_y[(size_t)(height - 1) * stride + x] =
        sobel_at_clamped(raw_y, stride, width, height, (int32_t)x,
                         (int32_t)height - 1, threshold);
  }
  for (uint32_t y = 1; y + 1 < height; ++y) {
    out_y[(size_t)y * stride] = sobel_at_clamped(raw_y, stride, width, height,
                                                 0, (int32_t)y, threshold);
    out_y[(size_t)y * stride + (width - 1)] =
        sobel_at_clamped(raw_y, stride, width, height, (int32_t)width - 1,
                         (int32_t)y, threshold);
  }

  // plane_sizes[1]/[2] aren't available without a VideoFrame -- both equal
  // stride*height, exactly how vf_create derives them in the first place.
  memset(dst_planes[1], 128, stride_arr[1] * height);
  memset(dst_planes[2], 128, stride_arr[2] * height);
}

// 5-tap binomial approximation to a Gaussian ([1,4,6,4,1]/16): separable
// (apply horizontally then vertically, 2*5=10 taps/pixel instead of a 2D
// kernel's 25), and integer-exact -- the weights sum to a power of two, so
// normalizing is a plain rounded right-shift, no float, no accumulated
// rounding error. The same kind of stand-in for a true Gaussian that
// sobel_magnitude's L1 norm is for a true gradient magnitude: visually
// indistinguishable, far cheaper.
static inline uint8_t blur5(int32_t p0, int32_t p1, int32_t p2, int32_t p3,
                           int32_t p4) {
  return (uint8_t)((p0 + 4 * p1 + 6 * p2 + 4 * p3 + p4 + 8) >> 4);
}

#if defined(GS_NEON_AARCH64)
// 8-lane half of blur5: same weights (1,4,6,4,1)/16, same parameter order,
// on a pre-widened 16-bit half so the running sum (max 255*16=4080) can't
// overflow. The +8 before the shift is the same round-to-nearest bias
// blur5 adds, done here so vshrn_n_u16's plain truncating narrow-shift
// matches blur5's `(... + 8) >> 4` exactly -- no separate rounding
// instruction needed.
//
// Verified bit-exact against blur5() by exhaustive test (500 trials --
// all-zero, all-max, checkerboard, and random content, at a width not
// divisible by 16 so the scalar remainder tail is also exercised)
// cross-compiled for aarch64 and run under qemu-user emulation; this file
// has not been built or run on real hardware.
static inline uint8x8_t blur5_half8(uint16x8_t p0, uint16x8_t p1,
                                    uint16x8_t p2, uint16x8_t p3,
                                    uint16x8_t p4) {
  uint16x8_t sum = vaddq_u16(p0, p4);
  sum = vmlaq_n_u16(sum, p1, 4);
  sum = vmlaq_n_u16(sum, p3, 4);
  sum = vmlaq_n_u16(sum, p2, 6);
  sum = vaddq_u16(sum, vdupq_n_u16(8));
  // Every lane's sum here is <= 4088 (255*16 + 8), so sum >> 4 <= 255.5
  // truncated to 255 -- always fits the low 8 bits vshrn_n_u16 keeps, same
  // as blur5's plain uint8_t cast never needing to saturate.
  return vshrn_n_u16(sum, 4);
}

// 16-lane blur5: widens each of the 5 neighbor vectors into low/high 8-lane
// halves, runs blur5_half8 on each half, recombines. Parameter order/names
// match blur5 exactly for easy side-by-side comparison.
static inline uint8x16_t blur5_row16(uint8x16_t p0, uint8x16_t p1,
                                     uint8x16_t p2, uint8x16_t p3,
                                     uint8x16_t p4) {
  uint8x8_t lo = blur5_half8(vmovl_u8(vget_low_u8(p0)), vmovl_u8(vget_low_u8(p1)),
                             vmovl_u8(vget_low_u8(p2)), vmovl_u8(vget_low_u8(p3)),
                             vmovl_u8(vget_low_u8(p4)));
  uint8x8_t hi = blur5_half8(vmovl_u8(vget_high_u8(p0)), vmovl_u8(vget_high_u8(p1)),
                             vmovl_u8(vget_high_u8(p2)), vmovl_u8(vget_high_u8(p3)),
                             vmovl_u8(vget_high_u8(p4)));
  return vcombine_u8(lo, hi);
}
#endif // GS_NEON_AARCH64

// Scratch for blur_plane's horizontal pass, grown lazily to the largest
// plane a caller has asked for so far and reused across the Y/U/V calls
// within one gaussian_blur() invocation (sequential, never concurrent, so
// one shared buffer is safe) and across frames (never freed until process
// exit -- like v4l2_in.c's dht_scratch, this trades a small, bounded,
// one-time cost for not touching the allocator on the hot path).
static uint8_t *blur_scratch;
static size_t blur_scratch_cap;

// One plane, two passes. Needs a private intermediate for the horizontal
// pass's output rather than writing straight into dst: the vertical pass
// reads a column spanning 5 rows of that output, so those rows can't
// already be sitting in a buffer being overwritten as they're read -- the
// same reason Sobel needs raw and work to be separate buffers, just
// recurring a second time within this one function's two internal passes.
//
// Border (2px on every edge -- a 5-tap kernel spans x-2..x+2): clamp-to-
// edge, matching Sobel's replicate-rather-than-zero-pad reasoning --
// zero-padding would read as a false soft vignette at the frame boundary
// that has nothing to do with the actual scene.
static void blur_plane(const uint8_t *src, uint8_t *dst, uint32_t width,
                      uint32_t height, size_t stride) {
  size_t needed = (size_t)width * height;
  if (blur_scratch_cap < needed) {
    uint8_t *grown = realloc(blur_scratch, needed);
    if (!grown)
      return; // Leave dst untouched rather than crash; gaussian_blur's
             // other plane(s) still produced valid output.
    blur_scratch = grown;
    blur_scratch_cap = needed;
  }

  // Horizontal: src (stride-spaced rows) -> blur_scratch (tightly packed,
  // width-spaced -- no stride padding to carry, since this buffer never
  // exists outside this one function call).
#pragma omp parallel for
  for (uint32_t y = 0; y < height; ++y) {
    const uint8_t *row = src + (size_t)y * stride;
    uint8_t *out_row = blur_scratch + (size_t)y * width;

#if defined(GS_NEON_AARCH64)
    uint32_t x = 0;
    // x < 2 always needs the left-edge clamp, so it's left to the same
    // clamped formula as the tail loop below rather than folded into the
    // NEON chunk.
    for (; x < 2 && x < width; ++x) {
      int32_t xm2 = (int32_t)x < 2 ? 0 : (int32_t)x - 2;
      int32_t xm1 = (int32_t)x < 1 ? 0 : (int32_t)x - 1;
      int32_t xp1 =
          (int32_t)x + 1 >= (int32_t)width ? (int32_t)width - 1 : (int32_t)x + 1;
      int32_t xp2 =
          (int32_t)x + 2 >= (int32_t)width ? (int32_t)width - 1 : (int32_t)x + 2;
      out_row[x] = blur5(row[xm2], row[xm1], row[x], row[xp1], row[xp2]);
    }
    // A chunk starting at x covers columns [x, x+15]; the rightmost lane
    // also needs its +2 neighbor, so the last byte actually read is at
    // x+17 -- the loop bound reflects that (one further in than Sobel's
    // equivalent chunk, which only needs a +1 neighbor).
    for (; x + 17 < width; x += 16) {
      uint8x16_t result =
          blur5_row16(vld1q_u8(&row[x - 2]), vld1q_u8(&row[x - 1]),
                     vld1q_u8(&row[x]), vld1q_u8(&row[x + 1]),
                     vld1q_u8(&row[x + 2]));
      vst1q_u8(&out_row[x], result);
    }
    // Tail: whatever the NEON loop above didn't cover (a non-multiple-of-16
    // remainder, and/or the true right edge needing its own clamp).
    for (; x < width; ++x) {
      int32_t xm2 = (int32_t)x < 2 ? 0 : (int32_t)x - 2;
      int32_t xm1 = (int32_t)x < 1 ? 0 : (int32_t)x - 1;
      int32_t xp1 =
          (int32_t)x + 1 >= (int32_t)width ? (int32_t)width - 1 : (int32_t)x + 1;
      int32_t xp2 =
          (int32_t)x + 2 >= (int32_t)width ? (int32_t)width - 1 : (int32_t)x + 2;
      out_row[x] = blur5(row[xm2], row[xm1], row[x], row[xp1], row[xp2]);
    }
#else
    for (uint32_t x = 0; x < width; ++x) {
      int32_t xm2 = (int32_t)x < 2 ? 0 : (int32_t)x - 2;
      int32_t xm1 = (int32_t)x < 1 ? 0 : (int32_t)x - 1;
      int32_t xp1 =
          (int32_t)x + 1 >= (int32_t)width ? (int32_t)width - 1 : (int32_t)x + 1;
      int32_t xp2 =
          (int32_t)x + 2 >= (int32_t)width ? (int32_t)width - 1 : (int32_t)x + 2;
      out_row[x] = blur5(row[xm2], row[xm1], row[x], row[xp1], row[xp2]);
    }
#endif
  }

  // Vertical: blur_scratch -> dst.
#pragma omp parallel for
  for (uint32_t y = 0; y < height; ++y) {
    int32_t ym2 = (int32_t)y < 2 ? 0 : (int32_t)y - 2;
    int32_t ym1 = (int32_t)y < 1 ? 0 : (int32_t)y - 1;
    int32_t yp1 =
        (int32_t)y + 1 >= (int32_t)height ? (int32_t)height - 1 : (int32_t)y + 1;
    int32_t yp2 =
        (int32_t)y + 2 >= (int32_t)height ? (int32_t)height - 1 : (int32_t)y + 2;

    const uint8_t *r_m2 = blur_scratch + (size_t)ym2 * width;
    const uint8_t *r_m1 = blur_scratch + (size_t)ym1 * width;
    const uint8_t *r_0 = blur_scratch + (size_t)y * width;
    const uint8_t *r_p1 = blur_scratch + (size_t)yp1 * width;
    const uint8_t *r_p2 = blur_scratch + (size_t)yp2 * width;
    uint8_t *out_row = dst + (size_t)y * stride;

    // No per-x clamping needed here, unlike the horizontal pass: y was
    // already clamped once above (r_m2..r_p2 are five valid, in-bounds
    // rows), and every column in a row is equally safe to read, so the
    // NEON chunk can cover the whole width down to a plain remainder tail.
#if defined(GS_NEON_AARCH64)
    uint32_t x = 0;
    for (; x + 16 <= width; x += 16) {
      uint8x16_t result =
          blur5_row16(vld1q_u8(&r_m2[x]), vld1q_u8(&r_m1[x]), vld1q_u8(&r_0[x]),
                     vld1q_u8(&r_p1[x]), vld1q_u8(&r_p2[x]));
      vst1q_u8(&out_row[x], result);
    }
    for (; x < width; ++x) {
      out_row[x] = blur5(r_m2[x], r_m1[x], r_0[x], r_p1[x], r_p2[x]);
    }
#else
    for (uint32_t x = 0; x < width; ++x) {
      out_row[x] = blur5(r_m2[x], r_m1[x], r_0[x], r_p1[x], r_p2[x]);
    }
#endif
  }
}

// Unlike sobel_edges (luma only, chroma reset to neutral -- an edge map
// has no meaningful color), blur touches all three planes: it's meant to
// work as a standalone soft-focus effect on its own, not only as Sobel
// preprocessing, and blurring luma while leaving chroma sharp would look
// visually inconsistent (fine detail in color, none in brightness).
// One plane, `passes` repetitions of blur_plane(). The first pass reads
// src (untouched by this function) into dst; every pass after that reads
// dst and writes dst again -- safe aliasing, since blur_plane() always
// fully drains its source into blur_scratch before writing its
// destination at all (see blur_plane's own comment).
static void blur_plane_repeated(const uint8_t *src, uint8_t *dst, uint32_t width,
                               uint32_t height, size_t stride, uint32_t passes) {
  blur_plane(src, dst, width, height, stride);
  for (uint32_t p = 1; p < passes; ++p) {
    blur_plane(dst, dst, width, height, stride);
  }
}

// --- Half-resolution fast path for higher blur_strength values --------
//
// Frame dimensions here are always the compile-time constants in eeye.c
// (1280x720 luma, 640x720 chroma for 4:2:2 -- see vf_create), which are
// exactly divisible by 2 at every level this code is ever called with, so
// every split below is a clean half with no remainder row/column to
// special-case.
//
// blur_plane_repeated's cost is linear in `passes`; downsampling first
// cuts every one of those repeated passes to a quarter of its pixels (half
// width * half height) at the fixed one-time cost of one 2x2 box-average
// down and one 2x bilinear up. That fixed cost isn't worth paying for a
// single pass -- see halfres_blur_pass_threshold below -- but it pays for
// itself increasingly well as `passes` climbs, which is exactly the
// situation that makes blur the dominant cost of a frame in the first
// place.

// 2x2 box average: touches every input pixel exactly once. No benefit to
// splitting this into two 2:1 passes the way the blur itself is separable
// -- every output sample already needs exactly its own 4 input samples, so
// an extra buffer round-trip would only add cost, not remove any.
static void downsample_box2x2(const uint8_t *src, size_t src_stride,
                              uint32_t half_w, uint32_t half_h, uint8_t *dst) {
#pragma omp parallel for
  for (uint32_t y = 0; y < half_h; ++y) {
    const uint8_t *row0 = src + (size_t)(2 * y) * src_stride;
    const uint8_t *row1 = src + (size_t)(2 * y + 1) * src_stride;
    uint8_t *out_row = dst + (size_t)y * half_w;
    for (uint32_t x = 0; x < half_w; ++x) {
      uint32_t x0 = 2 * x, x1 = x0 + 1;
      out_row[x] =
          (uint8_t)((row0[x0] + row0[x1] + row1[x0] + row1[x1] + 2) >> 2);
    }
  }
}

// One row of the 2x horizontal bilinear upsample below -- standalone so
// the vertical pass (upsample_bilinear2x) can apply the exact same 3-1/1-3
// weighting to rows instead of columns.
//
// Standard half-pixel-center bilinear upsample for an exact 2x scale
// factor: output sample 2k sits 1/4 of the way from in[k] toward in[k-1];
// output sample 2k+1 sits 1/4 of the way from in[k] toward in[k+1]. A
// fixed 2x scale means those weights (3/4, 1/4) never change, so they're
// baked in as integers rather than computed per pixel -- no runtime
// interpolation math, same rounding convention as blur5's "+bias >> shift".
static void upsample2x_row(const uint8_t *in, uint32_t half_w, uint8_t *out) {
  for (uint32_t k = 0; k < half_w; ++k) {
    uint32_t km1 = k == 0 ? 0 : k - 1;
    uint32_t kp1 = k + 1 >= half_w ? half_w - 1 : k + 1;
    out[2 * k] = (uint8_t)((1 * in[km1] + 3 * in[k] + 2) >> 2);
    out[2 * k + 1] = (uint8_t)((3 * in[k] + 1 * in[kp1] + 2) >> 2);
  }
}

// Mirrors blur_plane's own horizontal-then-vertical structure: expand
// columns first (half_w -> width, one row at a time) into blur_scratch,
// then expand rows (half_h -> height) out of that into dst. Safe to reuse
// blur_scratch here even though blur_plane also owns it -- by the time
// upsampling starts, whatever repeated blur passes used it have already
// completed and returned, so it's free for this second, unrelated purpose.
static void upsample_bilinear2x(const uint8_t *small, uint32_t half_w,
                                uint32_t half_h, uint8_t *dst, uint32_t width,
                                uint32_t height, size_t dst_stride) {
  size_t needed = (size_t)width * half_h;
  if (blur_scratch_cap < needed) {
    uint8_t *grown = realloc(blur_scratch, needed);
    if (!grown)
      return; // Leave dst untouched, matching blur_plane's own OOM handling.
    blur_scratch = grown;
    blur_scratch_cap = needed;
  }

#pragma omp parallel for
  for (uint32_t y = 0; y < half_h; ++y) {
    upsample2x_row(small + (size_t)y * half_w, half_w,
                   blur_scratch + (size_t)y * width);
  }

#pragma omp parallel for
  for (uint32_t y = 0; y < height; ++y) {
    uint32_t k = y / 2;
    bool lean_down = (y % 2) == 0; // even output row leans toward row k-1
    uint32_t other =
        lean_down ? (k == 0 ? 0 : k - 1) : (k + 1 >= half_h ? half_h - 1 : k + 1);
    const uint8_t *r_k = blur_scratch + (size_t)k * width;
    const uint8_t *r_other = blur_scratch + (size_t)other * width;
    uint8_t *out_row = dst + (size_t)y * dst_stride;
    for (uint32_t x = 0; x < width; ++x) {
      out_row[x] = (uint8_t)((3 * r_k[x] + r_other[x] + 2) >> 2);
    }
  }
}

// Half-resolution scratch for blur_plane_repeated_auto's downsample step --
// same lazy-grow/never-shrink lifetime as blur_scratch, just a distinct
// buffer since it needs to stay alive across the downsample, the repeated
// half-res blur passes, and the upsample, whereas blur_scratch itself gets
// reused partway through by both the repeated passes and the upsample.
static uint8_t *halfres_scratch;
static size_t halfres_scratch_cap;

// Below this many repeat passes, the fixed downsample/upsample cost isn't
// worth paying: it eats most of the savings right when a single extra
// full-res pass is already cheap, and a low pass count is the "just soften
// this a little" case most likely to actually be scrutinized. At and above
// it, the linear-in-passes savings from blurring a quarter as many pixels
// per pass clearly wins, and the softening those two extra resize steps
// add is comparatively unnoticeable on top of an already-heavy blur.
constexpr uint32_t halfres_blur_pass_threshold = 4;

static void blur_plane_repeated_auto(const uint8_t *src, uint8_t *dst,
                                     uint32_t width, uint32_t height,
                                     size_t stride, uint32_t passes) {
  if (passes < halfres_blur_pass_threshold) {
    blur_plane_repeated(src, dst, width, height, stride, passes);
    return;
  }

  uint32_t half_w = width / 2, half_h = height / 2;
  size_t needed = (size_t)half_w * half_h;
  if (halfres_scratch_cap < needed) {
    uint8_t *grown = realloc(halfres_scratch, needed);
    if (!grown) { // Fall back to the always-correct full-res path rather
                  // than leaving dst stale.
      blur_plane_repeated(src, dst, width, height, stride, passes);
      return;
    }
    halfres_scratch = grown;
    halfres_scratch_cap = needed;
  }

  downsample_box2x2(src, stride, half_w, half_h, halfres_scratch);
  // halfres_scratch is tightly packed, so its own stride is half_w. Using
  // it as both source and destination here is the same aliasing
  // blur_plane_repeated's pass 2+ already relies on (see blur_plane's own
  // comment): the horizontal pass fully drains its source into blur_scratch
  // before the vertical pass writes anything, with a hard OpenMP barrier
  // between the two, so it's just as safe on pass 1 when src and dst happen
  // to be the same buffer.
  blur_plane_repeated(halfres_scratch, halfres_scratch, half_w, half_h,
                      half_w, passes);
  upsample_bilinear2x(halfres_scratch, half_w, half_h, dst, width, height,
                      stride);
}

void gaussian_blur(const uint8_t *const src_planes[3], uint8_t *const dst_planes[3],
                   uint32_t width, uint32_t height, const size_t stride[3],
                   uint8_t strength) {
  if (!src_planes[0] || !dst_planes[0])
    return;

  // 0 and 1 both mean "just the one pass" -- see conv.h.
  uint32_t passes = strength == 0 ? 1 : strength;

  blur_plane_repeated_auto(src_planes[0], dst_planes[0], width, height,
                           stride[0], passes);

  // stride[1]/[2] are exactly the chroma width with no padding (see
  // vf_create), matching how color_tint already uses them as its own loop
  // bound.
  uint32_t chroma_width = (uint32_t)stride[1];
  blur_plane_repeated_auto(src_planes[1], dst_planes[1], chroma_width, height,
                           stride[1], passes);
  blur_plane_repeated_auto(src_planes[2], dst_planes[2], chroma_width, height,
                           stride[2], passes);
}

// --- Laplacian of Gaussian (Marr-Hildreth zero-crossing edges) --------
//
// Two stages, in the order the name implies: smooth with a Gaussian, then
// take the Laplacian (the 2nd derivative). Edges are where that response
// crosses zero, which is what gives this its characteristic output --
// thin, closed contours, rather than Sobel's thicker gradient ridges.
//
// Sigma comes from repeating the existing 5-tap blur rather than from a
// wider one-shot LoG kernel. Repeated blur passes compose into a wider
// effective Gaussian (sigma grows as sqrt(passes)), so this reuses a
// kernel that is already NEON-accelerated and verified bit-exact against
// its scalar form, instead of introducing a second, separately-tuned
// smoothing path that would need all of that again.
//
// Why not a single fixed 5x5 LoG kernel: sigma would then be hardcoded,
// and how much fine detail survives is exactly the knob worth having --
// underwater, backscatter and suspended particulate are high-frequency
// noise that a wider sigma rejects.

// The Laplacian is signed and, unlike Sobel's magnitude, its *sign* is the
// entire point -- a zero-crossing is where neighbouring responses differ
// in sign. So this keeps int16_t rather than clamping into uint8_t.
//
// Range: the 3x3 kernel below has a center weight of -4 and four +1
// neighbours, so with 0..255 inputs the response spans -1020..1020, which
// int16_t holds comfortably.
static int16_t *log_response;
static size_t log_response_cap;
static uint8_t *log_blurred;
static size_t log_blurred_cap;

// 4-neighbour Laplacian ([[0,1,0],[1,-4,1],[0,1,0]]) rather than the
// 8-neighbour variant: after Gaussian smoothing the two are visually
// near-identical, and this one needs 4 loads per pixel instead of 8.
static inline int16_t laplacian4(int32_t up, int32_t left, int32_t center,
                                 int32_t right, int32_t down) {
  return (int16_t)(up + left + right + down - 4 * center);
}

// A zero-crossing between two responses, with a slope test. Comparing
// signs alone marks a crossing wherever the response merely grazes zero,
// which in a smooth region is just sensor noise -- so the magnitude of the
// jump across the crossing has to clear `threshold` too. That difference
// is the local gradient steepness, which is precisely what distinguishes a
// real edge from noise wobbling around zero.
static inline bool crosses_zero(int16_t a, int16_t b, int32_t threshold) {
  if ((a < 0 && b < 0) || (a > 0 && b > 0))
    return false;
  if (a == 0 && b == 0)
    return false; // a flat zero region is not an edge
  int32_t jump = (int32_t)a - (int32_t)b;
  if (jump < 0)
    jump = -jump;
  return jump >= threshold;
}

void log_edges(const uint8_t *const src_planes[3], uint8_t *const dst_planes[3],
               uint32_t width, uint32_t height, const size_t stride_arr[3],
               uint8_t strength, uint8_t threshold) {
  if (!src_planes[0] || !dst_planes[0])
    return;

  const size_t stride = stride_arr[0];
  const size_t pixels = (size_t)width * height;

  if (log_blurred_cap < pixels) {
    uint8_t *grown = realloc(log_blurred, pixels);
    if (!grown)
      return; // leave dst untouched rather than crash, as blur_plane does
    log_blurred = grown;
    log_blurred_cap = pixels;
  }
  if (log_response_cap < pixels) {
    int16_t *grown = realloc(log_response, pixels * sizeof *log_response);
    if (!grown)
      return;
    log_response = grown;
    log_response_cap = pixels;
  }

  // Stage 1: Gaussian. 0 and 1 both mean a single pass, matching
  // blur_strength's documented behaviour so the two knobs read the same
  // way. The blurred plane is tightly packed (width-spaced, no stride
  // padding) since it never leaves this function.
  const uint32_t passes = strength == 0 ? 1 : strength;
  blur_plane_repeated(src_planes[0], log_blurred, width, height, stride,
                      passes);

  // blur_plane_repeated writes through `stride`, but log_blurred is
  // allocated tightly packed at width*height. Those agree only when
  // stride == width, which is how vf_create lays out the luma plane -- but
  // relying on that silently would corrupt memory the day it stops being
  // true, so require it explicitly and bail otherwise.
  if (stride != (size_t)width)
    return;

  // Stage 2: Laplacian over the blurred plane, into a signed buffer.
  // Borders replicate rather than zero-pad, for the same reason Sobel's do:
  // zero-padding manufactures a hard response along the frame edge that
  // isn't in the scene.
#pragma omp parallel for
  for (uint32_t y = 0; y < height; ++y) {
    const uint32_t y_up = y == 0 ? 0 : y - 1;
    const uint32_t y_dn = y + 1 >= height ? height - 1 : y + 1;
    const uint8_t *row_up = log_blurred + (size_t)y_up * width;
    const uint8_t *row = log_blurred + (size_t)y * width;
    const uint8_t *row_dn = log_blurred + (size_t)y_dn * width;
    int16_t *out = log_response + (size_t)y * width;

    for (uint32_t x = 0; x < width; ++x) {
      const uint32_t x_l = x == 0 ? 0 : x - 1;
      const uint32_t x_r = x + 1 >= width ? width - 1 : x + 1;
      out[x] = laplacian4(row_up[x], row[x_l], row[x], row[x_r], row_dn[x]);
    }
  }

  // Stage 3: mark zero-crossings. Each pixel is compared against its right
  // and lower neighbour only -- checking all four would mark both sides of
  // every crossing, doubling every contour's width and losing the
  // thin-line property that is the reason to use this operator at all.
  uint8_t *out_y = dst_planes[0];
#pragma omp parallel for
  for (uint32_t y = 0; y < height; ++y) {
    const int16_t *resp = log_response + (size_t)y * width;
    const int16_t *resp_dn =
        log_response + (size_t)(y + 1 >= height ? y : y + 1) * width;
    uint8_t *out_row = out_y + (size_t)y * stride;

    for (uint32_t x = 0; x < width; ++x) {
      const int16_t here = resp[x];
      bool edge = false;
      if (x + 1 < width)
        edge = crosses_zero(here, resp[x + 1], threshold);
      if (!edge && y + 1 < height)
        edge = crosses_zero(here, resp_dn[x], threshold);
      out_row[x] = edge ? 255 : 0;
    }
  }

  // Same as sobel_edges: the result is a luma-only edge map, so neutralize
  // chroma rather than carrying through colour that no longer corresponds
  // to anything in the output.
  memset(dst_planes[1], 128, stride_arr[1] * height);
  memset(dst_planes[2], 128, stride_arr[2] * height);
}
