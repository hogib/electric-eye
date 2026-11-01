#define _POSIX_C_SOURCE 200809L

#include "pipeline.h"
#include "child_proc.h"
#include "fbus.h"
#include "ffmpeg.h"
#include "rpicam_in.h"
#include "sinks.h"
#include "v4l2_in.h"
#include "video_frame.h"
#include <limits.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

// How long a missing source waits before trying again.
static const int source_retry_ms = 2000;
// fps is measured over windows at least this long.
static const uint64_t fps_window_ns = 1000000000ull;

typedef struct {
  uint64_t last_frames;
  uint64_t last_ns;
  double fps;
} FpsMeter;

typedef struct Sink {
  SinkConfig cfg;
  const PipelineConfig *pc; // the owning pipeline's; outlives the sink
  pthread_t thread;
  pthread_mutex_t lock;
  pthread_cond_t cond;
  bool stop; // under lock

  // The mailbox: the source copies into `slot`; the sink thread swaps it
  // with `work` and sends that, so neither waits on the other's copy.
  uint32_t width, height;
  size_t plane_bytes[3];
  size_t frame_bytes;
  uint8_t *slot;
  uint8_t *work;
  bool full;
  int64_t slot_frame_no;
  uint64_t slot_capture_ns;

  // Status, under lock.
  uint64_t frames;
  uint64_t dropped;
  SinkReport rep;
  FpsMeter fps;
  uint64_t last_frame_ns; // last frame taken from the mailbox; 0 = none
  // The sink thread's implementation, published so the status thread can
  // ask a network sink for its link stats. Set and cleared under lock.
  SinkImpl *impl;
} Sink;

struct Pipeline {
  PipelineConfig cfg;
  pthread_t thread;
  bool stop; // atomic (see compat.h)

  pthread_mutex_t lock; // the fields below, and the sinks array itself
  char src_state[hs_state_len];
  char src_detail[hs_detail_len];
  uint64_t src_frames;
  uint64_t src_last_frame_ns; // monotonic; 0 = none yet
  FpsMeter src_fps;
  uint32_t width, height;
  Sink *sinks[hcfg_max_sinks];
  size_t sink_count;

  // RTP source only, for the pipeline's whole life: the relay owns the
  // tether port, so packets keep reaching relay targets (QGC) while the
  // decoder restarts. The decoder is fed through dec_port.
  UdpRelay *rtp_relay;
  int dec_port;
  char sdp_path[PATH_MAX];

  // The decoder's complaints about damaged frames (from lost packets),
  // counted rather than logged one by one: at 2% loss it is ~10 lines a
  // second, which would bury everything else in top's log. Under lock.
  uint64_t damage_total;
  uint64_t damage_window;      // since damage_window_ns
  uint64_t damage_last_window; // the previous full window, for status
  uint64_t damage_window_ns;

  // Camera controls (camera sources only), under lock: set by
  // pipeline_set_camera, which bumps cam_gen; the source thread applies a
  // new generation and records it in cam_done_gen, with how many controls
  // the camera took. cam_gen 0 means none were ever given.
  CameraControls cam;
  uint64_t cam_gen;
  uint64_t cam_done_gen;
  int cam_done_n;
  bool cam_restarting; // rpicam-vid is being restarted to apply them
};

// How often the decoder's damage count is summarised in the log.
static const uint64_t damage_summary_ns = 10000000000ull;

