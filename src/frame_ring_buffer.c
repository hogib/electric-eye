#include "frame_ring_buffer.h"
#include <stddef.h>

void ring_init(FrameRingBuffer *rb) {
  atomic_init(&rb->head, 0);
  atomic_init(&rb->tail, 0);
}

// Called only by the PRODUCER thread
bool ring_push(FrameRingBuffer *rb, void *frame_ptr) {
  size_t head = atomic_load_explicit(&rb->head, memory_order_relaxed);
  size_t tail = atomic_load_explicit(&rb->tail, memory_order_acquire);

  size_t next_head = (head + 1) & (ring_buffer_size - 1);

  if (next_head == tail) {
    return false; // Buffer is full (Frame drop!)
  }

  rb->buffer[head] = frame_ptr;
  atomic_store_explicit(&rb->head, next_head, memory_order_release);
  return true;
}

// Called only by the CONSUMER thread
bool ring_pop(FrameRingBuffer *rb, void **frame_ptr) {
  size_t tail = atomic_load_explicit(&rb->tail, memory_order_relaxed);
  size_t head = atomic_load_explicit(&rb->head, memory_order_acquire);

  if (tail == head) {
    return false; // Buffer is empty (Wait for next frame)
  }

  *frame_ptr = rb->buffer[tail];
  size_t next_tail = (tail + 1) & (ring_buffer_size - 1);
  atomic_store_explicit(&rb->tail, next_tail, memory_order_release);
  return true;
}
