#include "conv.h"
#include <stdint.h>
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
static inline uint8_t sobel_magnitude(int32_t p00, int32_t p01, int32_t p02,
                                      int32_t p10, int32_t p12, int32_t p20,
                                      int32_t p21, int32_t p22) {
  int32_t gx = (p02 + 2 * p12 + p22) - (p00 + 2 * p10 + p20);
  int32_t gy = (p20 + 2 * p21 + p22) - (p00 + 2 * p01 + p02);

  // |Gx| + |Gy| (L1) instead of sqrt(Gx^2 + Gy^2) (L2, the "true" gradient
  // magnitude): visually near-identical, no float, no sqrt -- this is what
  // realtime edge detectors use in practice.
  int32_t mag = (gx < 0 ? -gx : gx) + (gy < 0 ? -gy : gy);
  return (uint8_t)(mag > 255 ? 255 : mag);
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
// 8-lane halves, runs sobel_half8 on each half, recombines. Parameter
// order/names match sobel_magnitude exactly for easy side-by-side
// comparison.
static inline uint8x16_t sobel_row16(uint8x16_t p00, uint8x16_t p01,
                                     uint8x16_t p02, uint8x16_t p10,
                                     uint8x16_t p12, uint8x16_t p20,
                                     uint8x16_t p21, uint8x16_t p22) {
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
  return vcombine_u8(lo, hi);
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
                                       int32_t x, int32_t y) {
  return sobel_magnitude(
      clamped_sample(y_plane, stride, width, height, x - 1, y - 1),
      clamped_sample(y_plane, stride, width, height, x, y - 1),
      clamped_sample(y_plane, stride, width, height, x + 1, y - 1),
      clamped_sample(y_plane, stride, width, height, x - 1, y),
      clamped_sample(y_plane, stride, width, height, x + 1, y),
      clamped_sample(y_plane, stride, width, height, x - 1, y + 1),
      clamped_sample(y_plane, stride, width, height, x, y + 1),
      clamped_sample(y_plane, stride, width, height, x + 1, y + 1));
}

void sobel_edges(VideoFrame *frame) {
  if (!frame || !frame->raw_planes[0] || !frame->planes[0])
    return;

  const uint8_t *raw_y = frame->raw_planes[0];
  uint8_t *out_y = frame->planes[0];
  uint32_t width = frame->width;
  uint32_t height = frame->height;
  size_t stride = frame->stride[0];

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
          vld1q_u8(&row_below[x]), vld1q_u8(&row_below[x + 1]));
      vst1q_u8(&out_row[x], result);
    }
    for (; x + 1 < width; ++x) {
      out_row[x] = sobel_magnitude(row_above[x - 1], row_above[x],
                                   row_above[x + 1], row[x - 1], row[x + 1],
                                   row_below[x - 1], row_below[x],
                                   row_below[x + 1]);
    }
#else
    for (uint32_t x = 1; x + 1 < width; ++x) {
      out_row[x] = sobel_magnitude(row_above[x - 1], row_above[x],
                                   row_above[x + 1], row[x - 1], row[x + 1],
                                   row_below[x - 1], row_below[x],
                                   row_below[x + 1]);
    }
#endif
  }

  // Border: no full neighborhood exists past the frame edge. Replicate
  // rather than zero-pad -- zero-padding a bright edge would read as a false
  // hard line running along the frame boundary that has nothing to do with
  // the actual scene.
  for (uint32_t x = 0; x < width; ++x) {
    out_y[x] = sobel_at_clamped(raw_y, stride, width, height, (int32_t)x, 0);
    out_y[(size_t)(height - 1) * stride + x] = sobel_at_clamped(
        raw_y, stride, width, height, (int32_t)x, (int32_t)height - 1);
  }
  for (uint32_t y = 1; y + 1 < height; ++y) {
    out_y[(size_t)y * stride] =
        sobel_at_clamped(raw_y, stride, width, height, 0, (int32_t)y);
    out_y[(size_t)y * stride + (width - 1)] = sobel_at_clamped(
        raw_y, stride, width, height, (int32_t)width - 1, (int32_t)y);
  }

  memset(frame->planes[1], 128, frame->plane_sizes[1]);
  memset(frame->planes[2], 128, frame->plane_sizes[2]);
}
