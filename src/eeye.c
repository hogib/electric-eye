#include "config.h"
#include "frame_ring_buffer.h"
#include "video_frame.h"
#include "video_threads.h"
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <threads.h>

constexpr uint32_t frame_width = 1280;
constexpr uint32_t frame_height = 720;

FrameRingBuffer ring_buffer_in;
FrameRingBuffer ring_buffer_out;
FrameRingBuffer ring_buffer_free;
atomic_bool is_running = true;

/*
 * .filename now holds the v4l2 camera device instead of an input file path.
 * .outpath now holds the v4l2loopback virtual camera device.
 *
 * One-time host setup required:
 *   sudo apt install v4l2loopback-dkms ffmpeg
 *   sudo modprobe v4l2loopback video_nr=10 card_label="VirtualCam" exclusive_caps=1
 *
 * Confirm your real camera's device number with:
 *   v4l2-ctl --list-devices
 */
ProducerArgs prod_args = {.filename = "/dev/video0",
                          .is_running = &is_running,
                          .ring_buffer_in = &ring_buffer_in,
                          .ring_buffer_free = &ring_buffer_free,
                          .frame_width = frame_width,
                          .frame_height = frame_height};

// .config is filled in by main() once config_watch_start() has run --
// building the watcher needs is_running, which isn't available until then.
WorkerArgs work_args = {
    .is_running = &is_running,
    .ring_buffer_in = &ring_buffer_in,
    .ring_buffer_out = &ring_buffer_out,
    .frame_width = frame_width,
    .frame_height = frame_height,
};

ConsumerArgs cons_args = {
    .outpath = "/dev/video10",
    .is_running = &is_running,
    .ring_buffer_out = &ring_buffer_out,
    .ring_buffer_free = &ring_buffer_free,
    .frame_width = frame_width,
    .frame_height = frame_height,
};

int main(int argc, char **argv) {
  // If the output ffmpeg (virtual cam) process dies/exits, writes to its pipe
  // would otherwise raise SIGPIPE and kill this whole process. Ignore it and
  // let consumer_loop's ferror(outfile) check handle shutdown gracefully.
  signal(SIGPIPE, SIG_IGN);

  const char *config_path = (argc > 1) ? argv[1] : "eeye_config.json";

  printf("Initializing pipeline...\n");
  ring_init(&ring_buffer_in);
  ring_init(&ring_buffer_out);
  ring_init(&ring_buffer_free);

  const unsigned int pool_size = ring_buffer_size - 1;
  VideoFrame **pool = vf_pool_create(pool_size, frame_width, frame_height);
  if (!pool) {
    printf("Error: Failed to allocate frame pool.\n");
    return 1;
  }

  for (unsigned int i = 0; i < pool_size; ++i) {
    ring_push(&ring_buffer_free, pool[i]);
  }

  atomic_store(&is_running, true);

  ConfigWatcher *config_watcher = config_watch_start(config_path, &is_running);
  if (!config_watcher) {
    printf("Error: Failed to start config watcher.\n");
    return 1;
  }
  work_args.config = config_watcher;

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

  printf("Pipeline running. Capturing from camera -> virtual cam...\n");
  printf("Point another app (browser, Zoom, etc.) at the virtual camera "
         "device to view the live feed.\n");

  pthread_join(producer, NULL);
  pthread_join(worker, NULL);
  pthread_join(consumer, NULL);

  printf("Processing complete. Pipeline shut down cleanly.\n");
  config_watch_stop(config_watcher);
  vf_pool_free(pool, pool_size);
  return 0;
}
