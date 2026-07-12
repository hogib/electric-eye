#pragma once
#include <stdint.h>
#include <stdlib.h>

#define MAX_STRIDE 3
#define MAX_PLANES 3

typedef struct {
  uint32_t height;
  uint32_t width;
  uint8_t *pixel_data;
  uint8_t *planes[MAX_PLANES];
  size_t stride[MAX_STRIDE];
  int64_t pts;
} VideoFrame;
