#pragma once
#include "frame_ring_buffer.h"
#include <stdatomic.h>
#include <stdint.h>
#include <threads.h>

static inline void sleep_us(long microseconds) {
  struct timespec ts;
  ts.tv_sec = microseconds / 1000000;
  ts.tv_nsec = (microseconds % 1000000) * 1000;

  thrd_sleep(&ts, NULL);
}

typedef struct {
  const char *filename;
  atomic_bool *is_running;
  FrameRingBuffer *ring_buffer_in;
  uint32_t frame_width;
  uint32_t frame_height;
} ProducerArgs;

typedef struct {
  atomic_bool *is_running;
  FrameRingBuffer *ring_buffer_in;
  FrameRingBuffer *ring_buffer_out;
  uint32_t frame_width;
  uint32_t frame_height;
} WorkerArgs;

typedef struct {
  const char *outpath;
  atomic_bool *is_running;
  FrameRingBuffer *ring_buffer_out;
  uint32_t frame_width;
  uint32_t frame_height;
} ConsumerArgs;

void *producer_loop(void *arg);

void *effects_loop(void *arg);

void *consumer_loop(void *arg);
