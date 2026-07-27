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
  uint8_t *pixel_data;
  uint8_t *planes[max_planes];
  size_t plane_sizes[max_planes];
  size_t stride[max_stride];
  int64_t pts; // Presentation time stamp
} VideoFrame;

VideoFrame *vf_create(uint32_t width, uint32_t height, int64_t pts);
void vf_free(VideoFrame *frame);

void vf_pool_free(VideoFrame **pool, unsigned int allocated_size);
VideoFrame **vf_pool_create(unsigned int pool_size, uint32_t width,
                            uint32_t height);