static uint64_t mono_ns(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

static bool stopping(Pipeline *p) {
  return __atomic_load_n(&p->stop, __ATOMIC_ACQUIRE);
}

// Sleeps up to `ms`, returning early (true) if the pipeline is stopping.
static bool nap(Pipeline *p, int ms) {
  for (int waited = 0; waited < ms; waited += 50) {
    if (stopping(p))
      return true;
    struct timespec ts = {0, 50000000L};
    nanosleep(&ts, NULL);
  }
  return stopping(p);
}

static void set_source(Pipeline *p, const char *state, const char *detail) {
  pthread_mutex_lock(&p->lock);
  snprintf(p->src_state, sizeof p->src_state, "%s", state);
  snprintf(p->src_detail, sizeof p->src_detail, "%.*s", hs_detail_len - 1,
           detail);
  pthread_mutex_unlock(&p->lock);
}

static double fps_update(FpsMeter *m, uint64_t frames, uint64_t now) {
  if (m->last_ns == 0) {
    m->last_ns = now;
    m->last_frames = frames;
  } else if (now - m->last_ns >= fps_window_ns) {
    m->fps = (double)(frames - m->last_frames) * 1e9 /
             (double)(now - m->last_ns);
    m->last_ns = now;
    m->last_frames = frames;
  }
  return m->fps;
}

// ---- Sinks --------------------------------------------------------------

static void *sink_loop(void *arg) {
  Sink *s = (Sink *)arg;
  SinkReport rep = {"starting", ""};
  SinkImpl *impl = sink_impl_open(&s->cfg, s->pc, s->width, s->height, &rep);
  // A sink that failed to open retries this often, not on every frame.
  const uint64_t reopen_every_ns = 2000000000ull;
  uint64_t next_open_ns = mono_ns() + reopen_every_ns;

  pthread_mutex_lock(&s->lock);
  s->rep = rep;
  s->impl = impl;
  while (!s->stop) {
    if (!s->full) {
      struct timespec until;
      clock_gettime(CLOCK_REALTIME, &until);
      until.tv_nsec += 250000000L;
      if (until.tv_nsec >= 1000000000L) {
        until.tv_sec++;
        until.tv_nsec -= 1000000000L;
      }
      pthread_cond_timedwait(&s->cond, &s->lock, &until);
      continue;
    }
    uint8_t *tmp = s->work;
    s->work = s->slot;
    s->slot = tmp;
    s->full = false;
    SinkFrame f = {
        .planes = {s->work, s->work + s->plane_bytes[0],
                   s->work + s->plane_bytes[0] + s->plane_bytes[1]},
        .plane_bytes = {s->plane_bytes[0], s->plane_bytes[1],
                        s->plane_bytes[2]},
        .width = s->width,
        .height = s->height,
        .frame_no = s->slot_frame_no,
        .capture_ns = s->slot_capture_ns,
    };
    pthread_mutex_unlock(&s->lock);

    bool ok = false;
    bool reopened = false;
    if (impl) {
      ok = sink_impl_write(impl, &f, &rep);
    } else if (mono_ns() >= next_open_ns) {
      // Opening failed outright; try again now and then.
      impl = sink_impl_open(&s->cfg, s->pc, s->width, s->height, &rep);
      next_open_ns = mono_ns() + reopen_every_ns;
      reopened = impl != NULL;
    }

    pthread_mutex_lock(&s->lock);
    if (ok)
      s->frames++;
    s->last_frame_ns = mono_ns();
    s->rep = rep;
    if (reopened)
      s->impl = impl;
  }
  s->impl = NULL; // before closing it, so status never reads a freed one
  pthread_mutex_unlock(&s->lock);

  sink_impl_close(impl);
  return NULL;
}

static Sink *sink_start(const SinkConfig *cfg, const PipelineConfig *pc,
                        uint32_t w, uint32_t h) {
  Sink *s = (Sink *)calloc(1, sizeof *s);
  if (!s)
    return NULL;
  s->cfg = *cfg;
  s->pc = pc;
  s->width = w;
  s->height = h;
  s->plane_bytes[0] = (size_t)w * h;
  s->plane_bytes[1] = s->plane_bytes[2] = (size_t)((w + 1) / 2) * h;
  s->frame_bytes = s->plane_bytes[0] + 2 * s->plane_bytes[1];
  s->slot = (uint8_t *)malloc(s->frame_bytes);
  s->work = (uint8_t *)malloc(s->frame_bytes);
  snprintf(s->rep.state, sizeof s->rep.state, "starting");
  pthread_mutex_init(&s->lock, NULL);
  pthread_cond_init(&s->cond, NULL);
  if (!s->slot || !s->work ||
      pthread_create(&s->thread, NULL, sink_loop, s) != 0) {
    printf("hellion: could not start sink %s\n", cfg->spec);
    free(s->slot);
    free(s->work);
    free(s);
    return NULL;
  }
  return s;
}

// Hands a frame to a sink: one copy into its slot, never a wait for the
// sink itself. An untaken frame is replaced, and counted as dropped.
static void sink_offer(Sink *s, uint8_t *const planes[3], int64_t frame_no,
                       uint64_t capture_ns) {
  pthread_mutex_lock(&s->lock);
  if (s->full)
    s->dropped++;
  memcpy(s->slot, planes[0], s->plane_bytes[0]);
  memcpy(s->slot + s->plane_bytes[0], planes[1], s->plane_bytes[1]);
  memcpy(s->slot + s->plane_bytes[0] + s->plane_bytes[1], planes[2],
         s->plane_bytes[2]);
  s->slot_frame_no = frame_no;
  s->slot_capture_ns = capture_ns;
  s->full = true;
  pthread_cond_signal(&s->cond);
  pthread_mutex_unlock(&s->lock);
}

// The same, without the copy: the frame's own raw block goes into the
// mailbox, and the frame takes the slot's block for the source to capture
// the next one into. For the last (on the ROV, the only) sink of each
// frame, which saves a full-frame copy -- 1.84MB at 720p, a millisecond on
// a Pi -- per frame. Only when the layouts match exactly (they do: the
// sinks are sized from the source), else a copy as above. The two blocks
// come from malloc on both sides, and raw_data is always owned[1] (see
// video_frame.h), so ownership moves with the pointers.
static void sink_offer_swap(Sink *s, VideoFrame *vf, int64_t frame_no) {
  size_t y = vf->plane_sizes[0], c = vf->plane_sizes[1];
  if (s->plane_bytes[0] != y || s->plane_bytes[1] != c ||
      vf->plane_sizes[2] != c || vf->owned[1] != vf->raw_data) {
    sink_offer(s, vf->raw_planes, frame_no, vf->capture_ns);
    return;
  }
  pthread_mutex_lock(&s->lock);
  if (s->full)
    s->dropped++;
  uint8_t *frame_block = vf->raw_data;
  vf->raw_data = vf->owned[1] = s->slot;
  vf->raw_planes[0] = s->slot;
  vf->raw_planes[1] = s->slot + y;
  vf->raw_planes[2] = s->slot + y + c;
  s->slot = frame_block;
  s->slot_frame_no = frame_no;
  s->slot_capture_ns = vf->capture_ns;
  s->full = true;
  pthread_cond_signal(&s->cond);
  pthread_mutex_unlock(&s->lock);
}

static void sink_stop(Sink *s) {
  if (!s)
    return;
  pthread_mutex_lock(&s->lock);
  s->stop = true;
  pthread_cond_signal(&s->cond);
  pthread_mutex_unlock(&s->lock);
  pthread_join(s->thread, NULL);
  pthread_mutex_destroy(&s->lock);
  pthread_cond_destroy(&s->cond);
  free(s->slot);
  free(s->work);
  free(s);
}

// (Re)starts every sink at w x h; 0 x 0 just stops them. The old ones are
// detached from the pipeline under the lock (so the status thread never
// sees a freed sink) and stopped outside it.
static void restart_sinks(Pipeline *p, uint32_t w, uint32_t h) {
  Sink *old[hcfg_max_sinks];
  size_t old_count;

  pthread_mutex_lock(&p->lock);
  old_count = p->sink_count;
  memcpy(old, p->sinks, sizeof old);
  p->sink_count = 0;
  pthread_mutex_unlock(&p->lock);
  for (size_t i = 0; i < old_count; i++)
    sink_stop(old[i]);

  Sink *fresh[hcfg_max_sinks];
  size_t n = 0;
  for (size_t i = 0; w && h && i < p->cfg.sink_count; i++) {
    Sink *s = sink_start(&p->cfg.sinks[i], &p->cfg, w, h);
    if (s)
      fresh[n++] = s;
  }

  pthread_mutex_lock(&p->lock);
  memcpy(p->sinks, fresh, n * sizeof fresh[0]);
  p->sink_count = n;
  p->width = w;
  p->height = h;
  pthread_mutex_unlock(&p->lock);
}

// ---- Sources ------------------------------------------------------------

typedef struct {
  V4l2In *v4l2;
  RpicamIn *rpicam;
  FbusReader *bus;
  // RTP: an ffmpeg decoder writing raw frames to dec.out_fd. Frames are
  // read in pieces as they arrive, so a timeout mid-frame keeps its place
  // rather than desynchronising the stream.
  ChildProc dec;
  bool dec_running;
  size_t dec_have;
  uint64_t dec_last_frame_ns;
  char dec_line[256]; // the decoder's stderr line being assembled
  size_t dec_line_len;
  uint32_t width, height;
  uint64_t cam_gen; // camera controls generation this open has applied
} Source;

// One complete line of the decoder's stderr. libavcodec's H.264 decoder
// prefixes everything it says about the bitstream with "[h264 @": with a
// lossy link that is damaged frames, expected and already measured as
// packet loss, so it is counted. Anything else is news and passes through.
static void decoder_line(Pipeline *p, const char *line) {
  if (strncmp(line, "[h264 @", 7) == 0) {
    pthread_mutex_lock(&p->lock);
    p->damage_total++;
    p->damage_window++;
    pthread_mutex_unlock(&p->lock);
    return;
  }
  // Its ~20s no-packets timeout, already shown as "no packets" on the link;
  // and ffmpeg's own "Last message repeated N times", which follows the
  // H.264 complaints counted above.
  if (strstr(line, "Connection timed out") ||
      strstr(line, "Last message repeated"))
    return;
  if (line[0])
    printf("hellion: pipeline \"%s\": decoder: %s\n", p->cfg.name, line);
}

// Reads whatever the decoder has said, and summarises the damage count
// every damage_summary_ns.
static void drain_decoder_log(Pipeline *p, Source *src) {
  char buf[1024];
  for (;;) {
    ssize_t n = read(src->dec.err_fd, buf, sizeof buf);
    if (n <= 0)
      break;
    for (ssize_t i = 0; i < n; i++) {
      if (buf[i] == '\n' || buf[i] == '\r') {
        src->dec_line[src->dec_line_len] = '\0';
        decoder_line(p, src->dec_line);
        src->dec_line_len = 0;
      } else if (src->dec_line_len < sizeof src->dec_line - 1) {
        src->dec_line[src->dec_line_len++] = buf[i];
      }
    }
  }

  uint64_t now = mono_ns();
  pthread_mutex_lock(&p->lock);
  if (p->damage_window_ns == 0)
    p->damage_window_ns = now;
  uint64_t n = 0;
  if (now - p->damage_window_ns >= damage_summary_ns) {
    n = p->damage_window;
    p->damage_last_window = n;
    p->damage_window = 0;
    p->damage_window_ns = now;
  }
  pthread_mutex_unlock(&p->lock);
  if (n)
    printf("hellion: pipeline \"%s\": the decoder reported %llu damaged-"
           "bitstream errors in the last %.0fs (lost packets; see the link's "
           "loss figure)\n",
           p->cfg.name, (unsigned long long)n, damage_summary_ns / 1e9);
}

// How long a decoder may produce nothing while packets are arriving
// before it is presumed wedged and restarted.
static const uint64_t decoder_wedged_ns = 5000000000ull;

// Sets up the RTP source's relay and decoder SDP, once per pipeline.
static bool rtp_prepare(Pipeline *p, char *err, size_t err_cap) {
  if (p->rtp_relay)
    return true;
  if (!ff_available()) {
    snprintf(err, err_cap, "ffmpeg is not installed");
    return false;
  }
  p->dec_port = udp_free_port_pair();
  if (!p->dec_port) {
    snprintf(err, err_cap, "no free local UDP port for the decoder");
    return false;
  }
  snprintf(p->sdp_path, sizeof p->sdp_path, "%s/.hellion-%s-%ld.sdp",
           fbus_dir(), p->cfg.name, (long)getpid());
  if (!ff_write_sdp(p->sdp_path, p->dec_port)) {
    snprintf(err, err_cap, "cannot write %.100s", p->sdp_path);
    return false;
  }

  UdpRelay *r = udp_relay_open(p->cfg.source_arg, err, err_cap);
  if (!r)
    return false;
  char dest[64];
  snprintf(dest, sizeof dest, "127.0.0.1:%d", p->dec_port);
  bool ok = udp_relay_add_decoder(r, dest, err, err_cap);
  for (size_t i = 0; ok && i < p->cfg.relay_count; i++)
    ok = udp_relay_add_dest(r, p->cfg.relays[i], err, err_cap);
  if (!ok || !udp_relay_start(r)) {
    udp_relay_close(r);
    return false;
  }
  printf("hellion: pipeline \"%s\": receiving RTP on UDP %d", p->cfg.name,
         udp_relay_port(r));
  for (size_t i = 0; i < p->cfg.relay_count; i++)
    printf("%s%s", i ? ", " : "; relaying to ", p->cfg.relays[i]);
  printf("\n");

  pthread_mutex_lock(&p->lock);
  p->rtp_relay = r;
  pthread_mutex_unlock(&p->lock);
  return true;
}

static bool device_exists(const char *path) {
  struct stat st;
  return stat(path, &st) == 0;
}

static void take_camera(Pipeline *p, CameraControls *out, uint64_t *gen) {
  pthread_mutex_lock(&p->lock);
  *out = p->cam;
  *gen = p->cam_gen;
  pthread_mutex_unlock(&p->lock);
}

static void camera_done(Pipeline *p, Source *src, uint64_t gen, int n) {
  src->cam_gen = gen;
  pthread_mutex_lock(&p->lock);
  p->cam_done_gen = gen;
  p->cam_done_n = n;
  p->cam_restarting = false;
  pthread_mutex_unlock(&p->lock);
}

// The latest controls onto an open V4L2 camera.
static void apply_v4l2_camera(Pipeline *p, Source *src) {
  CameraControls cc;
  uint64_t gen;
  take_camera(p, &cc, &gen);
  int n = 0;
  if (gen && camera_ctrl_any_set(&cc))
    n = camera_ctrl_apply(v4l2_in_fd(src->v4l2), &cc);
  camera_done(p, src, gen, n);
}

static int controls_set(const CameraControls *c) {
  const int32_t *f = (const int32_t *)c;
  int n = 0;
  for (size_t i = 0; i < sizeof *c / sizeof *f; i++)
    n += f[i] != camera_ctrl_unset;
  return n;
}

// Opens the pipeline's source. On failure sets the source status and
// returns false; the caller waits and retries.
static bool source_open(Pipeline *p, Source *src) {
  const PipelineConfig *c = &p->cfg;
  *src = (Source){0};
  uint32_t w = c->width, h = c->height;
  char detail[hs_detail_len];

  switch (c->source) {
  case SRC_V4L2:
    // Checked first so a camera left unplugged waits quietly instead of
    // v4l2_in logging the same failure every retry.
    if (!device_exists(c->source_arg)) {
      snprintf(detail, sizeof detail, "%.90s: no such device", c->source_arg);
      set_source(p, "waiting", detail);
      return false;
    }
    v4l2_in_negotiate_size(c->source_arg, &w, &h, 1, c->format);
    src->v4l2 =
        v4l2_in_open(c->source_arg, w, h, c->framerate, 1, c->format);
    if (!src->v4l2) {
      set_source(p, "waiting", "camera would not open (see log)");
      return false;
    }
    // Every open, not only when they change: a replugged camera comes back
    // at its factory defaults.
    apply_v4l2_camera(p, src);
    break;
  case SRC_RPICAM: {
    CameraControls cc;
    uint64_t gen;
    take_camera(p, &cc, &gen);
    bool any = gen && camera_ctrl_any_set(&cc);
    src->rpicam = rpicam_in_open(w, h, c->framerate, 1, any ? &cc : NULL,
                                 c->format);
    if (!src->rpicam) {
      set_source(p, "waiting", "rpicam-vid would not start (see log)");
      return false;
    }
    camera_done(p, src, gen, any ? controls_set(&cc) : 0);
    break;
  }
  case SRC_BUS: {
    FbusOpenResult why;
    src->bus = fbus_reader_open(c->source_arg, &why);
    if (!src->bus) {
      snprintf(detail, sizeof detail, "bus stream \"%.31s\": %s", c->source_arg,
               fbus_open_result_str(why));
      set_source(p, "waiting", detail);
      return false;
    }
    fbus_reader_geometry(src->bus, &w, &h);
    break;
  }
  case SRC_RTP: {
    char err[160];
    if (!rtp_prepare(p, err, sizeof err)) {
      set_source(p, "error", err);
      return false;
    }
    // ffmpeg scales whatever it decodes to the configured size, so every
    // frame on the pipe is exactly w x h I422.
    FfDecoder d = {.sdp_path = p->sdp_path, .width = w, .height = h};
    FfArgs args;
    ff_decoder_args(&d, &args);
    if (!child_spawn(&src->dec, "hellion", args.argv,
                     CHILD_PIPE_STDOUT | CHILD_PIPE_STDERR | CHILD_NULL_STDIN)) {
      set_source(p, "error", "could not start the ffmpeg decoder (see log)");
      return false;
    }
    child_set_pipe_size(src->dec.out_fd, 1 << 20);
    src->dec_running = true;
    src->dec_last_frame_ns = mono_ns();
    break;
  }
  }
  src->width = w;
  src->height = h;
  return true;
}

static void source_close(Source *src) {
  v4l2_in_close(src->v4l2);
  rpicam_in_close(src->rpicam);
  fbus_reader_close(src->bus);
  if (src->dec_running)
    child_stop(&src->dec, 1000);
  *src = (Source){0};
}

typedef enum { CAP_FRAME, CAP_IDLE, CAP_LOST } CaptureResult;

// One frame from the RTP decoder: exactly one I422 frame's bytes, read
// in whatever pieces they arrive in.
static CaptureResult rtp_capture(Pipeline *p, Source *src, VideoFrame *vf) {
  const size_t total =
      vf->plane_sizes[0] + vf->plane_sizes[1] + vf->plane_sizes[2];
  while (src->dec_have < total) {
    drain_decoder_log(p, src);
    size_t got = 0;
    ChildIo r = child_read_some(src->dec.out_fd, vf->raw_data + src->dec_have,
                                total - src->dec_have, 500, &got);
    if (r == CHILD_IO_OK) {
      src->dec_have += got;
      continue;
    }
    if (r != CHILD_IO_TIMEOUT) {
      // ffmpeg's RTP input gives up after ~20s without packets ("Connection
      // timed out") -- normal while the ROV is off, and restarting it is
      // all there is to do, so that case is not worth a log line each time.
      RelayStats rs;
      udp_relay_stats(p->rtp_relay, &rs);
      if (rs.idle_s >= 0 && rs.idle_s < 15.0)
        printf("hellion: pipeline \"%s\": the ffmpeg decoder exited (see "
               "log); restarting it\n",
               p->cfg.name);
      return CAP_LOST;
    }
    // Nothing for a while. If packets are still arriving the decoder is
    // wedged, not starved: restart it. The clock only runs while packets
    // arrive -- a decoder started during an outage (the old one timed out)
    // has had nothing to decode, and was being killed the moment packets
    // came back, before it could produce a frame.
    RelayStats rs;
    udp_relay_stats(p->rtp_relay, &rs);
    if (rs.idle_s < 0 || rs.idle_s >= 1.0) {
      src->dec_last_frame_ns = mono_ns(); // starved, not wedged
      return CAP_IDLE;
    }
    if (mono_ns() - src->dec_last_frame_ns > decoder_wedged_ns) {
      printf("hellion: pipeline \"%s\": packets arriving but the decoder "
             "has produced nothing for %.0fs; restarting it\n",
             p->cfg.name, decoder_wedged_ns / 1e9);
      return CAP_LOST;
    }
    return CAP_IDLE;
  }
  src->dec_have = 0;
  src->dec_last_frame_ns = mono_ns();
  return CAP_FRAME;
}

static CaptureResult source_capture(Pipeline *p, Source *src, VideoFrame *vf,
                                    int64_t *frame_no) {
  vf->capture_ns = 0;
  if (src->dec_running) {
    CaptureResult r = rtp_capture(p, src, vf);
    if (r == CAP_FRAME)
      (*frame_no)++;
    // The camera's time stayed on the ROV: RTP carries no capture clock
    // hellion reads yet, so this frame is stamped when it was decoded.
    return r;
  }
  if (src->bus) {
    FbusFrameInfo info;
    switch (fbus_reader_next(src->bus, vf->raw_planes, 500, &info)) {
    case FBUS_FRAME:
      vf->capture_ns = info.capture_mono_ns;
      *frame_no = info.frame_no; // keep the publisher's numbering
      return CAP_FRAME;
    case FBUS_TIMEOUT:
      return CAP_IDLE;
    case FBUS_GONE:
      return CAP_LOST;
    }
    return CAP_LOST;
  }
  bool ok = src->rpicam ? rpicam_in_capture(src->rpicam, vf)
                        : v4l2_in_capture(src->v4l2, vf);
  if (!ok)
    return CAP_LOST;
  (*frame_no)++;
  return CAP_FRAME;
}

static void *source_loop(void *arg) {
  Pipeline *p = (Pipeline *)arg;
  VideoFrame *vf = NULL;
  int64_t frame_no = 0;
  bool was_running = false;

  while (!stopping(p)) {
    Source src;
    if (!source_open(p, &src)) {
      if (nap(p, p->cfg.source == SRC_BUS ? 500 : source_retry_ms))
        break;
      continue;
    }

    if (!vf || vf->width != src.width || vf->height != src.height) {
      if (vf)
        printf("hellion: pipeline \"%s\": source is now %ux%u (was %ux%u); "
               "restarting its sinks\n",
               p->cfg.name, src.width, src.height, vf->width, vf->height);
      vf_free(vf);
      vf = vf_create(src.width, src.height, 0);
      if (!vf) {
        printf("hellion: pipeline \"%s\": out of memory\n", p->cfg.name);
        source_close(&src);
        break;
      }
      restart_sinks(p, src.width, src.height);
    }

    printf("hellion: pipeline \"%s\": %s connected at %ux%u\n", p->cfg.name,
           p->cfg.source_spec, src.width, src.height);
    set_source(p, "running", "");
    was_running = true;

    bool camera_restart = false;
    while (!stopping(p)) {
      CaptureResult r = source_capture(p, &src, vf, &frame_no);
      if (r == CAP_IDLE) {
        set_source(p, "idle", "connected, but no frames arriving");
        continue;
      }
      if (r == CAP_LOST)
        break;
      if (vf->capture_ns == 0)
        vf->capture_ns = vf_now_ns();

      pthread_mutex_lock(&p->lock);
      p->src_frames++;
      p->src_last_frame_ns = mono_ns();
      if (strcmp(p->src_state, "running") != 0) {
        snprintf(p->src_state, sizeof p->src_state, "running");
        p->src_detail[0] = '\0';
      }
      size_t n = p->sink_count;
      Sink *sinks[hcfg_max_sinks];
      memcpy(sinks, p->sinks, n * sizeof sinks[0]);
      bool new_camera = p->cam_gen != src.cam_gen;
      pthread_mutex_unlock(&p->lock);

      // Only this thread replaces the sinks, so the copies stay valid.
      // Every sink but the last gets a copy; the last gets the frame itself
      // (see sink_offer_swap), after which vf's raw planes are another
      // block -- the next capture goes there.
      for (size_t i = 0; i + 1 < n; i++)
        sink_offer(sinks[i], vf->raw_planes, frame_no, vf->capture_ns);
      if (n > 0)
        sink_offer_swap(sinks[n - 1], vf, frame_no);

      if (new_camera && src.v4l2) {
        apply_v4l2_camera(p, &src);
      } else if (new_camera && src.rpicam) {
        // rpicam-vid takes controls on its command line only. Announced:
        // it is a visible gap, and whoever moved a slider should know why
        // the picture blinked.
        printf("hellion: pipeline \"%s\": camera controls changed; "
               "restarting rpicam-vid to apply them\n",
               p->cfg.name);
        camera_restart = true;
        pthread_mutex_lock(&p->lock);
        p->cam_restarting = true;
        pthread_mutex_unlock(&p->lock);
        break;
      }
    }

    source_close(&src);
    if (camera_restart) {
      set_source(p, "waiting", "restarting the camera for new controls");
    } else if (!stopping(p) && was_running) {
      printf("hellion: pipeline \"%s\": lost %s; reconnecting\n", p->cfg.name,
             p->cfg.source_spec);
      set_source(p, "waiting", "lost; reconnecting");
    }
  }

  restart_sinks(p, 0, 0); // stops them
  vf_free(vf);
  return NULL;
}

// ---- Public -------------------------------------------------------------

bool pipeline_has_camera(const Pipeline *p) {
  return p->cfg.source == SRC_V4L2 || p->cfg.source == SRC_RPICAM;
}

void pipeline_set_camera(Pipeline *p, const CameraControls *c) {
  pthread_mutex_lock(&p->lock);
  if (p->cam_gen == 0 || !camera_ctrl_equal(&p->cam, c)) {
    p->cam = *c;
    p->cam_gen++;
  }
  pthread_mutex_unlock(&p->lock);
}

int pipeline_camera_wait(Pipeline *p, int timeout_ms, int *applied) {
  for (int waited = 0;; waited += 20) {
    pthread_mutex_lock(&p->lock);
    bool done = p->cam_done_gen == p->cam_gen;
    int n = p->cam_done_n;
    // "idle" is open but silent: the controls still go on.
    bool open = strcmp(p->src_state, "running") == 0 ||
                strcmp(p->src_state, "idle") == 0 || p->cam_restarting;
    pthread_mutex_unlock(&p->lock);
    if (done) {
      *applied = n;
      return 1;
    }
    if (!open)
      return 0;
    if (waited >= timeout_ms)
      return -1;
    struct timespec ts = {0, 20 * 1000000L};
    nanosleep(&ts, NULL);
  }
}

Pipeline *pipeline_start(const PipelineConfig *cfg) {
  return pipeline_start_with_camera(cfg, NULL);
}

Pipeline *pipeline_start_with_camera(const PipelineConfig *cfg,
                                     const CameraControls *camera) {
  Pipeline *p = (Pipeline *)calloc(1, sizeof *p);
  if (!p)
    return NULL;
  p->cfg = *cfg;
  camera_ctrl_init(&p->cam);
  if (camera) {
    p->cam = *camera;
    p->cam_gen = 1;
  }
  pthread_mutex_init(&p->lock, NULL);
  snprintf(p->src_state, sizeof p->src_state, "starting");
  if (pthread_create(&p->thread, NULL, source_loop, p) != 0) {
    printf("hellion: could not start pipeline \"%s\"\n", cfg->name);
    pthread_mutex_destroy(&p->lock);
    free(p);
    return NULL;
  }
  return p;
}

static void endpoint(HsEndpoint *e, const char *spec, const char *state,
                     const char *detail, uint64_t frames, uint64_t dropped,
                     double fps) {
  snprintf(e->spec, sizeof e->spec, "%.*s", hs_spec_len - 1, spec);
  snprintf(e->state, sizeof e->state, "%s", state);
  snprintf(e->detail, sizeof e->detail, "%s", detail);
  e->frames = frames;
  e->dropped = dropped;
  e->fps = fps;
}

static void endpoint_net(HsEndpoint *e, const RelayStats *rs) {
  e->net = true;
  e->kbps = rs->kbps;
  e->loss_pct = rs->loss_pct;
  e->packets = rs->packets;
  e->lost = rs->lost;
  e->idle_s = rs->idle_s;
}

void pipeline_status(Pipeline *p, HsPipeline *out) {
  uint64_t now = mono_ns();
  memset(out, 0, sizeof *out);
  snprintf(out->name, sizeof out->name, "%s", p->cfg.name);

  pthread_mutex_lock(&p->lock);
  out->width = p->width;
  out->height = p->height;
  endpoint(&out->source, p->cfg.source_spec, p->src_state, p->src_detail,
           p->src_frames, 0, fps_update(&p->src_fps, p->src_frames, now));
  // A camera that stops sending is only declared lost after v4l2_in has
  // waited out a possible hiccup (up to ~10s). Until then "running" at
  // 0 fps would mislead, so say what is actually known.
  if (strcmp(p->src_state, "running") == 0 && p->src_last_frame_ns &&
      now - p->src_last_frame_ns > 1000000000ull) {
    snprintf(out->source.state, sizeof out->source.state, "idle");
    snprintf(out->source.detail, sizeof out->source.detail,
             "no frames for %.0fs", (double)(now - p->src_last_frame_ns) / 1e9);
  }
  if (p->rtp_relay) {
    RelayStats rs;
    udp_relay_stats(p->rtp_relay, &rs);
    endpoint_net(&out->source, &rs);
    if (p->damage_last_window && strcmp(out->source.state, "running") == 0)
      snprintf(out->source.detail, sizeof out->source.detail,
               "decoder: %llu damaged-bitstream errors in the last %.0fs",
               (unsigned long long)p->damage_last_window,
               damage_summary_ns / 1e9);
  }

  // Sinks that failed to start are still listed, so they do not vanish
  // from the dashboard.
  out->sink_count = (uint32_t)p->cfg.sink_count;
  for (size_t i = 0; i < p->cfg.sink_count; i++) {
    const char *spec = p->cfg.sinks[i].spec;
    Sink *s = NULL;
    for (size_t j = 0; j < p->sink_count; j++)
      if (strcmp(p->sinks[j]->cfg.spec, spec) == 0)
        s = p->sinks[j];
    if (!s) {
      endpoint(&out->sinks[i], spec,
               p->width ? "error" : "waiting",
               p->width ? "did not start (see log)" : "waiting for the source",
               0, 0, 0);
      continue;
    }
    pthread_mutex_lock(&s->lock);
    endpoint(&out->sinks[i], spec, s->rep.state, s->rep.detail, s->frames,
             s->dropped, fps_update(&s->fps, s->frames, now));
    // A sink reports its state when it handles a frame, so with no frames
    // coming its last word ("sending") would stand. Say what is true.
    if (s->last_frame_ns && now - s->last_frame_ns > 2000000000ull) {
      snprintf(out->sinks[i].state, sizeof out->sinks[i].state, "idle");
      snprintf(out->sinks[i].detail, sizeof out->sinks[i].detail,
               "no frames from the source for %.0fs",
               (double)(now - s->last_frame_ns) / 1e9);
    }
    RelayStats rs;
    if (sink_impl_net(s->impl, &rs))
      endpoint_net(&out->sinks[i], &rs);
    pthread_mutex_unlock(&s->lock);
  }
  pthread_mutex_unlock(&p->lock);
}

UdpRelay *pipeline_rtp_relay(Pipeline *p) {
  if (!p)
    return NULL;
  pthread_mutex_lock(&p->lock);
  UdpRelay *r = p->rtp_relay;
  pthread_mutex_unlock(&p->lock);
  return r;
}

const PipelineConfig *pipeline_config(const Pipeline *p) {
  return p ? &p->cfg : NULL;
}

void pipeline_stop(Pipeline *p) {
  if (!p)
    return;
  __atomic_store_n(&p->stop, true, __ATOMIC_RELEASE);
  pthread_join(p->thread, NULL);
  udp_relay_close(p->rtp_relay);
  if (p->sdp_path[0])
    unlink(p->sdp_path);
  pthread_mutex_destroy(&p->lock);
  free(p);
}
