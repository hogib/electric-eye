#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>

/* Constants */
enum { max_planes = 3 };
enum { max_stride = 3 };

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

  // The three allocations behind the role pointers above. Roles can move
  // between them -- effect_chain.c swaps work and spare instead of copying
  // one into the other, and points work at raw for an empty chain -- so
  // vf_free() frees these rather than whatever the roles point at now.
  // raw_data always stays on owned[1].
  uint8_t *owned[3];

  // The compressed bytes this frame was decoded from, when the camera sent
  // MJPEG: the recording tap can write these instead of the decoded planes
  // (see record_format in config.h). source_jpeg_len is 0 for a frame that
  // never was a JPEG (YUYV, rpicam yuv420). Grown lazily, never shrunk.
  uint8_t *source_jpeg;
  size_t source_jpeg_len;
  size_t source_jpeg_cap;
} VideoFrame;

VideoFrame *vf_create(uint32_t width, uint32_t height, int64_t pts);
void vf_free(VideoFrame *frame);

// Points the work role (pixel_data/planes) at `data`, which must be one of
// frame->owned[].
void vf_set_work(VideoFrame *frame, uint8_t *data);

// Swaps which allocations the work and spare roles point at. O(1): this is
// what replaces a full-frame copy from spare into work.
void vf_swap_work_spare(VideoFrame *frame);

// Undoes vf_set_work(frame, frame->raw_data): makes work and spare point at
// the two allocations raw isn't using again. A no-op when they already do.
void vf_restore_roles(VideoFrame *frame);

// Keeps a copy of the JPEG this frame was decoded from. Returns false (and
// leaves source_jpeg_len at 0) only if growing the buffer failed.
bool vf_set_source_jpeg(VideoFrame *frame, const uint8_t *data, size_t len);

void vf_pool_free(VideoFrame **pool, unsigned int allocated_size);
VideoFrame **vf_pool_create(unsigned int pool_size, uint32_t width,
                            uint32_t height);
