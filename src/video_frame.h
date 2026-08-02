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

  // The work buffer: what the consumer sends out once the effect chain
  // finishes. Point ops (color_tint, grayscale, ...) read and write this
  // in place, exactly as before the chain existed.
  uint8_t *pixel_data;
  uint8_t *planes[max_planes];

  // The untouched camera frame. producer_loop is the only writer. The
  // first neighborhood op in a chain (blur or Sobel) reads from here.
  uint8_t *raw_data;
  uint8_t *raw_planes[max_planes];

  // A third buffer, needed once a chain can hold more than one
  // neighborhood op (e.g. blur -> Sobel): each such op must read a buffer
  // no other stage in the same frame is still writing, so they ping-pong
  // between work and spare rather than always targeting work. Point ops
  // stay untouched by this -- effect_chain.c copies whichever buffer holds
  // the chain's current state into work before calling one, so their
  // always-mutate-frame->planes-in-place contract never has to change.
  uint8_t *spare_data;
  uint8_t *spare_planes[max_planes];

  // All three buffers share one layout: same width/height, so one set of
  // sizes and strides describes all of them.
  size_t plane_sizes[max_planes];
  size_t stride[max_stride];
  int64_t pts; // Presentation time stamp
} VideoFrame;

VideoFrame *vf_create(uint32_t width, uint32_t height, int64_t pts);
void vf_free(VideoFrame *frame);

void vf_pool_free(VideoFrame **pool, unsigned int allocated_size);
VideoFrame **vf_pool_create(unsigned int pool_size, uint32_t width,
                            uint32_t height);
