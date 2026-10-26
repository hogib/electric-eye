// _POSIX_C_SOURCE before any header include: -std=c99 puts glibc in strict
// ISO mode, which hides clock_gettime() and pthread_condattr_setclock()
// unless a feature-test macro asks for them explicitly.
#define _POSIX_C_SOURCE 200809L
#include "frame_ring_buffer.h"
#include <assert.h>
#include <stddef.h>
#include <time.h>

// Bound on how long a *_wait() call blocks before re-checking is_running.
// 200ms matches config.c's inotify-thread poll timeout, so a shutdown
// request is noticed on the same order of magnitude everywhere in this
// codebase. In the steady state (frames flowing at 30fps) this bound is
// essentially never hit -- the real post from the other side of the ring
// arrives within a frame interval, tens of milliseconds at most.
static const long ring_wait_timeout_ns = 200000000;

// Deadlines are on CLOCK_MONOTONIC (hence MonoSem rather than sem_t, whose
// sem_timedwait only takes CLOCK_REALTIME): a Pi has no RTC, so its wall
// clock can be stepped by timesyncd at any point, and a backwards step
// would stretch a wait that is supposed to notice shutdown in 200ms.
static void add_timeout(struct timespec *ts) {
  clock_gettime(CLOCK_MONOTONIC, ts);
  ts->tv_nsec += ring_wait_timeout_ns;
  if (ts->tv_nsec >= 1000000000) {
    ts->tv_sec += 1;
    ts->tv_nsec -= 1000000000;
  }
}

static void monosem_init(MonoSem *s, unsigned int count) {
  pthread_condattr_t attr;
  pthread_condattr_init(&attr);
  pthread_condattr_setclock(&attr, CLOCK_MONOTONIC);
  pthread_cond_init(&s->cond, &attr);
  pthread_condattr_destroy(&attr);
  pthread_mutex_init(&s->lock, NULL);
  s->count = count;
}

static void monosem_destroy(MonoSem *s) {
  pthread_cond_destroy(&s->cond);
  pthread_mutex_destroy(&s->lock);
}

static void monosem_post(MonoSem *s) {
  pthread_mutex_lock(&s->lock);
  s->count++;
  pthread_cond_signal(&s->cond);
  pthread_mutex_unlock(&s->lock);
}

// 0 once a unit is taken, ETIMEDOUT if deadline (CLOCK_MONOTONIC) passes
// first. Unlike sem_t, never fails with EINTR: pthread_cond_timedwait
// doesn't.
static int monosem_timedwait(MonoSem *s, const struct timespec *deadline) {
  int r = 0;
  pthread_mutex_lock(&s->lock);
  while (s->count == 0 && r == 0)
    r = pthread_cond_timedwait(&s->cond, &s->lock, deadline);
  // A post can land between the timeout and reacquiring the lock; take it
  // rather than report a timeout with a unit sitting there.
  if (s->count > 0) {
    s->count--;
    r = 0;
  }
  pthread_mutex_unlock(&s->lock);
  return r;
}

void ring_init(FrameRingBuffer *rb) {
  rb->head = 0;
  rb->tail = 0;
  monosem_init(&rb->filled, 0);
  monosem_init(&rb->empty, ring_buffer_size - 1);
  pthread_mutex_init(&rb->push_lock, NULL);
  rb->high_water = 0;
  rb->full_stalls = 0;
  rb->empty_stalls = 0;
}

void ring_destroy(FrameRingBuffer *rb) {
  monosem_destroy(&rb->filled);
  monosem_destroy(&rb->empty);
  pthread_mutex_destroy(&rb->push_lock);
}

