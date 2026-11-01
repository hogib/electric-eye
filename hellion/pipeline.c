#define _POSIX_C_SOURCE 200809L

#include "pipeline.h"
#include "fbus.h"
#include "rpicam_in.h"
#include "sinks.h"
#include "v4l2_in.h"
#include "video_frame.h"
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

// Asked of cameras; like eeye, a hint the driver may not honor exactly.
static const uint32_t camera_framerate = 30;
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
};

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
  snprintf(p->src_detail, sizeof p->src_detail, "%s", detail);
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
  SinkImpl *impl = sink_impl_open(&s->cfg, s->width, s->height, &rep);

  pthread_mutex_lock(&s->lock);
  s->rep = rep;
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
    if (impl) {
      ok = sink_impl_write(impl, &f, &rep);
    } else {
      // Opening failed outright; try again now and then.
      impl = sink_impl_open(&s->cfg, s->width, s->height, &rep);
    }

    pthread_mutex_lock(&s->lock);
    if (ok)
      s->frames++;
    s->rep = rep;
  }
  pthread_mutex_unlock(&s->lock);

  sink_impl_close(impl);
  return NULL;
}

static Sink *sink_start(const SinkConfig *cfg, uint32_t w, uint32_t h) {
  Sink *s = (Sink *)calloc(1, sizeof *s);
  if (!s)
    return NULL;
  s->cfg = *cfg;
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
    Sink *s = sink_start(&p->cfg.sinks[i], w, h);
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
  uint32_t width, height;
} Source;

static bool device_exists(const char *path) {
  struct stat st;
  return stat(path, &st) == 0;
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
        v4l2_in_open(c->source_arg, w, h, camera_framerate, 1, c->format);
    if (!src->v4l2) {
      set_source(p, "waiting", "camera would not open (see log)");
      return false;
    }
    break;
  case SRC_RPICAM:
    src->rpicam = rpicam_in_open(w, h, camera_framerate, 1, NULL, c->format);
    if (!src->rpicam) {
      set_source(p, "waiting", "rpicam-vid would not start (see log)");
      return false;
    }
    break;
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
  }
  src->width = w;
  src->height = h;
  return true;
}

static void source_close(Source *src) {
  v4l2_in_close(src->v4l2);
  rpicam_in_close(src->rpicam);
  fbus_reader_close(src->bus);
  *src = (Source){0};
}

typedef enum { CAP_FRAME, CAP_IDLE, CAP_LOST } CaptureResult;

static CaptureResult source_capture(Source *src, VideoFrame *vf,
                                    int64_t *frame_no) {
  vf->capture_ns = 0;
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

    uint8_t *const planes[3] = {vf->raw_planes[0], vf->raw_planes[1],
                                vf->raw_planes[2]};
    while (!stopping(p)) {
      CaptureResult r = source_capture(&src, vf, &frame_no);
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
      pthread_mutex_unlock(&p->lock);

      // Only this thread replaces the sinks, so the copies stay valid.
      for (size_t i = 0; i < n; i++)
        sink_offer(sinks[i], planes, frame_no, vf->capture_ns);
    }

    source_close(&src);
    if (!stopping(p) && was_running) {
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

Pipeline *pipeline_start(const PipelineConfig *cfg) {
  Pipeline *p = (Pipeline *)calloc(1, sizeof *p);
  if (!p)
    return NULL;
  p->cfg = *cfg;
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
    pthread_mutex_unlock(&s->lock);
  }
  pthread_mutex_unlock(&p->lock);
}

void pipeline_stop(Pipeline *p) {
  if (!p)
    return;
  __atomic_store_n(&p->stop, true, __ATOMIC_RELEASE);
  pthread_join(p->thread, NULL);
  pthread_mutex_destroy(&p->lock);
  free(p);
}
