#include "point_opps.h"
#include "video_frame.h"
#include <stddef.h>
#include <string.h>

inline uint32_t calc_frame_size(uint32_t height, uint32_t width) {
  return height * width;
}

void grayscale(VideoFrame *frame) {
  if (!frame || !frame->planes[1] || !frame->planes[2])
    return;

  size_t u_size = (size_t)frame->stride[1] * frame->height;
  size_t v_size = (size_t)frame->stride[2] * frame->height;

  memset(frame->planes[1], 128, u_size);
  memset(frame->planes[2], 128, v_size);
}

void gs_contrast_normalize(VideoFrame *frame) {
  if (!frame || !frame->planes[0])
    return;

  uint8_t r_min = y_plain_max_jpeg;
  uint8_t r_max = y_plain_min_jpeg;

  uint8_t lut[256];

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

  if (r_max == r_min)
    return;

  float scale = (float)y_plain_max_jpeg / (r_max - r_min);

#pragma omp parallel for
  for (int i = 0; i <= y_plain_max_jpeg; ++i) {
    float val = (i - r_min) * scale;

    if (val > (float)y_plain_max_jpeg)
      val = (float)y_plain_max_jpeg;
    if (val < (float)y_plain_min_jpeg)
      val = (float)y_plain_min_jpeg;

    lut[i] = (uint8_t)(val + 0.5f);
  }

  for (uint32_t y = 0; y < frame->height; ++y) {
    uint8_t *row = frame->planes[0] + (y * frame->stride[0]);

    for (uint32_t x = 0; x < frame->width; ++x) {
      row[x] = lut[row[x]];
    }
  }
}

void gs_threshold_by_value(VideoFrame *frame, uint8_t tval) {
  if (!frame || !frame->planes[1] || !frame->planes[2])
    return;

  if (!frame->planes[0])
    return;

  for (int32_t y = 0; y < frame->height; ++y) {
    uint8_t *row = frame->planes[0] + (y * frame->stride[0]);

    for (uint32_t x = 0; x < frame->width; ++x) {
      if (row[x] < tval)
        row[x] = y_plain_min_jpeg;
      else
        row[x] = y_plain_max_jpeg;
    }
  }
}
