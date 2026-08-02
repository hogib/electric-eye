#include "point_opps.h"
#include "video_frame.h"
#include <stddef.h>
#include <stdint.h>
#include <string.h>

/* NEON intrinsics header is shared by AArch64 and ARMv7 (with NEON enabled).
 * We gate on architecture explicitly rather than just __ARM_NEON so the
 * AArch64-only intrinsics (e.g. vminvq_u8/vmaxvq_u8) can be kept in their
 * own branch below. */
#if defined(__aarch64__)
#include <arm_neon.h>
#define GS_NEON_AARCH64 1
#elif defined(__arm__) && defined(__ARM_NEON)
#include <arm_neon.h>
#define GS_NEON_ARM32 1
#endif

inline uint32_t calc_y_plane_size(uint32_t height, uint32_t width) {
  return height * width;
}

void grayscale(VideoFrame *frame) {
  if (!frame || !frame->planes[1] || !frame->planes[2])
    return;

  memset(frame->planes[1], 128, frame->plane_sizes[1]);
  memset(frame->planes[2], 128, frame->plane_sizes[2]);
}

void gs_contrast_normalize(VideoFrame *frame) {
  if (!frame || !frame->planes[0])
    return;

  uint8_t r_min = y_plain_max_jpeg;
  uint8_t r_max = y_plain_min_jpeg;

#if defined(GS_NEON_AARCH64)
#pragma omp parallel for reduction(min : r_min) reduction(max : r_max)
  for (uint32_t y = 0; y < frame->height; ++y) {
    uint8_t *row = frame->planes[0] + (y * frame->stride[0]);
    uint8x16_t v_min = vdupq_n_u8(y_plain_max_jpeg);
    uint8x16_t v_max = vdupq_n_u8(y_plain_min_jpeg);

    uint32_t x = 0;
    for (; x + 15 < frame->width; x += 16) {
      uint8x16_t v = vld1q_u8(&row[x]);
      v_min = vminq_u8(v_min, v);
      v_max = vmaxq_u8(v_max, v);
    }

    // AArch64 specific: Horizontal vector reduction to scalar
    uint8_t row_min = vminvq_u8(v_min);
    uint8_t row_max = vmaxvq_u8(v_max);

    if (row_min < r_min)
      r_min = row_min;
    if (row_max > r_max)
      r_max = row_max;

    // Cleanup remainder pixels
    for (; x < frame->width; ++x) {
      if (row[x] < r_min)
        r_min = row[x];
      if (row[x] > r_max)
        r_max = row[x];
    }
  }
#elif defined(GS_NEON_ARM32)
#pragma omp parallel for reduction(min : r_min) reduction(max : r_max)
  for (uint32_t y = 0; y < frame->height; ++y) {
    uint8_t *row = frame->planes[0] + (y * frame->stride[0]);
    uint8x16_t v_min = vdupq_n_u8(y_plain_max_jpeg);
    uint8x16_t v_max = vdupq_n_u8(y_plain_min_jpeg);

    uint32_t x = 0;
    for (; x + 15 < frame->width; x += 16) {
      uint8x16_t v = vld1q_u8(&row[x]);
      v_min = vminq_u8(v_min, v);
      v_max = vmaxq_u8(v_max, v);
    }

    // ARMv7 NEON has no vminvq_u8/vmaxvq_u8 (those are AArch64-only), so the
    // 128-bit -> scalar horizontal reduction has to be done manually via
    // pairwise min/max down from 8 lanes -> 4 -> 2 -> 1.
    uint8x8_t min_lo = vget_low_u8(v_min);
    uint8x8_t min_hi = vget_high_u8(v_min);
    uint8x8_t min8 = vmin_u8(min_lo, min_hi);
    min8 = vpmin_u8(min8, min8);
    min8 = vpmin_u8(min8, min8);
    min8 = vpmin_u8(min8, min8);
    uint8_t row_min = vget_lane_u8(min8, 0);

    uint8x8_t max_lo = vget_low_u8(v_max);
    uint8x8_t max_hi = vget_high_u8(v_max);
    uint8x8_t max8 = vmax_u8(max_lo, max_hi);
    max8 = vpmax_u8(max8, max8);
    max8 = vpmax_u8(max8, max8);
    max8 = vpmax_u8(max8, max8);
    uint8_t row_max = vget_lane_u8(max8, 0);

    if (row_min < r_min)
      r_min = row_min;
    if (row_max > r_max)
      r_max = row_max;

    // Cleanup remainder pixels
    for (; x < frame->width; ++x) {
      if (row[x] < r_min)
        r_min = row[x];
      if (row[x] > r_max)
        r_max = row[x];
    }
  }
#else
#pragma omp parallel for reduction(min : r_min) reduction(max : r_max)
  for (uint32_t y = 0; y < frame->height; ++y) {
    uint8_t *row = frame->planes[0] + (y * frame->stride[0]);
    for (uint32_t x = 0; x < frame->width; ++x) {
      if (row[x] < r_min)
        r_min = row[x];
      if (row[x] > r_max)
        r_max = row[x];
    }
  }
#endif

  if (r_max == r_min)
    return;

  // Calculate LUT
  float scale = (float)y_plain_max_jpeg / (r_max - r_min);
  uint8_t lut[256];

#pragma omp parallel for
  for (int i = 0; i <= y_plain_max_jpeg; ++i) {
    float val = (i - r_min) * scale;
    if (val > (float)y_plain_max_jpeg)
      val = (float)y_plain_max_jpeg;
    if (val < (float)y_plain_min_jpeg)
      val = (float)y_plain_min_jpeg;
    lut[i] = (uint8_t)(val + 0.5f);
  }

// NOTE: Applying a 256-byte LUT across 16-byte registers is extremely complex
// and often slower than a standard memory fetch due to L1 cache efficiency.
#pragma omp parallel for
  for (uint32_t y = 0; y < frame->height; ++y) {
    uint8_t *row = frame->planes[0] + (y * frame->stride[0]);
    for (uint32_t x = 0; x < frame->width; ++x) {
      row[x] = lut[row[x]];
    }
  }
}

