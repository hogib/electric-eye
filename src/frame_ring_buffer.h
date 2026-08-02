#pragma once
#include <pthread.h>
#include <semaphore.h>
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

  // filled/empty are pure wakeup signals layered on top of the lock-free
  // head/tail scheme above -- they never gate correctness, only whether a
  // waiter blocks or spins. filled counts pushed-but-not-yet-popped items
  // (posted after a successful push, waited on before a pop); empty counts
  // free slots (posted after a successful pop, waited on before a push).
  // Both start matching a freshly-init'd ring: filled=0, empty=capacity.
  sem_t filled;
  sem_t empty;

  // ring_push() takes this internally, making it safe for more than one
  // thread to push to the same ring -- which the three-ring pipeline
  // actually needs: both producer_loop (on a capture failure) and
  // consumer_loop (on every frame) push back to the shared free-list ring.
  // The original head/tail dance alone assumes a single pusher; two
  // threads racing the same read-check-write on head can corrupt the ring
  // or drop a frame pointer. ring_pop() needs no such lock -- every ring
  // in this pipeline has exactly one reader, so that side stays lock-free.
  pthread_mutex_t push_lock;
} FrameRingBuffer;

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
 * `while (!ring_push(...) && atomic_load(is_running))` pattern). Pass NULL
 * for a call site that must never abandon its frame instead (e.g.
 * returning a frame to the free-list ring) -- mirrors the original's bare
 * `while (!ring_push(...))` with no is_running check at all. Either way,
 * the underlying ring_push()/ring_pop() call is only ever attempted once
 * the semaphore accounting has already guaranteed it will succeed.
 */
bool ring_push_wait(FrameRingBuffer *rb, void *frame_ptr,
                    const atomic_bool *is_running);

bool ring_pop_wait(FrameRingBuffer *rb, void **frame_ptr,
                   const atomic_bool *is_running);
