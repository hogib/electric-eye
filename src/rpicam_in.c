// _POSIX_C_SOURCE before any header include: -std=c23 puts glibc in strict
// ISO mode, which hides pipe()/fork()/kill() unless a feature-test macro
// asks for them explicitly. Same guard as v4l2_in.c/config.c/eeye.c.
#define _POSIX_C_SOURCE 200809L

#include "rpicam_in.h"
#include "camera_ctrl.h"
#include "jpeg_decode.h"
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

// JPEG quality handed to rpicam-vid. This is the *capture* quality, well
// above the preview tap's default (stream_quality, 60) because everything
// downstream -- the effect chain, the recording tap, the virtual camera --
// sees only what arrives here. Compression artifacts introduced at this
// point are permanent; the preview can afford them because it is a
// throwaway copy, this cannot.
constexpr unsigned rpicam_jpeg_quality = 93;

// Upper bound on one buffered JPEG. A 4:2:0 frame at quality 93 runs a few
// hundred KB even at 1080p, so this is generous headroom rather than a
// tight fit -- but it is a hard cap: a stream that never yields a complete
// frame within it is treated as a protocol failure instead of growing the
// buffer without limit.
constexpr size_t rpicam_max_frame_bytes = 16u * 1024 * 1024;

// How long to wait for data before concluding the child has wedged. The
// child is spawned with a framerate, so frames should arrive on that
// cadence; this is several frame intervals even at low rates.
constexpr int rpicam_poll_timeout_ms = 4000;

struct RpicamIn {
  pid_t child;
  int fd; // read end of the child's stdout

  uint32_t width; // source geometry, before downscale
  uint32_t height;
  uint32_t downscale;

  JpegDecoder *decoder;

  // Accumulates bytes from the pipe until a complete JPEG is present.
  // Whatever follows a decoded frame stays here and is carried into the
  // next call rather than discarded -- a single read() routinely straddles
  // a frame boundary.
  uint8_t *buf;
  size_t buf_cap;
  size_t buf_len;

  bool eof; // the child closed its stdout / exited
};

// Runs `rpicam-vid --list-cameras` and reports whether it named at least
// one camera. Both the binary being absent (not a Pi, or rpicam-apps not
// installed) and it running but finding nothing (no ribbon cable attached)
// answer the same question -- "is this backend the right one here?" -- so
// they collapse into one false rather than being distinguished.
bool rpicam_in_available(void) {
  int pipefd[2];
  if (pipe(pipefd) != 0)
    return false;

  pid_t pid = fork();
  if (pid < 0) {
    close(pipefd[0]);
    close(pipefd[1]);
    return false;
  }
  if (pid == 0) {
    close(pipefd[0]);
    dup2(pipefd[1], STDOUT_FILENO);
    dup2(pipefd[1], STDERR_FILENO);
    close(pipefd[1]);
    char *const argv[] = {"rpicam-vid", "--list-cameras", NULL};
    execvp(argv[0], argv);
    _exit(127);
  }

  close(pipefd[1]);
  char out[4096];
  size_t total = 0;
  ssize_t n;
  while (total + 1 < sizeof out &&
         (n = read(pipefd[0], out + total, sizeof out - 1 - total)) > 0)
    total += (size_t)n;
  out[total] = '\0';
  close(pipefd[0]);

  int status = 0;
  if (waitpid(pid, &status, 0) < 0)
    return false;
  if (!WIFEXITED(status) || WEXITSTATUS(status) != 0)
    return false;

  // rpicam-vid prints "Available cameras" followed by an indexed list when
  // it finds any, and "No cameras available!" when it doesn't. Keying on
  // the negative is the more robust of the two: the positive header's
  // exact wording has changed across releases, whereas a run that found
  // nothing is unambiguous.
  return strstr(out, "No cameras available") == NULL;
}

