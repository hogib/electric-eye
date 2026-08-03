#pragma once
#include "config.h"
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
  FrameRingBuffer *ring_buffer_free;
  uint32_t frame_width;
  uint32_t frame_height;
} ProducerArgs;

typedef struct {
  atomic_bool *is_running;
  FrameRingBuffer *ring_buffer_in;
  FrameRingBuffer *ring_buffer_out;
  const ConfigWatcher *config;
  uint32_t frame_width;
  uint32_t frame_height;
} WorkerArgs;

typedef struct {
  const char *outpath;
  atomic_bool *is_running;
  FrameRingBuffer *ring_buffer_out;
  FrameRingBuffer *ring_buffer_free;
  const ConfigWatcher *config; // for the recording tap's record_path
  uint32_t frame_width;
  uint32_t frame_height;
} ConsumerArgs;

// All three rings, read-only from here -- stats_loop only ever calls
// ring_get_stats() on them, never pushes/pops.
typedef struct {
  atomic_bool *is_running;
  const FrameRingBuffer *ring_buffer_in;
  const FrameRingBuffer *ring_buffer_out;
  const FrameRingBuffer *ring_buffer_free;
} StatsArgs;

void *producer_loop(void *arg);

void *effects_loop(void *arg);

void *consumer_loop(void *arg);

/*
 * Periodically logs each ring's occupancy, high-water mark, and stall
 * counts (see RingStats in frame_ring_buffer.h) -- real numbers on whether
 * the 32-slot rings ever actually saturate, in place of estimating. Purely
 * diagnostic: never touches a ring's contents, only ring_get_stats().
 */
void *stats_loop(void *arg);
