#include "video_pool.h"
#include "video_frame.h"
#include <pthread.h>
#include <stdlib.h>

struct VideoBufferPool {
  VideoFrame *frames;
  uint8_t **raw_memory;
  int *is_available;
  int pool_size;

  pthread_mutex_t lock;
};

VideoBufferPool *vpool_create(int num_frames, int width, int height) {
  VideoBufferPool *pool = malloc(sizeof(VideoBufferPool));

  if (!pool)
    return NULL;

  pool->pool_size = num_frames;
  pool->frames = malloc(sizeof(VideoFrame) * num_frames);
  pool->raw_memory = malloc(sizeof(uint8_t *) * num_frames);
  pool->is_available = malloc(sizeof(int) * num_frames);

  pthread_mutex_init(&pool->lock, NULL);

  int y_size = width * height;
  int uv_size = width * (height / 2);
  int total_frame_size = y_size + uv_size;

  for (int i = 0; i < num_frames; i++) {
    pool->is_available[i] = 1; // Mark as free

    // Allocate the single block of contiguous memory for this frame
    pool->raw_memory[i] = malloc(total_frame_size);

    // Setup the NV12 struct pointers
    pool->frames[i].format = FORMAT_NV12;
    pool->frames[i].width = width;
    pool->frames[i].height = height;

    // Plane 0 (Y) starts at the beginning
    pool->frames[i].planes[0] = pool->raw_memory[i];
    pool->frames[i].stride[0] = width;

    // Plane 1 (UV) starts exactly after the Y plane ends
    pool->frames[i].planes[1] = pool->raw_memory[i] + y_size;
    pool->frames[i].stride[1] = width;

    // Store the index so the frame knows where it lives in the pool
    pool->frames[i].pool_index = i;
  }

  return pool;
}
