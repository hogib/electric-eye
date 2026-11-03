#include "thread_name.h"
#include "video_threads.h"
#include "effect_chain.h"
#include "camera_ctrl.h"
#include "frame_ring_buffer.h"
#include "health.h"
#include "recorder.h"
#include "bus_in.h"
#include "fbus.h"
#include "rpicam_in.h"
#include "v4l2_in.h"
#include "virtual_cam.h"
#include "video_frame.h"
#include <errno.h>
#include <linux/videodev2.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

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

// The capture backends have identical lifecycles from producer_loop's
// point of view -- open, capture repeatedly, close -- so they're unified
// here rather than duplicating the whole reconnect loop below per backend.
// Exactly one of the handles is non-NULL at a time.
typedef struct {
  V4l2In *v4l2;
  RpicamIn *rpicam;
  BusIn *bus;
} CaptureHandle;

// `log_failure` asks the backend to explain a failed open; the caller
// throttles it so a long outage doesn't flood the log. Only the bus
// backend uses it -- the camera backends always explain.
static bool capture_open(ProducerArgs *args, CaptureHandle *h,
                         bool log_failure) {
  *h = (CaptureHandle){0};
  if (args->capture_source == CAPTURE_BUS) {
    bool wrong_size = false;
    h->bus = bus_in_open(args->capture_stream, args->frame_width,
                         args->frame_height, log_failure, &wrong_size);
    if (wrong_size) {
      // The pool can't follow a new size; a fresh start can. Under
      // systemd that is automatic (Restart=always).
      printf("Stopping so eeye can restart at the stream's new size.\n");
      __atomic_store_n(args->restart_needed, true, __ATOMIC_SEQ_CST);
      __atomic_store_n(args->is_running, false, __ATOMIC_SEQ_CST);
    }
    return h->bus != NULL;
  }
  if (args->capture_source == CAPTURE_RPICAM) {
    const Config *cfg = config_current(args->config);
    h->rpicam = rpicam_in_open(args->capture_width, args->capture_height,
                               CAMERA_FRAMERATE, args->downscale,
                               &cfg->camera, args->capture_format);
    return h->rpicam != NULL;
  }
  h->v4l2 = v4l2_in_open(args->filename, args->capture_width,
                         args->capture_height, CAMERA_FRAMERATE,
                         args->downscale, args->capture_format);
  return h->v4l2 != NULL;
}

// Pushes camera controls to whichever backend is open.
//
// V4L2 takes them as ioctls on the live device, so they apply instantly.
// rpicam-vid takes them as spawn-time CLI flags, so a change there needs
// the child restarted -- handled by the caller, since it is a visible
// ~1s gap in the video rather than something to do silently. A bus
// stream has no camera to apply them to.
static void capture_apply_controls(CaptureHandle *h, const CameraControls *c) {
  if (h->v4l2 && camera_ctrl_any_set(c))
    camera_ctrl_apply(v4l2_in_fd(h->v4l2), c);
}

// Whether this backend can take a control change without being restarted.
// V4L2 can (they are ioctls on the live device); rpicam cannot, since it
// takes them as command-line arguments to a child process. A bus stream
// ignores them, so there is nothing to restart either.
static bool capture_controls_are_live(const CaptureHandle *h) {
  return h->v4l2 != NULL || h->bus != NULL;
}

static bool capture_frame(ProducerArgs *args, CaptureHandle *h,
                          VideoFrame *frame) {
  if (h->bus)
    return bus_in_capture(h->bus, frame, args->is_running);
  return h->rpicam ? rpicam_in_capture(h->rpicam, frame)
                   : v4l2_in_capture(h->v4l2, frame);
}

static void capture_close(CaptureHandle *h) {
  if (h->bus)
    bus_in_close(h->bus);
  else if (h->rpicam)
    rpicam_in_close(h->rpicam);
  else
    v4l2_in_close(h->v4l2);
  *h = (CaptureHandle){0};
}

