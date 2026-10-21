#include "video_frame.h"
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/*
 *@params:
 *  planes: Pointer to stream data. I422 Input stream expected.
 *  stride: Plane stride, refer to YUV I422 format.
 *  height: Height of video frame.
 *  width: Width of video frame.
 *  pts: Presentation time stamp. Feed to consumer to properly decode video.
 **/

/*
 * Constructor for an I422 VideoFrame.
 * Returns a pointer to the allocated frame, or NULL on failure.
 */
VideoFrame *vf_create(uint32_t width, uint32_t height, int64_t pts) {
  if (width == 0 || height == 0) {
    return NULL;
  }

  VideoFrame *frame = (VideoFrame *)calloc(1, sizeof(VideoFrame));

  if (!frame) {
    return NULL;
  }

  frame->width = width;
  frame->height = height;
  frame->pts = pts;

  // I422 Chroma width is halved.
  uint32_t chroma_width = (width + 1) / 2;

  frame->stride[0] = width;        // Y plane stride
  frame->stride[1] = chroma_width; // U plane stride
  frame->stride[2] = chroma_width; // V plane stride

  size_t y_size = (size_t)frame->stride[0] * height;
  size_t u_size = (size_t)frame->stride[1] * height;
  size_t v_size = (size_t)frame->stride[2] * height;

  frame->plane_sizes[0] = y_size;
  frame->plane_sizes[1] = u_size;
  frame->plane_sizes[2] = v_size;

  frame->pixel_data = (uint8_t *)malloc(y_size + u_size + v_size);
  if (!frame->pixel_data) {
    free(frame);
    return NULL;
  }

  // Each plane is an offset of pixel data.
  frame->planes[0] = frame->pixel_data;
  frame->planes[1] = frame->pixel_data + y_size;
  frame->planes[2] = frame->pixel_data + y_size + u_size;

  // Separate allocation, not a slice of pixel_data: raw and work need to
  // remain independently writable, and giving raw its own block means an
  // out-of-bounds write into one can never corrupt the other.
  frame->raw_data = (uint8_t *)malloc(y_size + u_size + v_size);
  if (!frame->raw_data) {
    free(frame->pixel_data);
    free(frame);
    return NULL;
  }

  frame->raw_planes[0] = frame->raw_data;
  frame->raw_planes[1] = frame->raw_data + y_size;
  frame->raw_planes[2] = frame->raw_data + y_size + u_size;

  // Third, separate allocation for the same reason raw_data is: a chain's
  // ping-pong target must not alias any buffer another stage in the same
  // frame is still reading.
  frame->spare_data = (uint8_t *)malloc(y_size + u_size + v_size);
  if (!frame->spare_data) {
    free(frame->raw_data);
    free(frame->pixel_data);
    free(frame);
    return NULL;
  }

  frame->spare_planes[0] = frame->spare_data;
  frame->spare_planes[1] = frame->spare_data + y_size;
  frame->spare_planes[2] = frame->spare_data + y_size + u_size;

  frame->owned[0] = frame->pixel_data;
  frame->owned[1] = frame->raw_data;
  frame->owned[2] = frame->spare_data;

  return frame;
}

void vf_free(VideoFrame *frame) {
  if (!frame)
    return;

  // owned[], not the role pointers: after an empty chain pixel_data and
  // raw_data are the same block, and freeing by role would free it twice.
  for (uint32_t i = 0; i < max_planes; ++i)
    free(frame->owned[i]);
  free(frame->source_jpeg);

  free(frame);
}

static void set_planes(uint8_t *planes[max_planes], uint8_t *data,
                       const size_t plane_sizes[max_planes]) {
  planes[0] = data;
  planes[1] = data + plane_sizes[0];
  planes[2] = data + plane_sizes[0] + plane_sizes[1];
}

void vf_set_work(VideoFrame *frame, uint8_t *data) {
  frame->pixel_data = data;
  set_planes(frame->planes, data, frame->plane_sizes);
}

void vf_swap_work_spare(VideoFrame *frame) {
  uint8_t *work = frame->pixel_data;
  vf_set_work(frame, frame->spare_data);
  frame->spare_data = work;
  set_planes(frame->spare_planes, work, frame->plane_sizes);
}

void vf_restore_roles(VideoFrame *frame) {
  if (frame->pixel_data != frame->raw_data)
    return;
  // spare never moves onto raw, so the one allocation that is neither raw
  // nor spare is the one work was using before it was pointed at raw.
  uint8_t *free_block = frame->owned[0] == frame->spare_data ? frame->owned[2]
                                                             : frame->owned[0];
  vf_set_work(frame, free_block);
}

bool vf_set_source_jpeg(VideoFrame *frame, const uint8_t *data, size_t len) {
  frame->source_jpeg_len = 0;
  if (frame->source_jpeg_cap < len) {
    uint8_t *grown = realloc(frame->source_jpeg, len);
    if (!grown)
      return false;
    frame->source_jpeg = grown;
    frame->source_jpeg_cap = len;
  }
  memcpy(frame->source_jpeg, data, len);
  frame->source_jpeg_len = len;
  return true;
}

void vf_pool_free(VideoFrame **pool, unsigned int allocated_size) {
  if (!pool) {
    return;
  }

  for (unsigned int i = 0; i < allocated_size; ++i) {
    if (pool[i]) {
      vf_free(pool[i]);
    }
  }

  free(pool);
}

VideoFrame **vf_pool_create(unsigned int pool_size, uint32_t width,
                            uint32_t height) {
  VideoFrame **pool = (VideoFrame **)malloc(pool_size * sizeof(VideoFrame *));
  if (!pool) {
    return NULL;
  }

  for (unsigned int i = 0; i < pool_size; ++i) {
    pool[i] = vf_create(width, height, 0);

    if (!pool[i]) {
      vf_pool_free(pool, i);
      return NULL;
    }
  }

  return pool;
}