// Safe for any number of concurrent callers (see push_lock's comment in the
// header); ring_pop() below is not and does not need to be -- every ring in
// this pipeline has exactly one reader.
bool ring_push(FrameRingBuffer *rb, void *frame_ptr) {
  pthread_mutex_lock(&rb->push_lock);

  size_t head = __atomic_load_n(&rb->head, __ATOMIC_RELAXED);
  size_t tail = __atomic_load_n(&rb->tail, __ATOMIC_ACQUIRE);
  size_t next_head = (head + 1) & (ring_buffer_size - 1);

  bool ok = (next_head != tail); // false: buffer is full (frame drop!)
  if (ok) {
    rb->buffer[head] = frame_ptr;
    __atomic_store_n(&rb->head, next_head, __ATOMIC_RELEASE);

    // high_water diagnostics: occupancy just after this push. push_lock
    // (held for the whole function) already serializes every pusher, so
    // this load-compare-store is race-free without any extra atomicity.
    size_t occupancy = (next_head - tail) & (ring_buffer_size - 1);
    size_t prev_high = __atomic_load_n(&rb->high_water, __ATOMIC_RELAXED);
    if (occupancy > prev_high) {
      __atomic_store_n(&rb->high_water, occupancy, __ATOMIC_RELAXED);
    }
  }

  pthread_mutex_unlock(&rb->push_lock);
  return ok;
}

// Called only by the CONSUMER thread
bool ring_pop(FrameRingBuffer *rb, void **frame_ptr) {
  size_t tail = __atomic_load_n(&rb->tail, __ATOMIC_RELAXED);
  size_t head = __atomic_load_n(&rb->head, __ATOMIC_ACQUIRE);

  if (tail == head) {
    return false; // Buffer is empty (Wait for next frame)
  }

  *frame_ptr = rb->buffer[tail];
  size_t next_tail = (tail + 1) & (ring_buffer_size - 1);
  __atomic_store_n(&rb->tail, next_tail, __ATOMIC_RELEASE);
  return true;
}

bool ring_push_wait(FrameRingBuffer *rb, void *frame_ptr,
                    const bool *is_running) {
  for (;;) {
    struct timespec deadline;
    add_timeout(&deadline);

    if (monosem_timedwait(&rb->empty, &deadline) == 0)
      break; // a free slot is reserved for us -- push below cannot fail
    // ETIMEDOUT (the expected case while genuinely waiting) or any other
    // unexpected error: either way, only stop retrying if told to give up.
    // One increment per ~200ms window actually spent waiting here (not per
    // call) -- see RingStats' comment in the header.
    __atomic_fetch_add(&rb->full_stalls, 1, __ATOMIC_RELAXED);
    if (is_running && !__atomic_load_n(is_running, __ATOMIC_SEQ_CST))
      return false;
  }

  bool pushed = ring_push(rb, frame_ptr);
  // The semaphore accounting guarantees this: we hold a slot reservation
  // that only this call can consume, so ring_push() finding no room would
  // mean empty/head/tail have gone out of sync with each other -- a bug in
  // this file, not a real "buffer full" condition.
  assert(pushed);
  (void)pushed; // avoid an unused-variable warning when NDEBUG drops the assert

  monosem_post(&rb->filled);
  return true;
}

bool ring_pop_wait(FrameRingBuffer *rb, void **frame_ptr,
                   const bool *is_running) {
  for (;;) {
    struct timespec deadline;
    add_timeout(&deadline);

    if (monosem_timedwait(&rb->filled, &deadline) == 0)
      break; // an item is reserved for us -- pop below cannot fail
    __atomic_fetch_add(&rb->empty_stalls, 1, __ATOMIC_RELAXED);
    if (is_running && !__atomic_load_n(is_running, __ATOMIC_SEQ_CST))
      return false;
  }

  bool popped = ring_pop(rb, frame_ptr);
  assert(popped); // see the matching comment in ring_push_wait()
  (void)popped;

  monosem_post(&rb->empty);
  return true;
}

void ring_get_stats(const FrameRingBuffer *rb, RingStats *out) {
  size_t head = __atomic_load_n(&rb->head, __ATOMIC_ACQUIRE);
  size_t tail = __atomic_load_n(&rb->tail, __ATOMIC_ACQUIRE);

  out->capacity = ring_buffer_size - 1;
  out->occupancy = (head - tail) & (ring_buffer_size - 1);
  out->high_water = __atomic_load_n(&rb->high_water, __ATOMIC_RELAXED);
  out->full_stalls = __atomic_load_n(&rb->full_stalls, __ATOMIC_RELAXED);
  out->empty_stalls = __atomic_load_n(&rb->empty_stalls, __ATOMIC_RELAXED);
}