void gs_threshold_by_value(VideoFrame *frame, uint8_t tval) {
  if (!frame || !frame->planes[0])
    return;

#if defined(GS_NEON_AARCH64)
  uint8x16_t v_tval = vdupq_n_u8(tval);

#pragma omp parallel for
  for (uint32_t y = 0; y < frame->height; ++y) {
    uint8_t *row = frame->planes[0] + (y * frame->stride[0]);
    uint32_t x = 0;

    for (; x + 15 < frame->width; x += 16) {
      uint8x16_t v = vld1q_u8(&row[x]);
      uint8x16_t mask = vcgeq_u8(v, v_tval);
      vst1q_u8(&row[x], mask);
    }

    for (; x < frame->width; ++x) {
      row[x] = (row[x] < tval) ? y_plain_min_jpeg : y_plain_max_jpeg;
    }
  }
#elif defined(GS_NEON_ARM32)
  // ARMv7 NEON: vcgeq_u8/vld1q_u8/vst1q_u8 are all available identically to
  // AArch64 here, so the loop body is the same as the 64-bit path above.
  uint8x16_t v_tval = vdupq_n_u8(tval);

#pragma omp parallel for
  for (uint32_t y = 0; y < frame->height; ++y) {
    uint8_t *row = frame->planes[0] + (y * frame->stride[0]);
    uint32_t x = 0;

    for (; x + 15 < frame->width; x += 16) {
      uint8x16_t v = vld1q_u8(&row[x]);
      uint8x16_t mask = vcgeq_u8(v, v_tval);
      vst1q_u8(&row[x], mask);
    }

    for (; x < frame->width; ++x) {
      row[x] = (row[x] < tval) ? y_plain_min_jpeg : y_plain_max_jpeg;
    }
  }
#else
#pragma omp parallel for
  for (uint32_t y = 0; y < frame->height; ++y) {
    uint8_t *row = frame->planes[0] + (y * frame->stride[0]);
    for (uint32_t x = 0; x < frame->width; ++x) {
      row[x] = (row[x] < tval) ? y_plain_min_jpeg : y_plain_max_jpeg;
    }
  }
#endif
}

