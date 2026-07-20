#pragma once
#include <stdatomic.h>
#include <stdbool.h>

constexpr unsigned int ring_buffer_size = 32;

#if (ring_buffer_size & (ring_buffer_size - 1)) != 0
#error "ring_buffer_size must be a power of two!"
#endif

typedef struct FrameRingBuffer {
  void *buffer[ring_buffer_size];
  atomic_size_t head; // Written by Producer
  atomic_size_t tail; // Written by Consumer
} FrameRingBuffer;

void ring_init(FrameRingBuffer *rb);

bool ring_push(FrameRingBuffer *rb, void *frame_ptr);

bool ring_pop(FrameRingBuffer *rb, void **frame_ptr);
