#define _POSIX_C_SOURCE 200809L

#include "sinks.h"
#include "child_proc.h"
#include "fbus.h"
#include "ffmpeg.h"
#include "udp_relay.h"
#include "video_frame.h"
#include "virtual_cam.h"
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

// An encoder that cannot take a frame within this long is stuck, not slow
// (a frame is a few ms of encoding), and is restarted.
static const int encoder_write_timeout_ms = 2000;
static const int restart_min_ms = 1000;
static const int restart_max_ms = 30000;
// An encoder that ran at least this long before failing starts its next
// back-off from the minimum again.
static const uint64_t healthy_run_ns = 30000000000ull;

struct SinkImpl {
  SinkKind kind;
  char arg[hcfg_max_spec];
  uint32_t width, height;

  FbusWriter *bus;
  VirtualCam *vcam;

  // RTP: ffmpeg encodes to a local port; the relay sends that on.
  UdpRelay *relay;
  ChildProc enc;
  FfEncoder enc_cfg;
  char encoder[32];
  uint64_t enc_started_ns;
  uint64_t next_start_ns; // 0 = start now
  int backoff_ms;
  unsigned restarts;
  // The frames actually arriving, against the nominal rate the encoder
  // stamps them at (see ff_encoder_args).
  uint64_t rate_window_ns;
  uint64_t rate_window_frames;
  double measured_fps;
  bool warned_rate;
};

