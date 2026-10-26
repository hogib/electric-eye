#pragma once
#include "compat.h"
#include <pthread.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

enum { ring_buffer_size = 32 };

STATIC_ASSERT((ring_buffer_size & (ring_buffer_size - 1)) == 0,
              "ring_buffer_size must be a power of two!");

// A counting semaphore whose timed wait runs on CLOCK_MONOTONIC. A POSIX
// sem_t can only time out against CLOCK_REALTIME (sem_clockwait, the
// monotonic variant, is glibc 2.30+), so this is the classic mutex+condvar
// build of one, with the condvar's clock set to monotonic --
// pthread_condattr_setclock() has been in glibc since 2.3.3.
typedef struct {
  pthread_mutex_t lock;
  pthread_cond_t cond;
  unsigned int count;
} MonoSem;

// Every size_t/uint_fast64_t field below, and *is_running in the wait
// functions, is atomic: only ever accessed through the __atomic builtins
// (see compat.h).
typedef struct FrameRingBuffer {
  void *buffer[ring_buffer_size];
  size_t head; // Written by Producer
  size_t tail; // Written by Consumer

  // filled/empty are pure wakeup signals layered on top of the lock-free
  // head/tail scheme above -- they never gate correctness, only whether a
  // waiter blocks or spins. filled counts pushed-but-not-yet-popped items
  // (posted after a successful push, waited on before a pop); empty counts
  // free slots (posted after a successful pop, waited on before a push).
  // Both start matching a freshly-init'd ring: filled=0, empty=capacity.
  MonoSem filled;
  MonoSem empty;

  // ring_push() takes this internally, making it safe for more than one
  // thread to push to the same ring -- which the three-ring pipeline
  // actually needs: both producer_loop (on a capture failure) and
  // consumer_loop (on every frame) push back to the shared free-list ring.
  // The original head/tail dance alone assumes a single pusher; two
  // threads racing the same read-check-write on head can corrupt the ring
  // or drop a frame pointer. ring_pop() needs no such lock -- every ring
  // in this pipeline has exactly one reader, so that side stays lock-free.
  pthread_mutex_t push_lock;

  // Diagnostics only -- never read by anything that gates correctness, so
  // every access uses relaxed ordering. high_water is updated from inside
  // ring_push() under push_lock, which already serializes every pusher, so
  // a plain load-compare-store there is race-free with no extra locking.
  // full_stalls/empty_stalls are plain atomic increments instead, since
  // ring_push_wait() itself isn't lock-protected before it calls
  // ring_push() -- and more than one thread can be waiting to push to the
  // same ring at once (again, the shared free-list ring).
  size_t high_water;          // largest occupancy ever observed on a push
  uint_fast64_t full_stalls;  // ring_push_wait() timeouts (ring full)
  uint_fast64_t empty_stalls; // ring_pop_wait() timeouts (ring empty)
} FrameRingBuffer;

// Snapshot of one ring's diagnostics, for logging/tuning -- see
// ring_get_stats() below. Not used for any correctness decision.
typedef struct {
  size_t capacity;  // usable slots (ring_buffer_size - 1; one slot is
                    // always kept empty to distinguish full from empty)
  size_t occupancy; // filled slots as of this call
  size_t high_water; // largest occupancy ever observed on a push

  // How many separate ~200ms wait-timeout windows a push/pop spent
  // genuinely blocked (not how many times a caller blocked overall) --
  // multiply by the wait timeout for a rough lower bound on total time
  // spent stalled. In steady-state operation at the pipeline's target
  // framerate, both should stay at or near zero: a real, sustained count
  // means this ring's producer or consumer can't keep up.
  uint_fast64_t full_stalls;  // ring was full when a push wanted in
  uint_fast64_t empty_stalls; // ring was empty when a pop wanted out
} RingStats;

void ring_init(FrameRingBuffer *rb);

// Releases the semaphores/mutex ring_init() created. The lock-free
// head/tail fields need no teardown, but real OS objects were added
// alongside them and do.
void ring_destroy(FrameRingBuffer *rb);

bool ring_push(FrameRingBuffer *rb, void *frame_ptr);

bool ring_pop(FrameRingBuffer *rb, void **frame_ptr);

/*
 * Blocking counterparts to ring_push()/ring_pop(): instead of the caller
 * busy-sleeping and retrying on failure, these block on the ring's
 * semaphore until a slot/item is genuinely available (bounded internally,
 * so a shutdown request is still noticed promptly -- see is_running below),
 * then perform the push/pop, which is now guaranteed to succeed.
 *
 * is_running: if non-NULL, the wait gives up and returns false once
 * *is_running becomes false while still waiting -- for a call site that
 * should abandon its attempt on shutdown (mirrors the original code's
 * `while (!ring_push(...) && is_running)` pattern). Pass NULL
 * for a call site that must never abandon its frame instead (e.g.
 * returning a frame to the free-list ring) -- mirrors the original's bare
 * `while (!ring_push(...))` with no is_running check at all. Either way,
 * the underlying ring_push()/ring_pop() call is only ever attempted once
 * the semaphore accounting has already guaranteed it will succeed.
 */
bool ring_push_wait(FrameRingBuffer *rb, void *frame_ptr,
                    const bool *is_running);

bool ring_pop_wait(FrameRingBuffer *rb, void **frame_ptr,
                   const bool *is_running);

// Fills *out with a point-in-time snapshot of rb's diagnostics. Safe to call
// from any thread at any time -- every field it reads is atomic, and none
// of them gate correctness.
void ring_get_stats(const FrameRingBuffer *rb, RingStats *out);
