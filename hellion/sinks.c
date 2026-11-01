#define _POSIX_C_SOURCE 200809L

#include "sinks.h"
#include "fbus.h"
#include "video_frame.h"
#include "virtual_cam.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct SinkImpl {
  SinkKind kind;
  FbusWriter *bus;
  VirtualCam *vcam;
  char arg[hcfg_max_spec];
};

static void report(SinkReport *rep, const char *state, const char *detail) {
  snprintf(rep->state, sizeof rep->state, "%s", state);
  snprintf(rep->detail, sizeof rep->detail, "%s", detail);
}

SinkImpl *sink_impl_open(const SinkConfig *cfg, uint32_t width,
                         uint32_t height, SinkReport *rep) {
  SinkImpl *s = (SinkImpl *)calloc(1, sizeof *s);
  if (!s) {
    report(rep, "error", "out of memory");
    return NULL;
  }
  s->kind = cfg->kind;
  snprintf(s->arg, sizeof s->arg, "%s", cfg->arg);

  switch (cfg->kind) {
  case SINK_BUS:
    s->bus = fbus_writer_open(cfg->arg, width, height);
    if (!s->bus) {
      report(rep, "error", "could not create the stream (see log)");
      free(s);
      return NULL;
    }
    report(rep, "publishing", "");
    break;
  case SINK_V4L2:
    s->vcam = virtual_cam_create(cfg->arg, width, height);
    report(rep, "waiting", "not opened yet");
    break;
  }
  return s;
}

bool sink_impl_write(SinkImpl *s, const SinkFrame *f, SinkReport *rep) {
  switch (s->kind) {
  case SINK_BUS:
    fbus_writer_publish(s->bus, f->planes, f->frame_no, f->capture_ns);
    report(rep, "publishing", "");
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
      report(rep, "writing", "");
    else
      report(rep, "waiting", "device missing or busy; retrying (see log)");
    return ok;
  }
  }
  return false;
}

void sink_impl_close(SinkImpl *s) {
  if (!s)
    return;
  fbus_writer_close(s->bus);
  virtual_cam_destroy(s->vcam);
  free(s);
}
