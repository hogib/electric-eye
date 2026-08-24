// The frame ring buffers are the pipeline's backbone: every frame passes
// through three of them. A lost pointer leaks a frame out of the pool
// permanently (the pool is fixed-size and never refilled), and a duplicated
// one means two threads write the same buffer at once. Both are silent.
#define _POSIX_C_SOURCE 200809L
#include "frame_ring_buffer.c"
#include "test_harness.h"
#include <pthread.h>

// Distinct non-NULL pointers to push; the ring only ever stores void*, so
// their targets don't matter.
static int slots[1024];

static void test_starts_empty(void) {
  FrameRingBuffer rb;
  ring_init(&rb);
  void *out = NULL;
  CHECK(!ring_pop(&rb, &out));
  RingStats s;
  ring_get_stats(&rb, &s);
  CHECK_EQ_INT(s.occupancy, 0);
  CHECK_EQ_INT(s.capacity, ring_buffer_size - 1);
  ring_destroy(&rb);
}

static void test_fifo_order(void) {
  FrameRingBuffer rb;
  ring_init(&rb);
  for (int i = 0; i < 10; i++)
    CHECK(ring_push(&rb, &slots[i]));
  for (int i = 0; i < 10; i++) {
    void *out = NULL;
    CHECK(ring_pop(&rb, &out));
    CHECK(out == &slots[i]); // strict FIFO -- frames must not reorder
  }
  ring_destroy(&rb);
}

// One slot is deliberately kept empty to distinguish full from empty, so
// usable capacity is size-1. Pushing past that must fail rather than
// overwrite an unread entry.
static void test_capacity_limit_refuses_overwrite(void) {
  FrameRingBuffer rb;
  ring_init(&rb);
  size_t usable = ring_buffer_size - 1;
  for (size_t i = 0; i < usable; i++)
    CHECK(ring_push(&rb, &slots[i]));
  CHECK(!ring_push(&rb, &slots[usable])); // full

  // Everything pushed is still retrievable, in order and unclobbered.
  for (size_t i = 0; i < usable; i++) {
    void *out = NULL;
    CHECK(ring_pop(&rb, &out));
    CHECK(out == &slots[i]);
  }
  CHECK(!ring_pop(&rb, &(void *){NULL}));
  ring_destroy(&rb);
}

// Indices wrap modulo the buffer size; a wrap must not corrupt ordering or
// lose entries. Cycles far past one wrap to catch index arithmetic errors.
static void test_wraparound_preserves_order(void) {
  FrameRingBuffer rb;
  ring_init(&rb);
  for (int cycle = 0; cycle < 10; cycle++) {
    for (int i = 0; i < 20; i++)
      CHECK(ring_push(&rb, &slots[i]));
    for (int i = 0; i < 20; i++) {
      void *out = NULL;
      CHECK(ring_pop(&rb, &out));
      CHECK(out == &slots[i]);
    }
  }
  ring_destroy(&rb);
}

static void test_stats_track_occupancy_and_high_water(void) {
  FrameRingBuffer rb;
  ring_init(&rb);
  RingStats s;

  for (int i = 0; i < 5; i++)
    ring_push(&rb, &slots[i]);
  ring_get_stats(&rb, &s);
  CHECK_EQ_INT(s.occupancy, 5);
  CHECK_EQ_INT(s.high_water, 5);

  for (int i = 0; i < 3; i++)
    ring_pop(&rb, &(void *){NULL});
  ring_get_stats(&rb, &s);
  CHECK_EQ_INT(s.occupancy, 2);
  CHECK_EQ_INT(s.high_water, 5); // high_water is a peak, it never decreases

  for (int i = 0; i < 6; i++)
    ring_push(&rb, &slots[i]);
  ring_get_stats(&rb, &s);
  CHECK_EQ_INT(s.occupancy, 8);
  CHECK_EQ_INT(s.high_water, 8);
  ring_destroy(&rb);
}

