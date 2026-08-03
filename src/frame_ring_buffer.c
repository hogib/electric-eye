#include "frame_ring_buffer.h"
#include <assert.h>
#include <errno.h>
#include <stddef.h>
#include <time.h>

// Bound on how long a *_wait() call blocks before re-checking is_running.
// 200ms matches config.c's inotify-thread poll timeout, so a shutdown
// request is noticed on the same order of magnitude everywhere in this
// codebase. In the steady state (frames flowing at 30fps) this bound is
// essentially never hit -- the real sem_post from the other side of the
// ring arrives within a frame interval, tens of milliseconds at most.
static constexpr long ring_wait_timeout_ns = 200000000;

static void add_timeout(struct timespec *ts) {
  clock_gettime(CLOCK_REALTIME, ts);
  ts->tv_nsec += ring_wait_timeout_ns;
  if (ts->tv_nsec >= 1000000000) {
    ts->tv_sec += 1;
    ts->tv_nsec -= 1000000000;
  }
}

void ring_init(FrameRingBuffer *rb) {
  atomic_init(&rb->head, 0);
  atomic_init(&rb->tail, 0);
  sem_init(&rb->filled, 0, 0);
  sem_init(&rb->empty, 0, ring_buffer_size - 1);
  pthread_mutex_init(&rb->push_lock, NULL);
  atomic_init(&rb->high_water, 0);
  atomic_init(&rb->full_stalls, 0);
  atomic_init(&rb->empty_stalls, 0);
}

void ring_destroy(FrameRingBuffer *rb) {
  sem_destroy(&rb->filled);
  sem_destroy(&rb->empty);
  pthread_mutex_destroy(&rb->push_lock);
}

// Safe for any number of concurrent callers (see push_lock's comment in the
// header); ring_pop() below is not and does not need to be -- every ring in
// this pipeline has exactly one reader.
bool ring_push(FrameRingBuffer *rb, void *frame_ptr) {
  pthread_mutex_lock(&rb->push_lock);

  size_t head = atomic_load_explicit(&rb->head, memory_order_relaxed);
  size_t tail = atomic_load_explicit(&rb->tail, memory_order_acquire);
  size_t next_head = (head + 1) & (ring_buffer_size - 1);

  bool ok = (next_head != tail); // false: buffer is full (frame drop!)
  if (ok) {
    rb->buffer[head] = frame_ptr;
    atomic_store_explicit(&rb->head, next_head, memory_order_release);

    // high_water diagnostics: occupancy just after this push. push_lock
    // (held for the whole function) already serializes every pusher, so
    // this load-compare-store is race-free without any extra atomicity.
    size_t occupancy = (next_head - tail) & (ring_buffer_size - 1);
    size_t prev_high = atomic_load_explicit(&rb->high_water, memory_order_relaxed);
    if (occupancy > prev_high) {
      atomic_store_explicit(&rb->high_water, occupancy, memory_order_relaxed);
    }
  }

  pthread_mutex_unlock(&rb->push_lock);
  return ok;
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

bool ring_push_wait(FrameRingBuffer *rb, void *frame_ptr,
                    const atomic_bool *is_running) {
  for (;;) {
    struct timespec deadline;
    add_timeout(&deadline);

    int r = sem_timedwait(&rb->empty, &deadline);
    if (r == 0)
      break; // a free slot is reserved for us -- push below cannot fail
    if (errno == EINTR)
      continue;
    // ETIMEDOUT (the expected case while genuinely waiting) or any other
    // unexpected errno: either way, only stop retrying if told to give up.
    // One increment per ~200ms window actually spent waiting here (not per
    // call) -- see RingStats' comment in the header.
    atomic_fetch_add_explicit(&rb->full_stalls, 1, memory_order_relaxed);
    if (is_running && !atomic_load(is_running))
      return false;
  }

  bool pushed = ring_push(rb, frame_ptr);
  // The semaphore accounting guarantees this: we hold a slot reservation
  // that only this call can consume, so ring_push() finding no room would
  // mean empty/head/tail have gone out of sync with each other -- a bug in
  // this file, not a real "buffer full" condition.
  assert(pushed);
  (void)pushed; // avoid an unused-variable warning when NDEBUG drops the assert

  sem_post(&rb->filled);
  return true;
}

bool ring_pop_wait(FrameRingBuffer *rb, void **frame_ptr,
                   const atomic_bool *is_running) {
  for (;;) {
    struct timespec deadline;
    add_timeout(&deadline);

    int r = sem_timedwait(&rb->filled, &deadline);
    if (r == 0)
      break; // an item is reserved for us -- pop below cannot fail
    if (errno == EINTR)
      continue;
    atomic_fetch_add_explicit(&rb->empty_stalls, 1, memory_order_relaxed);
    if (is_running && !atomic_load(is_running))
      return false;
  }

  bool popped = ring_pop(rb, frame_ptr);
  assert(popped); // see the matching comment in ring_push_wait()
  (void)popped;

  sem_post(&rb->empty);
  return true;
}

void ring_get_stats(const FrameRingBuffer *rb, RingStats *out) {
  size_t head = atomic_load_explicit(&rb->head, memory_order_acquire);
  size_t tail = atomic_load_explicit(&rb->tail, memory_order_acquire);

  out->capacity = ring_buffer_size - 1;
  out->occupancy = (head - tail) & (ring_buffer_size - 1);
  out->high_water = atomic_load_explicit(&rb->high_water, memory_order_relaxed);
  out->full_stalls = atomic_load_explicit(&rb->full_stalls, memory_order_relaxed);
  out->empty_stalls = atomic_load_explicit(&rb->empty_stalls, memory_order_relaxed);
}
