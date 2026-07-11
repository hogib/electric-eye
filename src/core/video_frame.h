#pragma once
#include <stdint.h>

typedef enum {
  FORMAT_NV12,
  FORMAT_I420,
} VideoFormat;

typedef struct {
  VideoFormat format;
  int height;
  int width;
  uint8_t *planes[3];
  int stride[3];

} VideoFrame;

