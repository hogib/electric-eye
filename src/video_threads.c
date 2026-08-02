#include "video_threads.h"
#include "conv.h"
#include "frame_ring_buffer.h"
#include "point_opps.h"
#include "v4l2_in.h"
#include "v4l2_out.h"
#include "video_frame.h"
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

      while (!ring_pop(args->ring_buffer_free, (void **)&frame) &&
             atomic_load(args->is_running)) {
        sleep_us(100);
      }

      if (!frame)
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
        while (!ring_push(args->ring_buffer_free, frame))
          sleep_us(10);
        break;
      }

      while (!ring_push(args->ring_buffer_in, frame) &&
             atomic_load(args->is_running)) {
        sleep_us(100);
      }
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

    if (!ring_pop(args->ring_buffer_in, (void **)&frame)) {
      sleep_us(100);
      continue;
    }

    // One snapshot per frame, not per effect: this guarantees a config push
    // landing mid-processing can't split a single output frame between old
    // and new parameters.
    const Config *cfg = config_current(args->config);

    if (cfg->effect == EFFECT_SOBEL) {
      // Sobel overwrites every byte of the work buffer itself (see
      // conv.c), so unlike the point ops below it needs no fresh copy from
      // raw first.
      sobel_edges(frame);
    } else {
      // Every other effect here is a point op inherited from before the
      // raw/work split: it mutates frame->planes in place, assuming that
      // buffer already holds the current frame. Now that raw and work are
      // separate allocations, nothing else keeps work fresh, so refresh it
      // here before handing off.
      size_t total = frame->plane_sizes[0] + frame->plane_sizes[1] +
                     frame->plane_sizes[2];
      memcpy(frame->pixel_data, frame->raw_data, total);

      switch (cfg->effect) {
      case EFFECT_GRAYSCALE:
        grayscale(frame);
        break;
      case EFFECT_INVERT:
        gs_invert(frame);
        break;
      case EFFECT_THRESHOLD:
        gs_threshold_by_value(frame, cfg->threshold_value);
        break;
      case EFFECT_TINT:
        color_tint(frame, cfg->tint_u, cfg->tint_v, cfg->tint_strength);
        break;
      case EFFECT_NONE:
      case EFFECT_SOBEL: // unreachable, handled in the branch above
        break;
      }
    }

    while (!ring_push(args->ring_buffer_out, frame)) {
      sleep_us(100);
    }
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

  while (atomic_load(args->is_running) ||
         atomic_load_explicit(&args->ring_buffer_out->head,
                              memory_order_relaxed) !=
             atomic_load_explicit(&args->ring_buffer_out->tail,
                                  memory_order_relaxed)) {

    VideoFrame *frame = NULL;

    if (!ring_pop(args->ring_buffer_out, (void **)&frame)) {
      sleep_us(100);
      continue;
    }

    if (!v4l2_out_write(out, frame)) {
      // The loopback device went away or rejected the frame. Stop the whole
      // pipeline rather than spinning on a dead device.
      printf("Virtual camera write failed; shutting down.\n");
      while (!ring_push(args->ring_buffer_free, frame))
        sleep_us(10);
      atomic_store(args->is_running, false);
      break;
    }

    while (!ring_push(args->ring_buffer_free, frame)) {
      sleep_us(100);
    }
  }

  v4l2_out_close(out);
  return NULL;
}