void gs_invert(VideoFrame *frame) {
  if (!frame || !frame->planes[0])
    return;

#if defined(GS_NEON_AARCH64)
#pragma omp parallel for
  for (uint32_t y = 0; y < frame->height; ++y) {
    uint8_t *row = frame->planes[0] + (y * frame->stride[0]);
    uint32_t x = 0;

    for (; x + 15 < frame->width; x += 16) {
      uint8x16_t v = vld1q_u8(&row[x]);
      v = vmvnq_u8(v); // Hardware Bitwise NOT (255 - val)
      vst1q_u8(&row[x], v);
    }

    for (; x < frame->width; ++x) {
      row[x] = y_plain_max_jpeg - row[x];
    }
  }
#elif defined(GS_NEON_ARM32)
  // ARMv7 NEON: vmvnq_u8 (bitwise NOT) is available identically to AArch64.
#pragma omp parallel for
  for (uint32_t y = 0; y < frame->height; ++y) {
    uint8_t *row = frame->planes[0] + (y * frame->stride[0]);
    uint32_t x = 0;

    for (; x + 15 < frame->width; x += 16) {
      uint8x16_t v = vld1q_u8(&row[x]);
      v = vmvnq_u8(v); // Hardware Bitwise NOT (255 - val)
      vst1q_u8(&row[x], v);
    }

    for (; x < frame->width; ++x) {
      row[x] = y_plain_max_jpeg - row[x];
    }
  }
#else
  uint8_t lut[y_plain_max_jpeg + 1];
  for (int i = 0; i <= y_plain_max_jpeg; ++i) {
    lut[i] = y_plain_max_jpeg - i;
  }

#pragma omp parallel for
  for (uint32_t y = 0; y < frame->height; ++y) {
    uint8_t *row = frame->planes[0] + (y * frame->stride[0]);
    for (uint32_t x = 0; x < frame->width; ++x) {
      row[x] = lut[row[x]];
    }
  }
#endif
}

#if defined(GS_NEON_AARCH64)
// out = u + (target - u) * strength / 255 -- a lerp from u toward target,
// weighted by strength/255. Deployment target is the Pi 5 (AArch64 only,
// per the project's stated scope), so unlike gs_contrast_normalize/
// gs_threshold_by_value/gs_invert above, this has no ARMv7 branch -- ARM32
// builds fall through to the scalar path below instead.
//
// Verified bit-exact against the scalar loop exhaustively over every
// (target, strength) pair -- the entire 256x256 input domain, not a
// sample -- across four row-content patterns (all-zero, all-max, a ramp,
// and pseudo-random), 262,144 trials total, cross-compiled for aarch64 and
// run under qemu-user emulation; this file has not been built or run on
// real hardware.
static inline uint8x8_t tint_half8(uint16x8_t u, uint16x8_t target,
                                   int16_t strength) {
  // u and target both hold 0-255, so reinterpreting the zero-extended
  // unsigned widen as signed changes no values (the sign bit is never
  // set) -- it's what lets the subtraction below go negative correctly.
  int16x8_t diff = vsubq_s16(vreinterpretq_s16_u16(target),
                             vreinterpretq_s16_u16(u)); // range -255..255

  // diff*strength ranges +-65025, past int16 range (+-32767) -- widen the
  // multiply to 32-bit. vmull_s16 takes 4 lanes at a time, so the 8-lane
  // diff needs two calls (low half, high half).
  int16x8_t strength_v = vdupq_n_s16(strength);
  int32x4_t prod_lo = vmull_s16(vget_low_s16(diff), vget_low_s16(strength_v));
  int32x4_t prod_hi = vmull_s16(vget_high_s16(diff), vget_high_s16(strength_v));

  // Signed truncating division by 255, matching C's `/` (truncates toward
  // zero) exactly -- NEON has no integer divide. (x+1+(x>>8))>>8 computes
  // x/255 exactly for unsigned x in [0, 65535], but only for non-negative
  // x; since prod can be negative, divide the magnitude with that trick,
  // then reapply the sign via the standard XOR-mask-and-subtract negate
  // (mask is all-1s when prod<0, all-0s otherwise, from an arithmetic
  // shift replicating the sign bit).
  uint32x4_t abs_lo = vreinterpretq_u32_s32(vabsq_s32(prod_lo));
  uint32x4_t q_lo_mag = vshrq_n_u32(
      vaddq_u32(vaddq_u32(abs_lo, vdupq_n_u32(1)), vshrq_n_u32(abs_lo, 8)), 8);
  int32x4_t mask_lo = vshrq_n_s32(prod_lo, 31);
  int32x4_t q_lo =
      vsubq_s32(veorq_s32(vreinterpretq_s32_u32(q_lo_mag), mask_lo), mask_lo);

  uint32x4_t abs_hi = vreinterpretq_u32_s32(vabsq_s32(prod_hi));
  uint32x4_t q_hi_mag = vshrq_n_u32(
      vaddq_u32(vaddq_u32(abs_hi, vdupq_n_u32(1)), vshrq_n_u32(abs_hi, 8)), 8);
  int32x4_t mask_hi = vshrq_n_s32(prod_hi, 31);
  int32x4_t q_hi =
      vsubq_s32(veorq_s32(vreinterpretq_s32_u32(q_hi_mag), mask_hi), mask_hi);

  int32x4_t u32_lo = vreinterpretq_s32_u32(vmovl_u16(vget_low_u16(u)));
  int32x4_t u32_hi = vreinterpretq_s32_u32(vmovl_u16(vget_high_u16(u)));

  int16x8_t sum16 = vcombine_s16(vqmovn_s32(vaddq_s32(u32_lo, q_lo)),
                                 vqmovn_s32(vaddq_s32(u32_hi, q_hi)));
  return vqmovun_s16(sum16); // result is always in [0,255] by construction
}