// Finds the end of the first complete JPEG in `data`, returning its total
// length, or 0 if it isn't all here yet.
//
// This walks marker segments to reach SOS and only then scans the
// entropy-coded data for EOI, rather than scanning the whole buffer for
// FF D9 outright. Inside entropy-coded data a literal FF is always stuffed
// (FF 00) or a restart marker (FF D0-D7), so FF D9 there genuinely is the
// end -- but *before* SOS, an APP segment carrying an embedded thumbnail
// or vendor blob can contain those two bytes as ordinary payload, and a
// naive scan would truncate the frame there.
//
// Returns SIZE_MAX if the data is not a JPEG at all (no SOI), which the
// caller treats as a stream-level failure rather than "need more bytes".
static size_t find_jpeg_end(const uint8_t *data, size_t size) {
  if (size < 2)
    return 0;
  if (data[0] != 0xFF || data[1] != 0xD8)
    return SIZE_MAX; // not SOI -- the stream is not what we think it is

  size_t pos = 2;
  // Marker-segment walk, up to SOS.
  while (pos + 4 <= size) {
    if (data[pos] != 0xFF)
      return SIZE_MAX; // desynchronized: a marker must start here
    uint8_t marker = data[pos + 1];
    if (marker == 0xFF) {
      ++pos; // fill byte before the real marker code
      continue;
    }
    if (marker == 0x01 || (marker >= 0xD0 && marker <= 0xD8)) {
      pos += 2; // markers with no length field (TEM, RSTn, SOI)
      continue;
    }
    if (marker == 0xD9)
      return pos + 2; // EOI before any SOS: empty but complete
    if (marker == 0xDA) {
      pos += 2 + (((size_t)data[pos + 2] << 8) | data[pos + 3]);
      break; // entropy-coded data starts here
    }
    size_t seg_len = ((size_t)data[pos + 2] << 8) | data[pos + 3];
    if (seg_len < 2)
      return SIZE_MAX; // malformed
    pos += 2 + seg_len;
  }
  if (pos >= size)
    return 0; // SOS header not fully buffered yet

  // Entropy-coded data: the only unstuffed FF sequences are markers.
  while (pos + 1 < size) {
    if (data[pos] != 0xFF) {
      ++pos;
      continue;
    }
    uint8_t marker = data[pos + 1];
    if (marker == 0x00 || marker == 0xFF) {
      pos += 2; // stuffed literal FF, or a fill byte
      continue;
    }
    if (marker >= 0xD0 && marker <= 0xD7) {
      pos += 2; // restart marker
      continue;
    }
    if (marker == 0xD9)
      return pos + 2; // EOI -- frame complete
    // Any other marker after SOS (e.g. a multi-scan progressive JPEG's
    // next SOS/DHT) means more segments follow; step over the marker and
    // keep going. rpicam-vid emits baseline JPEG so this is defensive.
    pos += 2;
  }
  return 0; // need more bytes
}

// Pulls more bytes from the pipe into in->buf. Returns false only on a
// hard failure (the child died, the pipe broke, the frame cap was hit);
// a timeout with no data is reported by the caller's own retry budget.
static bool fill_buffer(RpicamIn *in) {
  if (in->eof)
    return false;

  struct pollfd pfd = {.fd = in->fd, .events = POLLIN, .revents = 0};
  int pr = poll(&pfd, 1, rpicam_poll_timeout_ms);
  if (pr < 0) {
    if (errno == EINTR)
      return true; // nothing read, but nothing wrong either
    printf("rpicam_in: poll failed: %s\n", strerror(errno));
    return false;
  }
  if (pr == 0) {
    printf("rpicam_in: timed out waiting for data from rpicam-vid\n");
    return false;
  }

  if (in->buf_len == in->buf_cap) {
    if (in->buf_cap >= rpicam_max_frame_bytes) {
      printf("rpicam_in: no complete JPEG within %zu bytes; giving up on "
             "this stream\n",
             rpicam_max_frame_bytes);
      return false;
    }
    size_t want = in->buf_cap ? in->buf_cap * 2 : 256u * 1024;
    if (want > rpicam_max_frame_bytes)
      want = rpicam_max_frame_bytes;
    uint8_t *grown = realloc(in->buf, want);
    if (!grown) {
      printf("rpicam_in: out of memory growing the frame buffer\n");
      return false;
    }
    in->buf = grown;
    in->buf_cap = want;
  }

  ssize_t n = read(in->fd, in->buf + in->buf_len, in->buf_cap - in->buf_len);
  if (n < 0) {
    if (errno == EINTR || errno == EAGAIN)
      return true;
    printf("rpicam_in: read failed: %s\n", strerror(errno));
    return false;
  }
  if (n == 0) {
    printf("rpicam_in: rpicam-vid closed its output\n");
    in->eof = true;
    return false;
  }
  in->buf_len += (size_t)n;
  return true;
}

