// _POSIX_C_SOURCE before any header include: nanosleep() and
// clock_gettime() under strict ISO mode (-std=c99). Same guard as
// v4l2_in.c.
#define _POSIX_C_SOURCE 200809L

#include "bus_in.h"
#include "fbus.h"
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

// How long a live but silent stream goes before it is mentioned, and how
// often bus_in_wait() repeats itself.
static const uint64_t idle_report_ns = 3000000000ull;
static const int wait_log_every_ms = 30000;

struct BusIn {
  char name[fbus_max_stream_name];
  FbusReader *reader;
  bool reported_idle;
};

static uint64_t mono_ns(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

static void sleep_ms(int ms) {
  struct timespec ts = {ms / 1000, (long)(ms % 1000) * 1000000L};
  nanosleep(&ts, NULL);
}

bool bus_in_wait(const char *name, const bool *is_running, uint32_t *width,
                 uint32_t *height) {
  int waited_ms = 0;
  FbusOpenResult last = FBUS_OPEN_OK;
  while (__atomic_load_n(is_running, __ATOMIC_SEQ_CST)) {
    FbusOpenResult why;
    FbusReader *r = fbus_reader_open(name, &why);
    if (r) {
      fbus_reader_geometry(r, width, height);
      fbus_reader_close(r);
      printf("Bus stream \"%s\" found at %ux%u\n", name, *width, *height);
      return true;
    }
    if (why != last || waited_ms % wait_log_every_ms == 0) {
      printf("Waiting for bus stream \"%s\" in %s (%s)...\n", name,
             fbus_dir(), fbus_open_result_str(why));
      last = why;
    }
    sleep_ms(200);
    waited_ms += 200;
  }
  return false;
}

BusIn *bus_in_open(const char *name, uint32_t width, uint32_t height,
                   bool log_failure, bool *wrong_size) {
  *wrong_size = false;
  FbusOpenResult why;
  FbusReader *r = fbus_reader_open(name, &why);
  if (!r) {
    if (log_failure)
      printf("Bus stream \"%s\" unavailable: %s\n", name,
             fbus_open_result_str(why));
    return NULL;
  }

  uint32_t w, h;
  fbus_reader_geometry(r, &w, &h);
  if (w != width || h != height) {
    printf("Bus stream \"%s\" is now %ux%u, but this run was sized for "
           "%ux%u; its frame pool cannot change size while running.\n",
           name, w, h, width, height);
    fbus_reader_close(r);
    *wrong_size = true;
    return NULL;
  }

  BusIn *in = (BusIn *)calloc(1, sizeof *in);
  if (!in) {
    fbus_reader_close(r);
    return NULL;
  }
  snprintf(in->name, sizeof in->name, "%s", name);
  in->reader = r;
  return in;
}

bool bus_in_capture(BusIn *in, VideoFrame *frame, const bool *is_running) {
  uint8_t *const planes[3] = {frame->raw_planes[0], frame->raw_planes[1],
                              frame->raw_planes[2]};
  uint64_t idle_since = mono_ns();

  while (__atomic_load_n(is_running, __ATOMIC_SEQ_CST)) {
    FbusFrameInfo info;
    switch (fbus_reader_next(in->reader, planes, 250, &info)) {
    case FBUS_FRAME:
      if (in->reported_idle) {
        printf("Bus stream \"%s\": frames flowing again\n", in->name);
        in->reported_idle = false;
      }
      frame->capture_ns = info.capture_mono_ns;
      frame->source_jpeg_len = 0;
      return true;
    case FBUS_TIMEOUT:
      if (!in->reported_idle && mono_ns() - idle_since > idle_report_ns) {
        printf("Bus stream \"%s\": its writer is running but sending no "
               "frames\n",
               in->name);
        in->reported_idle = true;
      }
      break;
    case FBUS_GONE:
      printf("Bus stream \"%s\" went away (its writer stopped or "
             "restarted)\n",
             in->name);
      return false;
    }
  }
  return false;
}

void bus_in_close(BusIn *in) {
  if (!in)
    return;
  fbus_reader_close(in->reader);
  free(in);
}
