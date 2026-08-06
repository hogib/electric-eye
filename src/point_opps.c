#include "point_opps.h"
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

void gs_contrast_normalize(const uint8_t *src, uint8_t *dst, uint32_t width,
                           uint32_t height, size_t stride) {
  if (!src || !dst)
    return;

  uint8_t r_min = y_plain_max_jpeg;
  uint8_t r_max = y_plain_min_jpeg;

#if defined(GS_NEON_AARCH64)
#pragma omp parallel for reduction(min : r_min) reduction(max : r_max)
  for (uint32_t y = 0; y < height; ++y) {
    const uint8_t *row = src + (y * stride);
    uint8x16_t v_min = vdupq_n_u8(y_plain_max_jpeg);
    uint8x16_t v_max = vdupq_n_u8(y_plain_min_jpeg);

    uint32_t x = 0;
    for (; x + 15 < width; x += 16) {
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
    for (; x < width; ++x) {
      if (row[x] < r_min)
        r_min = row[x];
      if (row[x] > r_max)
        r_max = row[x];
    }
  }
#elif defined(GS_NEON_ARM32)
#pragma omp parallel for reduction(min : r_min) reduction(max : r_max)
  for (uint32_t y = 0; y < height; ++y) {
    const uint8_t *row = src + (y * stride);
    uint8x16_t v_min = vdupq_n_u8(y_plain_max_jpeg);
    uint8x16_t v_max = vdupq_n_u8(y_plain_min_jpeg);

    uint32_t x = 0;
    for (; x + 15 < width; x += 16) {
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
    for (; x < width; ++x) {
      if (row[x] < r_min)
        r_min = row[x];
      if (row[x] > r_max)
        r_max = row[x];
    }
  }
#else
#pragma omp parallel for reduction(min : r_min) reduction(max : r_max)
  for (uint32_t y = 0; y < height; ++y) {
    const uint8_t *row = src + (y * stride);
    for (uint32_t x = 0; x < width; ++x) {
      if (row[x] < r_min)
        r_min = row[x];
      if (row[x] > r_max)
        r_max = row[x];
    }
  }
#endif

  if (r_max == r_min) {
    // No dynamic range to stretch: pass src through unchanged. Needed even
    // though this used to be a plain early return -- now that src and dst
    // may be different buffers (effect_chain.c may call this with src
    // pointing at raw/spare rather than dst's own contents), skipping the
    // copy here would leave dst holding stale data from a previous frame
    // instead of this one.
    if (src != dst) {
#pragma omp parallel for
      for (uint32_t y = 0; y < height; ++y) {
        memcpy(dst + (size_t)y * stride, src + (size_t)y * stride, width);
      }
    }
    return;
  }

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
  for (uint32_t y = 0; y < height; ++y) {
    const uint8_t *src_row = src + (size_t)y * stride;
    uint8_t *dst_row = dst + (size_t)y * stride;
    for (uint32_t x = 0; x < width; ++x) {
      dst_row[x] = lut[src_row[x]];
    }
  }
}
