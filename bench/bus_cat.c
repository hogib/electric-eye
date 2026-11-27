// bus_cat: a frame bus stream's luma, frame after frame, on stdout.
//
//   bus_cat <stream> [--timeout seconds]
//
// For the latency bench (latency/g2g.py), which wants each decoded frame
// the moment an application could have it: this reads the bus the way
// any consumer does and writes the Y plane, width x height bytes, with
// no header. The size goes to stderr once, as "bus_cat: WxH".
//
// Waits for the stream to appear, and reopens it when its writer
// restarts. Ends when stdout closes, or after --timeout seconds with no
// stream and no frame (default 20).
#define _POSIX_C_SOURCE 200809L
#include "fbus.h"
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static double now_s(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return ts.tv_sec + ts.tv_nsec / 1e9;
}

int main(int argc, char **argv) {
  const char *stream = NULL;
  double timeout = 20;
  for (int i = 1; i < argc; i++) {
    if (!strcmp(argv[i], "--timeout") && i + 1 < argc)
      timeout = atof(argv[++i]);
    else if (!stream)
      stream = argv[i];
  }
  if (!stream) {
    fprintf(stderr, "usage: bus_cat <stream> [--timeout seconds]\n");
    return 2;
  }
  signal(SIGPIPE, SIG_IGN);

  FbusReader *r = NULL;
  uint8_t *buf = NULL;
  uint8_t *planes[3] = {0};
  uint32_t w = 0, h = 0;
  double last = now_s();
  for (;;) {
    if (now_s() - last > timeout) {
      fprintf(stderr, "bus_cat: nothing on \"%s\" for %.0fs\n", stream,
              timeout);
      return 1;
    }
    if (!r) {
      r = fbus_reader_open(stream, NULL);
      if (!r) {
        struct timespec nap = {0, 20000000};
        nanosleep(&nap, NULL);
        continue;
      }
      uint32_t nw, nh;
      fbus_reader_geometry(r, &nw, &nh);
      if (buf && (nw != w || nh != h)) {
        fprintf(stderr, "bus_cat: \"%s\" changed size\n", stream);
        return 1;
      }
      if (!buf) {
        w = nw;
        h = nh;
        // Y, U, V of a 4:2:2 frame in one block; only Y is written.
        buf = malloc((size_t)w * h * 2);
        if (!buf)
          return 1;
        planes[0] = buf;
        planes[1] = buf + (size_t)w * h;
        planes[2] = planes[1] + (size_t)(w / 2) * h;
        fprintf(stderr, "bus_cat: %ux%u\n", w, h);
      }
    }
    FbusReadResult res = fbus_reader_next(r, planes, 200, NULL);
    if (res == FBUS_TIMEOUT)
      continue;
    if (res != FBUS_FRAME) {
      fbus_reader_close(r);
      r = NULL;
      continue;
    }
    last = now_s();
    size_t n = (size_t)w * h, off = 0;
    while (off < n) {
      ssize_t k = write(STDOUT_FILENO, buf + off, n - off);
      if (k <= 0)
        return 0; // the reader went away
      off += (size_t)k;
    }
  }
}
