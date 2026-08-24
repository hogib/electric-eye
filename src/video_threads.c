#include "video_threads.h"
#include "effect_chain.h"
#include "camera_ctrl.h"
#include "frame_ring_buffer.h"
#include "health.h"
#include "stream_server.h"
#include "rpicam_in.h"
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
// How often consumer_loop republishes its health snapshot. Fast enough
// that an operator notices a failed recording within a couple of seconds,
// slow enough that it stays a rounding error against per-frame work.
#define HEALTH_PUBLISH_INTERVAL_MS 1000

// The two capture backends have identical lifecycles from producer_loop's
// point of view -- open, capture repeatedly, close -- so they're unified
// here rather than duplicating the whole reconnect loop below per backend.
// Exactly one of the two handles is non-NULL at a time.
typedef struct {
  V4l2In *v4l2;
  RpicamIn *rpicam;
} CaptureHandle;

static bool capture_open(const ProducerArgs *args, CaptureHandle *h) {
  *h = (CaptureHandle){0};
  if (args->capture_source == CAPTURE_RPICAM) {
    const Config *cfg = config_current(args->config);
    h->rpicam = rpicam_in_open(args->capture_width, args->capture_height,
                               CAMERA_FRAMERATE, args->downscale,
                               &cfg->camera);
    return h->rpicam != NULL;
  }
  h->v4l2 = v4l2_in_open(args->filename, args->capture_width,
                         args->capture_height, CAMERA_FRAMERATE,
                         args->downscale);
  return h->v4l2 != NULL;
}

// Pushes camera controls to whichever backend is open.
//
// V4L2 takes them as ioctls on the live device, so they apply instantly.
// rpicam-vid takes them as spawn-time CLI flags, so a change there needs
// the child restarted -- handled by the caller, since it is a visible
// ~1s gap in the video rather than something to do silently.
static void capture_apply_controls(CaptureHandle *h, const CameraControls *c) {
  if (h->v4l2 && camera_ctrl_any_set(c))
    camera_ctrl_apply(v4l2_in_fd(h->v4l2), c);
}

// Whether this backend can take a control change without being restarted.
// V4L2 can (they are ioctls on the live device); rpicam cannot, since it
// takes them as command-line arguments to a child process.
static bool capture_controls_are_live(const CaptureHandle *h) {
  return h->v4l2 != NULL;
}

static bool capture_frame(CaptureHandle *h, VideoFrame *frame) {
  return h->rpicam ? rpicam_in_capture(h->rpicam, frame)
                   : v4l2_in_capture(h->v4l2, frame);
}

static void capture_close(CaptureHandle *h) {
  if (h->rpicam)
    rpicam_in_close(h->rpicam);
  else
    v4l2_in_close(h->v4l2);
  *h = (CaptureHandle){0};
}