bool rpicam_in_capture(RpicamIn *in, VideoFrame *frame) {
  // Same retry budget and rationale as v4l2_in_capture(): an isolated
  // corrupt frame is absorbed, a format mismatch returns immediately.
  for (int attempt = 0; attempt < 5; attempt++) {
    size_t frame_len = find_jpeg_end(in->buf, in->buf_len);

    if (frame_len == SIZE_MAX) {
      printf("rpicam_in: rpicam-vid's output is not a JPEG stream\n");
      return false;
    }
    if (frame_len == 0) {
      if (!fill_buffer(in))
        return false;
      // Not a failed attempt -- no frame was available to try yet.
      --attempt;
      continue;
    }

    JpegDecodeResult result =
        jpeg_decode_frame(in->decoder, in->buf, frame_len, frame);

    // Consume this frame either way: leaving a frame that failed to decode
    // at the head of the buffer would make every subsequent attempt retry
    // the same bad bytes forever.
    memmove(in->buf, in->buf + frame_len, in->buf_len - frame_len);
    in->buf_len -= frame_len;

    if (result == JPEG_DECODE_OK)
      return true;
    if (result == JPEG_DECODE_FORMAT_MISMATCH)
      return false; // not transient; the decode path already said why

    printf("rpicam_in: dropped a frame (attempt %d/5)\n", attempt + 1);
  }

  printf("rpicam_in: too many consecutive capture failures; giving up\n");
  return false;
}

