#include "point_opps.h"
#include "video_frame.h"
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#if defined(__aarch64__)
#include <arm_neon.h>
#elif defined(__arm__) && (defined(__ARM_NEON__) || defined(__ARM_NEON))
#include <arm_neon.h>
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

#if defined(__aarch64__)
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
#elif defined(__arm__) && (defined(__ARM_NEON__) || defined(__ARM_NEON))
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

    // ARMv7 NEON has no vminvq_u8/vmaxvq_u8 (those "across vector"
    // intrinsics are AArch64-only). Reduce 16 -> 8 -> 4 -> 2 -> 1 lanes
    // using pairwise min/max on the two 64-bit halves instead.
    uint8x8_t min_pair = vpmin_u8(vget_low_u8(v_min), vget_high_u8(v_min));
    uint8x8_t max_pair = vpmax_u8(vget_low_u8(v_max), vget_high_u8(v_max));

    min_pair = vpmin_u8(min_pair, min_pair);
    max_pair = vpmax_u8(max_pair, max_pair);

    min_pair = vpmin_u8(min_pair, min_pair);
    max_pair = vpmax_u8(max_pair, max_pair);

    min_pair = vpmin_u8(min_pair, min_pair);
    max_pair = vpmax_u8(max_pair, max_pair);

    uint8_t row_min = vget_lane_u8(min_pair, 0);
    uint8_t row_max = vget_lane_u8(max_pair, 0);

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

// vld1q_u8 / vcgeq_u8 / vst1q_u8 / vdupq_n_u8 are identical on ARMv7 NEON
// and AArch64, so a single branch (widened to also match the ARMv7-only
// __ARM_NEON__ feature macro) already serves as the 32-bit counterpart.
#if defined(__ARM_NEON) || defined(__ARM_NEON__) || defined(__aarch64__)
  uint8x16_t v_tval = vdupq_n_u8(tval);

#pragma omp parallel for
  for (int32_t y = 0; y < frame->height; ++y) {
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

// vld1q_u8 / vmvnq_u8 / vst1q_u8 are identical on ARMv7 NEON and AArch64,
// so this single branch (widened to also match the ARMv7-only __ARM_NEON__
// feature macro) already serves as the 32-bit counterpart.
#if defined(__ARM_NEON) || defined(__ARM_NEON__) || defined(__aarch64__)
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