void *producer_loop(void *arg) {
  ProducerArgs *args = (ProducerArgs *)arg;
  int64_t pts = 0;
  int reconnect_attempt = 0;
  // The controls last pushed to the camera, so a reloaded config only
  // costs ioctls when something actually changed -- re-issuing them every
  // frame would be pointless syscall traffic at 30fps.
  CameraControls last_controls;
  camera_ctrl_init(&last_controls);
  bool have_last_controls = false;
  // Set when a control change forces the rpicam child to be respawned, so
  // the reconnect path below knows this was deliberate rather than a
  // camera failure -- otherwise it would log a scary "capture failed" and
  // count it against the retry budget.
  bool controls_need_restart = false;

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
    CaptureHandle in;
    if (!capture_open(args, &in)) {
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
    controls_need_restart = false;
    // Apply controls to the freshly-opened device. This has to happen on
    // every open, not just the first: producer_loop reopens the camera
    // from scratch on every reconnect, so a camera unplugged and
    // replugged mid-dive comes back at its factory defaults -- exposure
    // silently reverting is exactly the kind of thing nobody notices
    // until the footage is useless.
    {
      const Config *cfg = config_current(args->config);
      capture_apply_controls(&in, &cfg->camera);
      last_controls = cfg->camera;
      have_last_controls = true;
    }

    if (args->capture_source == CAPTURE_RPICAM)
      printf("Camera connected: Pi camera module (via rpicam-vid)\n");
    else
      printf("Camera connected: %s\n", args->filename);

    while (atomic_load(args->is_running)) {
      VideoFrame *frame = NULL;

      if (!ring_pop_wait(args->ring_buffer_free, (void **)&frame,
                        args->is_running))
        break; // shutting down while waiting for a free buffer

      // Controls are hot-reloadable: light changes as the drone descends,
      // and surfacing to adjust exposure is not an option.
      {
        const Config *cfg = config_current(args->config);
        if (!have_last_controls ||
            !camera_ctrl_equal(&last_controls, &cfg->camera)) {
          if (capture_controls_are_live(&in)) {
            capture_apply_controls(&in, &cfg->camera);
            last_controls = cfg->camera;
            have_last_controls = true;
          } else {
            // rpicam bakes controls into its child's command line, so the
            // only way to change them is to respawn it. Announced rather
            // than done silently: it is a visible ~1s gap in the video,
            // and an operator nudging a slider deserves to know why the
            // picture blinked.
            printf("Camera controls changed; restarting rpicam-vid to "
                   "apply them (brief gap in video)...\n");
            controls_need_restart = true;
            break; // drop to the reconnect loop, which reopens with them
          }
        }
      }

      frame->pts = pts++;

      if (!capture_frame(&in, frame)) {
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

    capture_close(&in);

    // A deliberate respawn for new controls should not pay the reconnect
    // backoff -- that delay exists for a camera that genuinely went away.
    if (controls_need_restart)
      continue;
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

  // Live-preview stream tap (see stream_server.h): unlike the virtual-cam
  // write above, this is genuinely optional -- a topside viewer over the
  // tether is a convenience, not something the local pipeline depends on.
  // A failure here (e.g. the port's already in use) only disables the
  // tap, logged, rather than tearing down the whole process.
  StreamServer *stream = stream_server_open(args->stream_port);
  if (!stream) {
    printf("Warning: live-preview stream disabled (see error above).\n");
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

  // Health, republished on a timer to "<config_path>.health" so topside
  // can see what only this process knows -- above all whether recording
  // is still actually running. See health.h.
  HealthSnapshot health = {
      .recording_state = RECORDING_OFF,
      .camera_connected = true, // reaching this loop means frames are flowing
      .frame_width = args->frame_width,
      .frame_height = args->frame_height,
  };
  int64_t last_health_pts = -1;

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
      if (record_path[0] == '\0') {
        health.recording_state = RECORDING_OFF;
        health.recording_path[0] = '\0';
        health.recording_error[0] = '\0';
        health.recording_bytes = 0;
      } else {
        snprintf(health.recording_path, sizeof health.recording_path, "%s",
                 record_path);
        health.recording_bytes = 0;

        // Check for room before opening, not just after failing to write.
        // Raw I422 at 30fps is ~53MB/s at 1280x720, so "there was space
        // when I hit record" is worth knowing before the dive rather than
        // after -- this is reported, not enforced, since how much footage
        // an operator intends to take is theirs to decide.
        uint64_t free_bytes = health_disk_free(record_path);
        size_t frame_bytes = (size_t)args->frame_width *
                             args->frame_height * 2;
        if (free_bytes > 0 && frame_bytes > 0) {
          uint64_t seconds = free_bytes / (frame_bytes * CAMERA_FRAMERATE);
          printf("Recording: %llu MB free at %s (~%llu seconds at this "
                 "geometry)\n",
                 (unsigned long long)(free_bytes / (1024 * 1024)), record_path,
                 (unsigned long long)seconds);
        }

        record_file = fopen(record_path, "wb");
        if (record_file) {
          health.recording_state = RECORDING_ACTIVE;
          health.recording_error[0] = '\0';
          printf("Recording started: %s (raw I422 %ux%u -- play back with: "
                 "ffplay -f rawvideo -pix_fmt yuv422p -s %ux%u -r %d -i %s)\n",
                 record_path, args->frame_width, args->frame_height,
                 args->frame_width, args->frame_height, CAMERA_FRAMERATE,
                 record_path);
        } else {
          printf("Recording failed to start: could not open %s: %s\n",
                 record_path, strerror(errno));
          health.recording_state = RECORDING_FAILED;
          snprintf(health.recording_error, sizeof health.recording_error,
                   "could not open: %s", strerror(errno));
          record_path[0] = '\0'; // don't retry the same failing path every frame
        }
      }
    }

    if (record_file) {
      size_t total = frame->plane_sizes[0] + frame->plane_sizes[1] +
                     frame->plane_sizes[2];
      if (fwrite(frame->raw_data, 1, total, record_file) != total) {
        // Almost always a full disk. This is the case that most needs to
        // reach topside: the operator asked to record, believes they are
        // recording, and nothing on their screen would otherwise change.
        printf("Recording write failed (%s); stopping recording.\n",
               strerror(errno));
        health.recording_state = RECORDING_FAILED;
        snprintf(health.recording_error, sizeof health.recording_error,
                 "write failed: %s", strerror(errno));
        fclose(record_file);
        record_file = NULL;
        record_path[0] = '\0';
      } else {
        health.recording_bytes += total;
      }
    }

    // Live-preview stream tap: throttled by pts rather than a separate
    // counter, since pts already increments by exactly 1 per frame (see
    // producer_loop) -- nothing extra to keep in sync across reloads.
    // stream_server_send_frame() itself skips all JPEG work when no
    // viewer is connected, so this costs nothing while unwatched even at
    // stream_frame_interval == 1.
    if (stream && cfg->stream_frame_interval > 0 &&
        frame->pts % cfg->stream_frame_interval == 0) {
      if (cfg->stream_raw) {
        // Preview the untouched camera frame instead of the processed
        // one, so an operator can tell a real object from a filter
        // artifact without dismantling their chain. Swapped by pointer
        // rather than by copying: raw_data and pixel_data share this
        // frame's layout exactly (see video_frame.h), so a shallow view
        // with the raw planes is all stream_server needs, and it costs
        // nothing per frame.
        VideoFrame raw_view = *frame;
        raw_view.pixel_data = frame->raw_data;
        raw_view.planes[0] = frame->raw_planes[0];
        raw_view.planes[1] = frame->raw_planes[1];
        raw_view.planes[2] = frame->raw_planes[2];
        stream_server_send_frame(stream, &raw_view, cfg->stream_quality);
      } else {
        stream_server_send_frame(stream, frame, cfg->stream_quality);
      }
    }

    // Health snapshot, on a timer. pts increments once per frame (see
    // producer_loop), so it doubles as the clock here rather than needing
    // a separate one that could drift from the frame rate.
    const int64_t health_every =
        (CAMERA_FRAMERATE * HEALTH_PUBLISH_INTERVAL_MS) / 1000;
    if (health_every > 0 && frame->pts / health_every != last_health_pts) {
      last_health_pts = frame->pts / health_every;
      health.frames_captured = (uint64_t)frame->pts;
      health.disk_free_bytes =
          health.recording_path[0] ? health_disk_free(health.recording_path) : 0;
      health_publish(args->config_path, &health);
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

  stream_server_close(stream);
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