static uint64_t mono_ns(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

static void report(SinkReport *rep, const char *state, const char *fmt, ...)
    __attribute__((format(printf, 3, 4)));

static void report(SinkReport *rep, const char *state, const char *fmt, ...) {
  snprintf(rep->state, sizeof rep->state, "%s", state);
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(rep->detail, sizeof rep->detail, fmt, ap);
  va_end(ap);
}

// ---- RTP ------------------------------------------------------------------

static bool encoder_start(SinkImpl *s, SinkReport *rep) {
  FfArgs args;
  ff_encoder_args(&s->enc_cfg, &args);
  if (!child_spawn(&s->enc, "hellion", args.argv, CHILD_PIPE_STDIN)) {
    report(rep, "error", "could not start ffmpeg (see log)");
    return false;
  }
  // A raw 720p frame is ~1.8MB; a bigger pipe takes it in fewer writes.
  child_set_pipe_size(s->enc.in_fd, 1 << 20);
  s->enc_started_ns = mono_ns();
  printf("hellion: rtp sink %s: encoding %ux%u with %s at %uk, gop %u "
         "(ffmpeg pid %d)\n",
         s->arg, s->width, s->height, s->encoder, s->enc_cfg.bitrate_kbps,
         s->enc_cfg.gop, (int)s->enc.pid);
  return true;
}

// Stops the encoder and schedules the next start, backing off while it
// keeps failing.
static void encoder_failed(SinkImpl *s, const char *why) {
  child_stop(&s->enc, 500);
  uint64_t now = mono_ns();
  if (now - s->enc_started_ns >= healthy_run_ns)
    s->backoff_ms = restart_min_ms;
  printf("hellion: rtp sink %s: encoder %s; restarting in %ds\n", s->arg, why,
         (s->backoff_ms + 999) / 1000);
  s->next_start_ns = now + (uint64_t)s->backoff_ms * 1000000ull;
  s->backoff_ms *= 2;
  if (s->backoff_ms > restart_max_ms)
    s->backoff_ms = restart_max_ms;
  s->restarts++;
}

// Frames are stamped at the nominal rate, so a source that really runs at
// another one makes the stream's clock run fast or slow: invisible live,
// but a recording plays at the wrong speed. Say so once, with the fix.
static void check_rate(SinkImpl *s, uint64_t now) {
  static const uint64_t window_ns = 10000000000ull;
  if (s->rate_window_ns == 0) {
    s->rate_window_ns = now;
    s->rate_window_frames = 0;
    return;
  }
  s->rate_window_frames++;
  if (now - s->rate_window_ns < window_ns)
    return;
  s->measured_fps =
      (double)s->rate_window_frames * 1e9 / (double)(now - s->rate_window_ns);
  s->rate_window_ns = now;
  s->rate_window_frames = 0;
  double nominal = (double)s->enc_cfg.framerate;
  if (!s->warned_rate && (s->measured_fps < nominal * 0.9 ||
                          s->measured_fps > nominal * 1.1)) {
    printf("hellion: rtp sink %s: frames arrive at %.1f fps but are stamped "
           "at %u fps; recordings of this stream will play at the wrong "
           "speed. Set this pipeline's \"framerate\" to %.0f.\n",
           s->arg, s->measured_fps, s->enc_cfg.framerate, s->measured_fps);
    s->warned_rate = true;
  }
}

static bool rtp_open(SinkImpl *s, const PipelineConfig *pc, SinkReport *rep) {
  if (!ff_available()) {
    report(rep, "error", "ffmpeg is not installed");
    return false;
  }
  char err[160];
  s->relay = udp_relay_open("127.0.0.1:0", err, sizeof err);
  if (!s->relay || !udp_relay_add_dest(s->relay, s->arg, err, sizeof err) ||
      !udp_relay_start(s->relay)) {
    printf("hellion: rtp sink %s: %s\n", s->arg, err);
    report(rep, "error", "%s", err);
    udp_relay_close(s->relay);
    s->relay = NULL;
    return false;
  }
  snprintf(s->encoder, sizeof s->encoder, "%s", pc->encoder);
  s->enc_cfg = (FfEncoder){.encoder = s->encoder,
                           .width = s->width,
                           .height = s->height,
                           .bitrate_kbps = pc->bitrate_kbps,
                           .gop = pc->gop,
                           .framerate = pc->framerate,
                           .rtp_port = udp_relay_port(s->relay)};
  s->enc = (ChildProc){.pid = 0, .in_fd = -1, .out_fd = -1, .err_fd = -1};
  s->backoff_ms = restart_min_ms;
  report(rep, "starting", "%s", "");
  return true;
}

static bool rtp_write(SinkImpl *s, const SinkFrame *f, SinkReport *rep) {
  uint64_t now = mono_ns();
  if (s->enc.pid <= 0) {
    if (now < s->next_start_ns) {
      report(rep, "waiting", "encoder restarting in %.0fs",
             (double)(s->next_start_ns - now) / 1e9);
      return false;
    }
    if (!encoder_start(s, rep)) {
      encoder_failed(s, "would not start");
      return false;
    }
  }

  for (int p = 0; p < 3; p++) {
    ChildIo r = child_write_all(s->enc.in_fd, f->planes[p], f->plane_bytes[p],
                                encoder_write_timeout_ms);
    if (r != CHILD_IO_OK) {
      encoder_failed(s, r == CHILD_IO_EOF       ? "exited (see log)"
                        : r == CHILD_IO_TIMEOUT ? "stopped taking frames"
                                                : "pipe error");
      report(rep, "waiting", "encoder restarting");
      return false;
    }
  }
  check_rate(s, now);
  report(rep, "sending", "%s %uk%s%s", s->encoder, s->enc_cfg.bitrate_kbps,
         s->restarts ? ", encoder restarted" : "",
         s->warned_rate ? ", framerate mismatch (see log)" : "");
  return true;
}

// ---- Common ---------------------------------------------------------------

SinkImpl *sink_impl_open(const SinkConfig *cfg, const PipelineConfig *pc,
                         uint32_t width, uint32_t height, SinkReport *rep) {
  SinkImpl *s = (SinkImpl *)calloc(1, sizeof *s);
  if (!s) {
    report(rep, "error", "out of memory");
    return NULL;
  }
  s->kind = cfg->kind;
  s->width = width;
  s->height = height;
  snprintf(s->arg, sizeof s->arg, "%s", cfg->arg);

  switch (cfg->kind) {
  case SINK_BUS:
    s->bus = fbus_writer_open(cfg->arg, width, height);
    if (!s->bus) {
      report(rep, "error", "could not create the stream (see log)");
      free(s);
      return NULL;
    }
    report(rep, "publishing", "%s", "");
    break;
  case SINK_V4L2:
    s->vcam = virtual_cam_create(cfg->arg, width, height);
    report(rep, "waiting", "not opened yet");
    break;
  case SINK_RTP:
    if (!rtp_open(s, pc, rep)) {
      free(s);
      return NULL;
    }
    break;
  }
  return s;
}

bool sink_impl_write(SinkImpl *s, const SinkFrame *f, SinkReport *rep) {
  switch (s->kind) {
  case SINK_BUS:
    fbus_writer_publish(s->bus, f->planes, f->frame_no, f->capture_ns);
    report(rep, "publishing", "%s", "");
    return true;

  case SINK_V4L2: {
    // virtual_cam.c takes a VideoFrame; it only needs the contiguous
    // planes and their sizes, so a view over this frame will do.
    VideoFrame view;
    memset(&view, 0, sizeof view);
    view.width = f->width;
    view.height = f->height;
    view.pixel_data = (uint8_t *)f->planes[0];
    for (int i = 0; i < 3; i++) {
      view.planes[i] = (uint8_t *)f->planes[i];
      view.plane_sizes[i] = f->plane_bytes[i];
    }
    bool ok = virtual_cam_write(s->vcam, &view);
    if (ok)
      report(rep, "writing", "%s", "");
    else
      report(rep, "waiting", "device missing or busy; retrying (see log)");
    return ok;
  }

  case SINK_RTP:
    return rtp_write(s, f, rep);
  }
  return false;
}

bool sink_impl_net(SinkImpl *s, RelayStats *out) {
  if (!s || !s->relay)
    return false;
  udp_relay_stats(s->relay, out);
  return true;
}

void sink_impl_close(SinkImpl *s) {
  if (!s)
    return;
  fbus_writer_close(s->bus);
  virtual_cam_destroy(s->vcam);
  if (s->kind == SINK_RTP) {
    // Closing stdin first lets ffmpeg flush what it has and exit cleanly.
    child_stop(&s->enc, 2000);
    udp_relay_close(s->relay);
  }
  free(s);
}
