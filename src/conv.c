#include "conv.h"
#include <stdint.h>
#include <string.h>

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
#pragma omp parallel for
  for (uint32_t y = 1; y + 1 < height; ++y) {
    const uint8_t *row_above = raw_y + (size_t)(y - 1) * stride;
    const uint8_t *row = raw_y + (size_t)y * stride;
    const uint8_t *row_below = raw_y + (size_t)(y + 1) * stride;
    uint8_t *out_row = out_y + (size_t)y * stride;

    for (uint32_t x = 1; x + 1 < width; ++x) {
      out_row[x] = sobel_magnitude(row_above[x - 1], row_above[x],
                                   row_above[x + 1], row[x - 1], row[x + 1],
                                   row_below[x - 1], row_below[x],
                                   row_below[x + 1]);
    }
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