static void tint_row(uint8_t *row, uint32_t width, uint8_t target,
                     uint8_t strength) {
  uint8x16_t target_v16 = vdupq_n_u8(target);
  uint16x8_t target_lo = vmovl_u8(vget_low_u8(target_v16));
  uint16x8_t target_hi = vmovl_u8(vget_high_u8(target_v16));

  uint32_t x = 0;
  for (; x + 15 < width; x += 16) {
    uint8x16_t u = vld1q_u8(&row[x]);
    uint8x8_t lo = tint_half8(vmovl_u8(vget_low_u8(u)), target_lo,
                             (int16_t)strength);
    uint8x8_t hi = tint_half8(vmovl_u8(vget_high_u8(u)), target_hi,
                             (int16_t)strength);
    vst1q_u8(&row[x], vcombine_u8(lo, hi));
  }
  for (; x < width; ++x) {
    int32_t diff = (int32_t)target - (int32_t)row[x];
    row[x] = (uint8_t)(row[x] + (diff * (int32_t)strength) / 255);
  }
}
#else
static void tint_row(uint8_t *row, uint32_t width, uint8_t target,
                     uint8_t strength) {
  for (uint32_t x = 0; x < width; ++x) {
    int32_t diff = (int32_t)target - (int32_t)row[x];
    row[x] = (uint8_t)(row[x] + (diff * (int32_t)strength) / 255);
  }
}
#endif

/*
 * Applies a color tint by pushing the U/V (chroma) planes toward a target
 * color instead of zeroing them out like grayscale() does. This is what
 * gives you an actual color cast (sepia, blue tint, etc.) rather than a flat
 * wash — the luma (Y) plane, and therefore all brightness/contrast detail,
 * is left completely untouched.
 *
 * target_u / target_v: the chroma values to tint toward. U (Y-Cb, blue-yellow
 *   axis) and V (Y-Cr, red-green axis) are both centered at 128 = neutral.
 *   Push U up for a blue cast, down for yellow. Push V up for red/warm, down
 *   for green.
 * strength: 0 = no change (original camera color), 255 = chroma fully
 *   replaced by target_u/target_v. Values in between blend linearly, so e.g.
 *   128 mixes the original color halfway toward the target.
 *
 * A couple of presets to try:
 *   Sepia:      gs_tint(frame, 90, 150, 180);
 *   Blue tint:  gs_tint(frame, 190, 100, 140);
 */
void color_tint(VideoFrame *frame, uint8_t target_u, uint8_t target_v,
             uint8_t strength) {
  if (!frame || !frame->planes[1] || !frame->planes[2])
    return;

  if (strength == 0)
    return; // No-op fast path: original color untouched.

#pragma omp parallel for
  for (uint32_t y = 0; y < frame->height; ++y) {
    uint8_t *u_row = frame->planes[1] + (y * frame->stride[1]);
    uint8_t *v_row = frame->planes[2] + (y * frame->stride[2]);

    // Chroma planes are half-width for I422, so loop bound is stride[1/2]
    // (== chroma_width), not frame->width.
    tint_row(u_row, frame->stride[1], target_u, strength);
    tint_row(v_row, frame->stride[2], target_v, strength);
  }
}
