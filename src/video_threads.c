#include "video_threads.h"
#include "effect_chain.h"
#include "frame_ring_buffer.h"
#include "v4l2_in.h"
#include "v4l2_out.h"
#include "video_frame.h"
#include <errno.h>
#include <linux/videodev2.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <threads.h>

// Best-effort request passed to v4l2_in_open(); V4L2 does not guarantee a
// driver honors an exact framerate. args->filename (ProducerArgs) is the
// camera device, e.g. "/dev/video0".
#define CAMERA_FRAMERATE 30
#define CAMERA_RECONNECT_DELAY_MS 2000
// Camera-missing heartbeat: log immediately on the first failed open, then
// only every 15th retry (~30s at the 2s delay above) instead of every
// single one, so a camera left unplugged for hours doesn't flood the log
// while the daemon keeps quietly retrying underneath.
#define CAMERA_RECONNECT_LOG_EVERY 15
#define STATS_LOG_INTERVAL_MS 5000

void *producer_loop(void *arg) {
  ProducerArgs *args = (ProducerArgs *)arg;
  int64_t pts = 0;
  int reconnect_attempt = 0;

  // Outer loop: own the camera device for as long as it stays usable, then
  // let go and retry. This covers both "no camera yet at startup" (USB
  // enumeration can lag process start) and "camera disappeared mid-session"
  // (unplugged, power-cycled, USB glitch) with the same mechanism -- either
  // way the fix is the same: close whatever handle we have, wait, try
  // v4l2_in_open() again. effects_loop and consumer_loop stay up the whole
  // time; an empty ring_buffer_in is already something they handle by
  // idling, so the rest of the pipeline just pauses rather than dying.
  //
  // This intentionally does NOT extend to consumer_loop's v4l2loopback
  // device: a missing v4l2loopback kernel module is a setup problem no
  // amount of retrying open() fixes, unlike a USB camera that may
  // genuinely come back. That class of failure is left to exit the process
  // and rely on the systemd unit's Restart= for recovery instead.
  while (atomic_load(args->is_running)) {
    V4l2In *in = v4l2_in_open(args->filename, args->frame_width,
                              args->frame_height, CAMERA_FRAMERATE);
    if (!in) {
      reconnect_attempt++;
      if (reconnect_attempt == 1 ||
          reconnect_attempt % CAMERA_RECONNECT_LOG_EVERY == 0) {
        printf("Camera unavailable (attempt %d); retrying every %ds...\n",
               reconnect_attempt, CAMERA_RECONNECT_DELAY_MS / 1000);
      }
      for (int waited_ms = 0;
          waited_ms < CAMERA_RECONNECT_DELAY_MS && atomic_load(args->is_running);
          waited_ms += 100) {
        sleep_us(100000); // 100ms chunks so shutdown is noticed promptly
      }
      continue;
    }

    reconnect_attempt = 0;
    printf("Camera connected: %s\n", args->filename);

    while (atomic_load(args->is_running)) {
      VideoFrame *frame = NULL;

      if (!ring_pop_wait(args->ring_buffer_free, (void **)&frame,
                        args->is_running))
        break; // shutting down while waiting for a free buffer

      frame->pts = pts++;

      if (!v4l2_in_capture(in, frame)) {
        // v4l2_in_capture already retried transient per-frame decode
        // glitches internally; this is either a device-level error or a
        // persistent format mismatch. Fall back to the reconnect loop
        // above rather than ending the pipeline -- if this is a cable
        // wiggle or a brief power drop, replugging fixes it without ever
        // needing a restart.
        printf("Camera capture failed; will attempt to reconnect.\n");
        // NULL: this frame must go back to the pool regardless of
        // is_running -- abandoning it here would leak it out of the pool
        // for the rest of the process's life.
        ring_push_wait(args->ring_buffer_free, frame, NULL);
        break;
      }

      ring_push_wait(args->ring_buffer_in, frame, args->is_running);
    }

    v4l2_in_close(in);
  }

  return NULL;
}

void *effects_loop(void *arg) {
  WorkerArgs *args = (WorkerArgs *)arg;
  while (
      atomic_load(args->is_running) ||
      atomic_load_explicit(&args->ring_buffer_in->head, memory_order_relaxed) !=
          atomic_load_explicit(&args->ring_buffer_in->tail,
                               memory_order_relaxed)) {

    VideoFrame *frame = NULL;

    if (!ring_pop_wait(args->ring_buffer_in, (void **)&frame,
                      args->is_running)) {
      // is_running is false; the loop condition above will end the loop
      // once ring_buffer_in is confirmed drained (or send us back here to
      // wait again, if something was pushed in the interim).
      continue;
    }

    // One snapshot per frame, not per effect: this guarantees a config push
    // landing mid-processing can't split a single output frame between old
    // and new parameters.
    const Config *cfg = config_current(args->config);
    apply_effect_chain(frame, cfg);

    // NULL: a fully-processed frame must reach the consumer regardless of
    // is_running -- abandoning it here would leak it out of the pool.
    ring_push_wait(args->ring_buffer_out, frame, NULL);
  }
  return NULL;
}

