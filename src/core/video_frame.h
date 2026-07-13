#pragma once
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
  size_t stride[max_stride];
  int64_t pts; // Presentation time stamp
} VideoFrame;
