// _POSIX_C_SOURCE before any header include: -std=c23 puts glibc in strict
// ISO mode, which hides write()/STDOUT_FILENO in <unistd.h> unless a
// feature-test macro asks for them explicitly. Same guard as
// video_threads.c/config.c.
#define _POSIX_C_SOURCE 200809L

#include "config.h"
#include "frame_ring_buffer.h"
#include "video_frame.h"
#include "video_threads.h"
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <threads.h>
#include <unistd.h>

constexpr uint32_t frame_width = 1280;
constexpr uint32_t frame_height = 720;

FrameRingBuffer ring_buffer_in;
FrameRingBuffer ring_buffer_out;
FrameRingBuffer ring_buffer_free;
atomic_bool is_running = true;

// atomic_store() on a lock-free type and write() are among the few
// operations POSIX guarantees are safe to call from a signal handler --
// printf is not one of them, since it can deadlock if the signal lands
// while the interrupted code already holds stdio's internal lock. This
// guarantee only holds because atomic_bool is required to be lock-free
// here: a lock-based atomic would mean this handler could call into a
// mutex from arbitrary interrupted context, which is not signal-safe.
static_assert(ATOMIC_BOOL_LOCK_FREE == 2,
             "is_running must be lock-free to set from a signal handler");

static void handle_shutdown_signal(int sig) {
  (void)sig;
  static const char msg[] = "\nShutting down...\n";
  write(STDOUT_FILENO, msg, sizeof(msg) - 1);
  atomic_store(&is_running, false);
}

/*
 * .filename is the V4L2 camera device (MJPEG capture -- see v4l2_in.h).
 * .outpath is the v4l2loopback virtual camera device.
 *
 * ffmpeg is no longer required at build or run time; both ends of the
 * pipeline talk directly to their /dev/videoN devices now.
 *
 * One-time host setup required:
 *   sudo apt install v4l2loopback-dkms libturbojpeg0-dev
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
  // Nothing in this pipeline talks to a pipe or socket anymore (both
  // producer_loop and consumer_loop are direct V4L2 device I/O now), but
  // stdout can still be one if the user runs `./eeye | something` and that
  // reader exits -- including the signal handler's own write() below.
  // Ignoring SIGPIPE means that shows up as a normal write() failure
  // instead of killing the process outright.
  signal(SIGPIPE, SIG_IGN);

  // SIGINT: Ctrl+C from an interactive terminal. SIGTERM: what a service
  // manager (systemd, etc.) sends on stop/restart -- handling both the same
  // way means "graceful exit" holds whether this runs at a terminal or as a
  // deployed service. Both threads and the config watcher already poll
  // is_running on a bounded interval, so flipping it is the entire job; the
  // existing join/close/free sequence below is unchanged and runs exactly
  // as it does on any other shutdown path.
  signal(SIGINT, handle_shutdown_signal);
  signal(SIGTERM, handle_shutdown_signal);

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