void *consumer_loop(void *arg) {
  ConsumerArgs *args = (ConsumerArgs *)arg;

  V4l2Out *out = v4l2_out_open(args->outpath, args->frame_width,
                               args->frame_height, V4L2_PIX_FMT_YUV422P);
  if (!out) {
    atomic_store(args->is_running, false);
    return NULL;
  }

  // Recording tap: writes frame->raw_data -- the untouched camera frame,
  // independent of whatever the effect chain does to the copy sent to the
  // virtual camera -- to record_path whenever the config specifies a
  // non-empty one. No container, just raw I422 bytes back to back; play
  // back with (matching this frame's width/height and CAMERA_FRAMERATE
  // above):
  //   ffplay -f rawvideo -pix_fmt yuv422p -s <width>x<height> -r 30 -i FILE
  //
  // This is genuinely large -- ~53MB/s, ~190GB/hour at 1280x720 -- with no
  // compression. Fine for short clips; anything longer wants an encoder,
  // which is a real follow-up, not this.
  FILE *record_file = NULL;
  char record_path[max_record_path_len] = "";

  while (atomic_load(args->is_running) ||
         atomic_load_explicit(&args->ring_buffer_out->head,
                              memory_order_relaxed) !=
             atomic_load_explicit(&args->ring_buffer_out->tail,
                                  memory_order_relaxed)) {

    VideoFrame *frame = NULL;

    if (!ring_pop_wait(args->ring_buffer_out, (void **)&frame,
                      args->is_running)) {
      continue;
    }

    // Independent config read from effects_loop's -- this frame's chain
    // was already decided by the time it got here, but the recording
    // decision is consumer_loop's own to make. Same once-per-frame
    // snapshot discipline: whatever record_path this frame observes is
    // the one it's judged against for its entire trip through this loop.
    const Config *cfg = config_current(args->config);
    if (strcmp(cfg->record_path, record_path) != 0) {
      if (record_file) {
        fclose(record_file);
        record_file = NULL;
        printf("Recording stopped: %s\n", record_path);
      }
      snprintf(record_path, sizeof record_path, "%s", cfg->record_path);
      if (record_path[0] != '\0') {
        record_file = fopen(record_path, "wb");
        if (record_file) {
          printf("Recording started: %s (raw I422 %ux%u -- play back with: "
                 "ffplay -f rawvideo -pix_fmt yuv422p -s %ux%u -r %d -i %s)\n",
                 record_path, args->frame_width, args->frame_height,
                 args->frame_width, args->frame_height, CAMERA_FRAMERATE,
                 record_path);
        } else {
          printf("Recording failed to start: could not open %s: %s\n",
                 record_path, strerror(errno));
          record_path[0] = '\0'; // don't retry the same failing path every frame
        }
      }
    }

    if (record_file) {
      size_t total = frame->plane_sizes[0] + frame->plane_sizes[1] +
                     frame->plane_sizes[2];
      if (fwrite(frame->raw_data, 1, total, record_file) != total) {
        printf("Recording write failed (%s); stopping recording.\n",
               strerror(errno));
        fclose(record_file);
        record_file = NULL;
        record_path[0] = '\0';
      }
    }

    if (!v4l2_out_write(out, frame)) {
      // The loopback device went away or rejected the frame. Stop the whole
      // pipeline rather than spinning on a dead device.
      printf("Virtual camera write failed; shutting down.\n");
      // NULL: this frame must go back to the pool regardless of
      // is_running (which the very next line is about to clear anyway).
      ring_push_wait(args->ring_buffer_free, frame, NULL);
      atomic_store(args->is_running, false);
      break;
    }

    // NULL: same as every other pushback to the free-list ring -- must not
    // be abandoned on shutdown, or the frame leaks out of the pool. This is
    // the hot path: every single processed frame returns to the pool here.
    ring_push_wait(args->ring_buffer_free, frame, NULL);
  }

  if (record_file)
    fclose(record_file);

  v4l2_out_close(out);
  return NULL;
}

static void log_ring_stats(const char *name, const FrameRingBuffer *rb) {
  RingStats s;
  ring_get_stats(rb, &s);
  printf("  %-24s occ %2zu/%2zu (peak %2zu)  full_stalls %llu  "
        "empty_stalls %llu\n",
        name, s.occupancy, s.capacity, s.high_water,
        (unsigned long long)s.full_stalls, (unsigned long long)s.empty_stalls);
}

void *stats_loop(void *arg) {
  StatsArgs *args = (StatsArgs *)arg;

  while (atomic_load(args->is_running)) {
    for (int waited_ms = 0;
        waited_ms < STATS_LOG_INTERVAL_MS && atomic_load(args->is_running);
        waited_ms += 100) {
      sleep_us(100000); // 100ms chunks so shutdown is noticed promptly
    }
    if (!atomic_load(args->is_running))
      break; // don't log a final partial-interval snapshot on shutdown --
             // eeye.c's main() logs one definitive snapshot after every
             // other thread has joined instead.

    // "200ms" here is ring_wait_timeout_ns from frame_ring_buffer.c (not
    // reachable from this file -- it's a private constant there); keep
    // this in sync if that ever changes.
    printf("Ring stats (occupancy out of usable capacity; stalls are "
          "~200ms wait-windows spent blocked, not event counts):\n");
    log_ring_stats("in   (producer->effects)", args->ring_buffer_in);
    log_ring_stats("out  (effects->consumer)", args->ring_buffer_out);
    log_ring_stats("free (pool)             ", args->ring_buffer_free);
  }

  return NULL;
}