RpicamIn *rpicam_in_open(uint32_t width, uint32_t height,
                         uint32_t framerate_hint, uint32_t downscale,
                         const CameraControls *controls) {
  if (downscale == 0)
    downscale = 1;

  char width_arg[16], height_arg[16], fps_arg[16], quality_arg[16];
  snprintf(width_arg, sizeof width_arg, "%u", width);
  snprintf(height_arg, sizeof height_arg, "%u", height);
  snprintf(fps_arg, sizeof fps_arg, "%u", framerate_hint);
  snprintf(quality_arg, sizeof quality_arg, "%u", rpicam_jpeg_quality);

  // -t 0: run until killed. -n: no preview window (there is no display on
  // a submersible, and drawing one costs real CPU). --flush: push each
  // frame out as soon as it is encoded rather than letting it sit in a
  // stdio buffer, which otherwise adds latency proportional to the buffer
  // size -- exactly the wrong trade for a live pilot's view. -o -: stdout.
  // Room for the fixed flags above plus whatever camera controls add.
  // rpicam takes controls as spawn-time arguments rather than as
  // something settable on a running process, which is why they are built
  // here and why changing them means restarting the child.
  char *argv[48];
  char ctrl_storage[512];
  size_t argc = 0;
  argv[argc++] = "rpicam-vid";
  argv[argc++] = "--codec";
  argv[argc++] = "mjpeg";
  argv[argc++] = "--width";
  argv[argc++] = width_arg;
  argv[argc++] = "--height";
  argv[argc++] = height_arg;
  argv[argc++] = "--framerate";
  argv[argc++] = fps_arg;
  argv[argc++] = "--quality";
  argv[argc++] = quality_arg;
  argv[argc++] = "-t";
  argv[argc++] = "0";
  argv[argc++] = "-n";
  argv[argc++] = "--flush";

  if (controls && camera_ctrl_any_set(controls)) {
    int extra = camera_ctrl_rpicam_args(
        controls, argv + argc, (sizeof argv / sizeof argv[0]) - argc - 3,
        ctrl_storage, sizeof ctrl_storage);
    if (extra < 0) {
      // Truncating would silently hand the camera a different
      // configuration than the operator asked for.
      printf("rpicam_in: too many camera controls to pass; ignoring them\n");
    } else {
      argc += (size_t)extra;
    }
  }

  argv[argc++] = "-o";
  argv[argc++] = "-";
  argv[argc] = NULL;

  int pipefd[2];
  if (pipe(pipefd) != 0) {
    printf("rpicam_in: pipe() failed: %s\n", strerror(errno));
    return NULL;
  }

  pid_t pid = fork();
  if (pid < 0) {
    printf("rpicam_in: fork() failed: %s\n", strerror(errno));
    close(pipefd[0]);
    close(pipefd[1]);
    return NULL;
  }
  if (pid == 0) {
    close(pipefd[0]);
    dup2(pipefd[1], STDOUT_FILENO);
    close(pipefd[1]);
    // rpicam-vid writes progress and camera-tuning chatter to stderr on
    // every run; it is left attached to this process's stderr so a real
    // failure (missing camera, bad geometry) is visible to the operator
    // rather than swallowed.
    execvp(argv[0], argv);
    // Reached only if exec failed. Write directly rather than printf:
    // this is a forked child sharing the parent's stdio buffers.
    const char msg[] = "rpicam_in: could not execute rpicam-vid (is "
                       "rpicam-apps installed?)\n";
    ssize_t ignored = write(STDERR_FILENO, msg, sizeof msg - 1);
    (void)ignored;
    _exit(127);
  }

  close(pipefd[1]);

  RpicamIn *in = (RpicamIn *)calloc(1, sizeof(RpicamIn));
  if (!in) {
    close(pipefd[0]);
    kill(pid, SIGTERM);
    waitpid(pid, NULL, 0);
    return NULL;
  }
  in->child = pid;
  in->fd = pipefd[0];
  in->width = width;
  in->height = height;
  in->downscale = downscale;

  in->decoder = jpeg_decoder_create(width, height);
  if (!in->decoder) {
    rpicam_in_close(in);
    return NULL;
  }

  // Probe one real frame before declaring success, for the same reason
  // v4l2_in_open() does: this turns "rpicam-vid isn't installed", "no
  // camera attached", or "this sensor won't do that geometry" into one
  // clear failure here rather than a per-frame failure loop once
  // producer_loop is already running.
  //
  // Sized at the *output* geometry so the probe exercises the same scaled
  // decode the real frames take.
  VideoFrame *probe = vf_create(width / downscale, height / downscale, 0);
  if (!probe) {
    printf("rpicam_in: failed to allocate probe frame\n");
    rpicam_in_close(in);
    return NULL;
  }
  bool probe_ok = rpicam_in_capture(in, probe);
  vf_free(probe);
  if (!probe_ok) {
    printf("rpicam_in: initial capture probe failed; see the error above\n");
    rpicam_in_close(in);
    return NULL;
  }

  return in;
}

void rpicam_in_close(RpicamIn *in) {
  if (!in)
    return;

  if (in->fd >= 0)
    close(in->fd);

  if (in->child > 0) {
    // SIGTERM, then reap. rpicam-vid exits promptly on it; closing the
    // read end above would also make it die of SIGPIPE on its next write,
    // but waiting for that would mean blocking until it happens to write.
    kill(in->child, SIGTERM);
    waitpid(in->child, NULL, 0);
  }

  jpeg_decoder_destroy(in->decoder);
  free(in->buf);
  free(in);
}
