#pragma once
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>

/* Constants */
constexpr uint32_t max_planes = 3;
constexpr uint32_t max_stride = 3;

typedef struct {
  uint32_t height;
  uint32_t width;

  // The work buffer: what effects operate on and what the consumer sends
  // out. Point ops (color_tint, grayscale, ...) still read and write this
  // in place, exactly as before.
  uint8_t *pixel_data;
  uint8_t *planes[max_planes];

  // The untouched camera frame. producer_loop is the only writer. Effects
  // that need neighborhood pixels (Sobel) read from here so they can write
  // planes[] without a stage eating its own output.
  uint8_t *raw_data;
  uint8_t *raw_planes[max_planes];

  // Both buffers share one layout: same width/height, so one set of sizes
  // and strides describes both.
  size_t plane_sizes[max_planes];
  size_t stride[max_stride];
  int64_t pts; // Presentation time stamp
} VideoFrame;

VideoFrame *vf_create(uint32_t width, uint32_t height, int64_t pts);
void vf_free(VideoFrame *frame);

void vf_pool_free(VideoFrame **pool, unsigned int allocated_size);
VideoFrame **vf_pool_create(unsigned int pool_size, uint32_t width,
                            uint32_t height);