void *producer_loop(void *arg) {
  thread_set_name("eeye/capture");
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
  // consumer_loop's v4l2loopback output has its own, independent retry
  // (virtual_cam.c), so neither end of the pipeline waiting on its device
  // stops the other.
  while (__atomic_load_n(args->is_running, __ATOMIC_SEQ_CST)) {
    CaptureHandle in;
    // Same throttle as the "unavailable" line below: the first failure,
    // then every CAMERA_RECONNECT_LOG_EVERY-th.
    bool log_this_attempt =
        reconnect_attempt == 0 ||
        (reconnect_attempt + 1) % CAMERA_RECONNECT_LOG_EVERY == 0;
    if (!capture_open(args, &in, log_this_attempt)) {
      reconnect_attempt++;
      if (log_this_attempt && args->capture_source != CAPTURE_BUS) {
        printf("Camera unavailable (attempt %d); retrying every %ds...\n",
               reconnect_attempt, CAMERA_RECONNECT_DELAY_MS / 1000);
      }
      for (int waited_ms = 0;
           waited_ms < CAMERA_RECONNECT_DELAY_MS &&
           __atomic_load_n(args->is_running, __ATOMIC_SEQ_CST);
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

    if (args->capture_source == CAPTURE_BUS)
      printf("Camera connected: bus stream \"%s\"\n", args->capture_stream);
    else if (args->capture_source == CAPTURE_RPICAM)
      printf("Camera connected: Pi camera module (via rpicam-vid)\n");
    else
      printf("Camera connected: %s\n", args->filename);

    while (__atomic_load_n(args->is_running, __ATOMIC_SEQ_CST)) {
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
            // The frame taken from the pool above goes back first: dropped
            // here, every control change would cost one of the pool's
            // eight frames, and the eighth would freeze the video.
            ring_push_wait(args->ring_buffer_free, frame, NULL);
            break; // drop to the reconnect loop, which reopens with them
          }
        }
      }

      frame->pts = pts++;
      frame->capture_ns = 0; // a backend that knows better sets it

      if (!capture_frame(args, &in, frame)) {
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
      // No driver timestamp (rpicam, or a driver stamping another clock):
      // the moment it arrived is the next best thing.
      if (frame->capture_ns == 0)
        frame->capture_ns = vf_now_ns();

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

static void record_effects_time(EffectsTiming *t, const struct timespec *t0,
                                const struct timespec *t1) {
  if (!t)
    return;
  uint64_t ns = (uint64_t)(t1->tv_sec - t0->tv_sec) * 1000000000u +
                (uint64_t)(t1->tv_nsec - t0->tv_nsec);
  __atomic_fetch_add(&t->frames, 1, __ATOMIC_RELAXED);
  __atomic_fetch_add(&t->total_ns, ns, __ATOMIC_RELAXED);
  uint_fast64_t prev = __atomic_load_n(&t->max_ns, __ATOMIC_RELAXED);
  while (ns > prev &&
         !__atomic_compare_exchange_n(&t->max_ns, &prev, ns, true,
                                      __ATOMIC_RELAXED, __ATOMIC_RELAXED)) {
  }
}

void *effects_loop(void *arg) {
  thread_set_name("eeye/effects");
  WorkerArgs *args = (WorkerArgs *)arg;
  // Auto visibility's smoothing state lives as long as this thread. NULL
  // (out of memory) only means auto visibility cannot run.
  AutoVis *av = auto_vis_create();
  if (!av)
    printf("Warning: auto visibility unavailable (out of memory).\n");
  static ImgStats stats; // 1KB+; this thread is the only user
  int64_t last_stats_bucket = -1;
  while (__atomic_load_n(args->is_running, __ATOMIC_SEQ_CST) ||
         __atomic_load_n(&args->ring_buffer_in->head, __ATOMIC_RELAXED) !=
             __atomic_load_n(&args->ring_buffer_in->tail, __ATOMIC_RELAXED)) {

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
    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    // The raw frame's statistics: every frame for auto visibility, which
    // steers by them; otherwise only for the frame the consumer will put in
    // the health file -- the first of each health interval, by the same
    // pts rule it uses -- since nothing else reads them. On a Pi that is
    // most of a millisecond of serial work saved on 29 frames in 30.
    const int64_t stats_every =
        (CAMERA_FRAMERATE * HEALTH_PUBLISH_INTERVAL_MS) / 1000;
    int64_t bucket = stats_every > 0 ? frame->pts / stats_every : frame->pts;
    bool for_health = bucket != last_stats_bucket;
    last_stats_bucket = bucket;
    if (cfg->auto_visibility || for_health) {
      const uint8_t *raw[3] = {frame->raw_planes[0], frame->raw_planes[1],
                               frame->raw_planes[2]};
      img_stats_compute(raw, frame->width, frame->height, frame->stride,
                        &stats);
      img_stats_summarize(&stats, &frame->stats);
    } else {
      memset(&frame->stats, 0, sizeof frame->stats); // valid = false
    }
    apply_effect_chain_auto(frame, cfg, av, &stats);
    clock_gettime(CLOCK_MONOTONIC, &t1);
    record_effects_time(args->timing, &t0, &t1);

    // NULL: a fully-processed frame must reach the consumer regardless of
    // is_running -- abandoning it here would leak it out of the pool.
    ring_push_wait(args->ring_buffer_out, frame, NULL);
  }
  auto_vis_destroy(av);
  return NULL;
}

void *consumer_loop(void *arg) {
  thread_set_name("eeye/output");
  ConsumerArgs *args = (ConsumerArgs *)arg;

  // Optional: a missing or failing loopback device is retried by
  // virtual_cam.c and never stops the pipeline (see virtual_cam.h).
  VirtualCam *vcam = virtual_cam_create(args->outpath, args->frame_width,
                                        args->frame_height);

  // The frame bus: the pilot's picture and the untouched camera frame, for
  // hellion, autonomy, and anything else on this machine (see fbus.h).
  // Publishing copies into shared memory and never waits for a reader, so
  // it is the one new piece of work on this thread. A stream that cannot
  // be created only costs that stream.
  FbusWriter *bus_pilot =
      args->bus_pilot && args->bus_pilot[0]
          ? fbus_writer_open(args->bus_pilot, args->frame_width,
                             args->frame_height)
          : NULL;
  FbusWriter *bus_raw =
      args->bus_raw && args->bus_raw[0]
          ? fbus_writer_open(args->bus_raw, args->frame_width,
                             args->frame_height)
          : NULL;
  if (args->bus_pilot && args->bus_pilot[0] && !bus_pilot)
    printf("Warning: not publishing \"%s\" (see above).\n", args->bus_pilot);
  if (args->bus_raw && args->bus_raw[0] && !bus_raw)
    printf("Warning: not publishing \"%s\" (see above).\n", args->bus_raw);

  // Recording tap, on its own thread (see recorder.h): this loop only
  // offers it finished frames, so a slow disk costs recorded frames, never
  // the virtual camera's.
  Recorder *recorder = recorder_create(args->ring_buffer_free,
                                       args->frame_width, args->frame_height,
                                       CAMERA_FRAMERATE);
  if (!recorder)
    printf("Warning: could not start the recording thread; recording "
           "disabled.\n");
  char record_path[max_record_path_len] = "";
  RecordFormat record_format = RECORD_RAW;

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

  while (__atomic_load_n(args->is_running, __ATOMIC_SEQ_CST) ||
         __atomic_load_n(&args->ring_buffer_out->head, __ATOMIC_RELAXED) !=
             __atomic_load_n(&args->ring_buffer_out->tail, __ATOMIC_RELAXED)) {

    VideoFrame *frame = NULL;

    if (!ring_pop_wait(args->ring_buffer_out, (void **)&frame,
                      args->is_running)) {
      continue;
    }

    // The pilot's feed first, before any optional tap gets a look at the
    // frame. A failure is handled (and logged) inside virtual_cam.c.
    virtual_cam_write(vcam, frame);

    // raw_planes is never written by the effect chain (see
    // video_frame.h), so it is the camera's frame as captured.
    fbus_writer_publish_frame(bus_pilot,
                              (const uint8_t *const *)frame->planes,
                              frame->pts, frame->capture_ns,
                              frame->chroma_neutral);
    fbus_writer_publish(bus_raw, (const uint8_t *const *)frame->raw_planes,
                        frame->pts, frame->capture_ns);

    // Independent config read from effects_loop's -- this frame's chain
    // was already decided by the time it got here, but the recording
    // decision is consumer_loop's own to make. Same once-per-frame
    // snapshot discipline: whatever record_path this frame observes is
    // the one it's judged against for its entire trip through this loop.
    const Config *cfg = config_current(args->config);
    if (recorder && (strcmp(cfg->record_path, record_path) != 0 ||
                     (record_path[0] && cfg->record_format != record_format))) {
      snprintf(record_path, sizeof record_path, "%s", cfg->record_path);
      record_format = cfg->record_format;
      recorder_set_target(recorder, record_path, record_format);
    }

    // Health snapshot, on a timer. pts increments once per frame (see
    // producer_loop), so it doubles as the clock here rather than needing
    // a separate one that could drift from the frame rate. Taken before
    // the frame is handed off below, after which it isn't ours to read.
    const int64_t health_every =
        (CAMERA_FRAMERATE * HEALTH_PUBLISH_INTERVAL_MS) / 1000;
    if (health_every > 0 && frame->pts / health_every != last_health_pts) {
      last_health_pts = frame->pts / health_every;
      if (recorder) {
        RecorderStatus rs;
        recorder_status(recorder, &rs);
        health.recording_state = rs.state;
        snprintf(health.recording_path, sizeof health.recording_path, "%s",
                 rs.path);
        snprintf(health.recording_error, sizeof health.recording_error, "%s",
                 rs.error);
        health.recording_bytes = rs.bytes;
        health.recording_dropped = rs.dropped_frames;
      }
      health.frames_captured = (uint64_t)frame->pts;
      health.image = frame->stats;
      HealthMailbox *box = args->health;
      if (box && __atomic_load_n(&box->writer, __ATOMIC_ACQUIRE)) {
        pthread_mutex_lock(&box->lock);
        box->snap = health;
        box->fresh = true;
        pthread_mutex_unlock(&box->lock);
      } else {
        health.disk_free_bytes = health.recording_path[0]
                                     ? health_disk_free(health.recording_path)
                                     : 0;
        health_publish(args->config_path, &health);
      }
    }

    // The recorder takes the frame and returns it to the pool once it is
    // written; otherwise it goes straight back. NULL: same as every other
    // pushback to the free-list ring -- must not be abandoned on shutdown,
    // or the frame leaks out of the pool. This is the hot path: every
    // single processed frame returns to the pool through one of these.
    if (!recorder || !recorder_submit(recorder, frame))
      ring_push_wait(args->ring_buffer_free, frame, NULL);
  }

  // Drains and closes the recording, returning any frames it still holds.
  recorder_destroy(recorder);
  virtual_cam_destroy(vcam);
  fbus_writer_close(bus_pilot);
  fbus_writer_close(bus_raw);
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

// Writes the consumer's latest health snapshot, if there is a new one.
// The file I/O happens here, off the consumer thread (see HealthMailbox).
static void write_health(HealthMailbox *box) {
  if (!box)
    return;
  HealthSnapshot snap;
  pthread_mutex_lock(&box->lock);
  bool fresh = box->fresh;
  if (fresh) {
    snap = box->snap;
    box->fresh = false;
  }
  pthread_mutex_unlock(&box->lock);
  if (!fresh)
    return;
  snap.disk_free_bytes =
      snap.recording_path[0] ? health_disk_free(snap.recording_path) : 0;
  health_publish(box->config_path, &snap);
}

void *stats_loop(void *arg) {
  thread_set_name("eeye/stats");
  StatsArgs *args = (StatsArgs *)arg;
  if (args->health)
    __atomic_store_n(&args->health->writer, true, __ATOMIC_RELEASE);

  while (__atomic_load_n(args->is_running, __ATOMIC_SEQ_CST)) {
    for (int waited_ms = 0; waited_ms < STATS_LOG_INTERVAL_MS &&
                            __atomic_load_n(args->is_running, __ATOMIC_SEQ_CST);
         waited_ms += 100) {
      sleep_us(100000); // 100ms chunks so shutdown is noticed promptly
      write_health(args->health); // within 100ms of the consumer's snapshot
    }
    if (!__atomic_load_n(args->is_running, __ATOMIC_SEQ_CST))
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

    if (args->effects_timing) {
      EffectsTiming *t = args->effects_timing;
      uint64_t frames = __atomic_exchange_n(&t->frames, 0, __ATOMIC_RELAXED);
      uint64_t total = __atomic_exchange_n(&t->total_ns, 0, __ATOMIC_RELAXED);
      uint64_t max = __atomic_exchange_n(&t->max_ns, 0, __ATOMIC_RELAXED);
      if (frames > 0)
        printf("  effects: %llu frames, avg %.2f ms, max %.2f ms\n",
               (unsigned long long)frames, (double)total / frames / 1e6,
               (double)max / 1e6);
    }
  }

  if (args->health) {
    __atomic_store_n(&args->health->writer, false, __ATOMIC_RELEASE);
    write_health(args->health); // the last snapshot it was handed
  }
  return NULL;
}
