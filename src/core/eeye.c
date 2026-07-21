#include "frame_ring_buffer.h"
#include "video_frame.h"
#include "video_threads.h"
#include <pthread.h>
#include <stdio.h>
#include <threads.h>

constexpr uint32_t frame_width = 1920;
constexpr uint32_t frame_height = 1080;

FrameRingBuffer ring_buffer_in;
FrameRingBuffer ring_buffer_out;


atomic_bool is_running = true;

ProducerArgs prod_args = {.filename = "input.yuv",
                          .is_running = &is_running,
                          .ring_buffer_in = &ring_buffer_in,
                          .frame_width = frame_width,
                          .frame_height = frame_height};
WorkerArgs work_args = {
    .is_running = &is_running,
    .ring_buffer_in = &ring_buffer_in,
    .ring_buffer_out = &ring_buffer_out,
    .frame_width = frame_width,
    .frame_height = frame_height,
};

ConsumerArgs cons_args = {
    .outpath = "output.yuv",
    .is_running = &is_running,
    .ring_buffer_out = &ring_buffer_out,
    .frame_width = frame_width,
    .frame_height = frame_height,
};

int main(void) {
  printf("Initializing pipeline...\n");

  ring_init(&ring_buffer_in);
  ring_init(&ring_buffer_out);

  atomic_store(&is_running, true);

  pthread_t producer, worker, consumer;

  if (pthread_create(&producer, NULL, producer_loop, &prod_args) != 0) {
    printf("Error: Failed to create producer thread.\n");
    return 1;
  }
  if (pthread_create(&worker, NULL, effects_loop, &work_args) != 0) {
    printf("Error: Failed to create worker thread.\n");
    return 1;
  }
  if (pthread_create(&consumer, NULL, consumer_loop, &cons_args) != 0) {
    printf("Error: Failed to create consumer thread.\n");
    return 1;
  }

  printf("Pipeline running. Processing video file...\n");

  pthread_join(producer, NULL);
  pthread_join(worker, NULL);
  pthread_join(consumer, NULL);

  printf("Processing complete. Output saved and pipeline shut down cleanly.\n");

  return 0;
}