// The free-list ring genuinely has two concurrent pushers (producer_loop on
// capture failure, consumer_loop on every frame), which is why ring_push()
// takes a lock. Every pushed pointer must come back exactly once: a lost
// one leaks a frame from the fixed pool, a duplicated one means two threads
// writing the same buffer.
#define PUSHERS 4
#define PER_PUSHER 2000

typedef struct {
  FrameRingBuffer *rb;
  int id;
} PusherArg;

static void *pusher_thread(void *arg) {
  PusherArg *a = (PusherArg *)arg;
  for (int i = 0; i < PER_PUSHER; i++) {
    // Encode (pusher, index) in the pointer so the consumer can verify
    // exact multiplicity without needing the pointers to be distinct
    // objects.
    void *p = (void *)(uintptr_t)(a->id * PER_PUSHER + i + 1);
    while (!ring_push(a->rb, p))
      sched_yield(); // ring full: the single consumer will drain it
  }
  return NULL;
}

static void test_concurrent_pushers_lose_nothing(void) {
  FrameRingBuffer rb;
  ring_init(&rb);

  pthread_t threads[PUSHERS];
  PusherArg args[PUSHERS];
  for (int i = 0; i < PUSHERS; i++) {
    args[i] = (PusherArg){.rb = &rb, .id = i};
    pthread_create(&threads[i], NULL, pusher_thread, &args[i]);
  }

  // One consumer, matching the pipeline: every ring has exactly one reader.
  static uint8_t seen[PUSHERS * PER_PUSHER + 1];
  memset(seen, 0, sizeof seen);
  int received = 0;
  while (received < PUSHERS * PER_PUSHER) {
    void *out = NULL;
    if (ring_pop(&rb, &out)) {
      uintptr_t v = (uintptr_t)out;
      CHECK(v >= 1 && v <= PUSHERS * PER_PUSHER);
      if (v >= 1 && v <= PUSHERS * PER_PUSHER) {
        seen[v]++;
        CHECK_EQ_INT(seen[v], 1); // never delivered twice
      }
      received++;
    } else {
      sched_yield();
    }
  }

  for (int i = 0; i < PUSHERS; i++)
    pthread_join(threads[i], NULL);

  CHECK_EQ_INT(received, PUSHERS * PER_PUSHER);
  int missing = 0;
  for (int v = 1; v <= PUSHERS * PER_PUSHER; v++)
    if (seen[v] != 1)
      missing++;
  CHECK_EQ_INT(missing, 0);

  void *leftover = NULL;
  CHECK(!ring_pop(&rb, &leftover)); // fully drained
  ring_destroy(&rb);
}

// ring_pop_wait() must return false rather than block forever once
// is_running goes false -- this is what lets shutdown actually finish.
static void test_pop_wait_returns_on_shutdown(void) {
  FrameRingBuffer rb;
  ring_init(&rb);
  atomic_bool running = false; // already shutting down
  void *out = NULL;
  CHECK(!ring_pop_wait(&rb, &out, &running));
  ring_destroy(&rb);
}

// A NULL is_running means "must not be abandoned" -- used for pushes back
// to the free list, which have to complete even during shutdown or the
// frame leaks out of the pool.
static void test_push_wait_with_null_flag_completes(void) {
  FrameRingBuffer rb;
  ring_init(&rb);
  CHECK(ring_push_wait(&rb, &slots[0], NULL));
  void *out = NULL;
  CHECK(ring_pop(&rb, &out));
  CHECK(out == &slots[0]);
  ring_destroy(&rb);
}

int main(void) {
  printf("test_ring_buffer:\n");
  RUN_TEST(test_starts_empty);
  RUN_TEST(test_fifo_order);
  RUN_TEST(test_capacity_limit_refuses_overwrite);
  RUN_TEST(test_wraparound_preserves_order);
  RUN_TEST(test_stats_track_occupancy_and_high_water);
  RUN_TEST(test_concurrent_pushers_lose_nothing);
  RUN_TEST(test_pop_wait_returns_on_shutdown);
  RUN_TEST(test_push_wait_with_null_flag_completes);
  TEST_MAIN_END();
}
