#include "point_opps.h"
#include "video_frame.h"
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#if defined(__ARM_NEON) || defined(__aarch64__)
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

#if defined(__ARM_NEON) || defined(__aarch64__)
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

#if defined(__ARM_NEON) || defined(__aarch64__)
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
  for (int32_t y = 0; y < frame->height; ++y) {
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

#if defined(__ARM_NEON) || defined(__aarch64__)
#pragma omp parallel for
  for (int32_t y = 0; y < frame->height; ++y) {
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
  for (int32_t y = 0; y < frame->height; ++y) {
    uint8_t *row = frame->planes[0] + (y * frame->stride[0]);
    for (uint32_t x = 0; x < frame->width; ++x) {
      row[x] = lut[row[x]];
    }
  }
#endif
}
