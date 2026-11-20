#define _POSIX_C_SOURCE 200809L

#include "h264_src.h"
#include "v4l2_ioctl.h"
#include <fcntl.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

enum { max_buffers = 32, want_buffers = 8 };

// A camera that delivers nothing for this long has stopped (unplugged
// mid-stream on some hubs, or wedged): close it and start again.
static const uint64_t silent_ns = 10000000000ull;

struct H264Src {
  // Camera.
  int fd; // -1 for a file
  char name[128];
  uint32_t width, height;
  uint32_t n_buffers;
  struct {
    void *start;
    size_t length;
  } buffers[max_buffers];
  struct v4l2_buffer held; // the buffer out between read and done
  bool holding;
  bool no_keyframe_control; // found out on the first request
  uint64_t last_au_ns;
  uint64_t bad_buffers;

  // File.
  bool is_file;
  uint8_t *file;
  size_t file_len;
  size_t file_pos; // the next access unit
  uint64_t period_ns;
  uint64_t next_due_ns; // 0 = now
};

static uint64_t mono_ns(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

// ---- Annex B --------------------------------------------------------------

// The offset of the next 00 00 01 at or after `i`, or `len`.
static size_t find_start_code(const uint8_t *b, size_t len, size_t i) {
  for (; i + 3 <= len; i++)
    if (b[i] == 0 && b[i + 1] == 0 && b[i + 2] == 1)
      return i;
  return len;
}

size_t h264_next_access_unit(const uint8_t *buf, size_t len, size_t pos) {
  bool have_slice = false;
  size_t i = find_start_code(buf, len, pos);
  while (i + 3 < len) {
    unsigned type = buf[i + 3] & 0x1f;
    bool slice = type >= 1 && type <= 5;
    // first_mb_in_slice is the slice header's first field, Exp-Golomb
    // coded: zero is a single 1 bit.
    bool first_slice = slice && i + 4 < len && (buf[i + 4] & 0x80);
    // Types that may only come before a picture's first slice: SEI, SPS,
    // PPS, the delimiter, and 14-18 (prefix NAL, subset SPS, ...).
    bool leads = (type >= 6 && type <= 9) || (type >= 14 && type <= 18);
    if (have_slice && (first_slice || leads)) {
      // A four-byte start code's first zero belongs to it.
      return i > pos && buf[i - 1] == 0 ? i - 1 : i;
    }
    if (slice)
      have_slice = true;
    i = find_start_code(buf, len, i + 3);
  }
  return len;
}

bool h264_is_keyframe(const uint8_t *au, size_t len) {
  for (size_t i = find_start_code(au, len, 0); i + 3 < len;
       i = find_start_code(au, len, i + 3))
    if ((au[i + 3] & 0x1f) == 5)
      return true;
  return false;
}

// ---- Camera ---------------------------------------------------------------

// The driver's capture time on CLOCK_MONOTONIC, or 0 (see v4l2_in.c).
static uint64_t driver_timestamp_ns(const struct v4l2_buffer *buf) {
#if defined(V4L2_BUF_FLAG_TIMESTAMP_MASK) &&                                   \
    defined(V4L2_BUF_FLAG_TIMESTAMP_MONOTONIC)
  if ((buf->flags & V4L2_BUF_FLAG_TIMESTAMP_MASK) ==
          V4L2_BUF_FLAG_TIMESTAMP_MONOTONIC &&
      (buf->timestamp.tv_sec > 0 || buf->timestamp.tv_usec > 0))
    return (uint64_t)buf->timestamp.tv_sec * 1000000000u +
           (uint64_t)buf->timestamp.tv_usec * 1000u;
#else
  (void)buf;
#endif
  return 0;
}

static bool queue_all(H264Src *s) {
  for (uint32_t i = 0; i < s->n_buffers; i++) {
    struct v4l2_buffer buf = {0};
    buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    buf.memory = V4L2_MEMORY_MMAP;
    buf.index = i;
    if (xioctl(s->fd, VIDIOC_QBUF, &buf) < 0) {
      printf("h264_src: %s: VIDIOC_QBUF failed for buffer %u: %s\n", s->name,
             i, strerror(errno));
      return false;
    }
  }
  return true;
}

static bool stream_on(H264Src *s) {
  enum v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
  if (xioctl(s->fd, VIDIOC_STREAMON, &type) < 0) {
    printf("h264_src: %s: VIDIOC_STREAMON failed: %s\n", s->name,
           strerror(errno));
    return false;
  }
  s->last_au_ns = mono_ns();
  return true;
}

H264Src *h264_src_open_camera(const char *device, uint32_t width,
                              uint32_t height, uint32_t framerate) {
  int fd = open(device, O_RDWR | O_NONBLOCK | O_CLOEXEC);
  if (fd < 0) {
    printf("h264_src: cannot open %s: %s%s\n", device, strerror(errno),
           errno == EACCES ? " (is this user in the 'video' group?)" : "");
    return NULL;
  }
  H264Src *s = (H264Src *)calloc(1, sizeof *s);
  if (!s) {
    close(fd);
    return NULL;
  }
  s->fd = fd;
  snprintf(s->name, sizeof s->name, "%s", device);

  struct v4l2_capability cap = {0};
  if (xioctl(fd, VIDIOC_QUERYCAP, &cap) < 0) {
    printf("h264_src: %s is not a V4L2 device (VIDIOC_QUERYCAP: %s)\n",
           device, strerror(errno));
    goto fail;
  }
  uint32_t caps = v4l2_node_caps(&cap);
  if (!(caps & V4L2_CAP_VIDEO_CAPTURE) || !(caps & V4L2_CAP_STREAMING)) {
    printf("h264_src: %s does not capture with streaming I/O (caps "
           "0x%08x)\n",
           device, caps);
    goto fail;
  }

  struct v4l2_format fmt = {0};
  fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
  fmt.fmt.pix.width = width;
  fmt.fmt.pix.height = height;
  fmt.fmt.pix.pixelformat = V4L2_PIX_FMT_H264;
  fmt.fmt.pix.field = V4L2_FIELD_ANY;
  // A driver without the format answers with one it does have rather
  // than an error, so the answer is what has to be checked.
  if (xioctl(fd, VIDIOC_S_FMT, &fmt) < 0 ||
      fmt.fmt.pix.pixelformat != V4L2_PIX_FMT_H264) {
    printf("h264_src: %s does not offer H.264. On many cameras it is "
           "another node of the same camera; list them with\n"
           "  v4l2-ctl --list-devices\n"
           "and each one's formats with\n"
           "  v4l2-ctl -d /dev/videoN --list-formats-ext\n",
           device);
    goto fail;
  }
  s->width = fmt.fmt.pix.width;
  s->height = fmt.fmt.pix.height;
  if (s->width != width || s->height != height)
    printf("h264_src: %s: no H.264 at %ux%u; the camera gives %ux%u\n",
           device, width, height, s->width, s->height);

  // Best-effort, as in v4l2_in.c.
  struct v4l2_streamparm parm = {0};
  parm.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
  parm.parm.capture.timeperframe.numerator = 1;
  parm.parm.capture.timeperframe.denominator = framerate ? framerate : 30;
  xioctl(fd, VIDIOC_S_PARM, &parm);
  double fps = 0;
  if (xioctl(fd, VIDIOC_G_PARM, &parm) == 0 &&
      parm.parm.capture.timeperframe.numerator)
    fps = (double)parm.parm.capture.timeperframe.denominator /
          parm.parm.capture.timeperframe.numerator;

  struct v4l2_requestbuffers req = {0};
  req.count = want_buffers;
  req.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
  req.memory = V4L2_MEMORY_MMAP;
  if (xioctl(fd, VIDIOC_REQBUFS, &req) < 0 || req.count < 2 ||
      req.count > max_buffers) {
    printf("h264_src: %s: could not get capture buffers (%s; %u granted)\n",
           device, strerror(errno), req.count);
    goto fail;
  }
  for (uint32_t i = 0; i < req.count; i++) {
    struct v4l2_buffer buf = {0};
    buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    buf.memory = V4L2_MEMORY_MMAP;
    buf.index = i;
    if (xioctl(fd, VIDIOC_QUERYBUF, &buf) < 0) {
      printf("h264_src: %s: VIDIOC_QUERYBUF failed: %s\n", device,
             strerror(errno));
      goto fail;
    }
    void *start =
        mmap(NULL, buf.length, PROT_READ, MAP_SHARED, fd, buf.m.offset);
    if (start == MAP_FAILED) {
      printf("h264_src: %s: mmap failed: %s\n", device, strerror(errno));
      goto fail;
    }
    s->buffers[i].start = start;
    s->buffers[i].length = buf.length;
    s->n_buffers = i + 1;
  }
  if (!queue_all(s) || !stream_on(s))
    goto fail;

  printf("h264_src: %s: the camera's own H.264 at %ux%u", device, s->width,
         s->height);
  if (fps > 0)
    printf(", %.4g fps", fps);
  printf(" (sent as it is: no decode, no encode)\n");
  return s;

fail:
  h264_src_close(s);
  return NULL;
}

static H264SrcResult camera_read(H264Src *s, const uint8_t **au, size_t *len,
                                 uint64_t *capture_ns, int timeout_ms) {
  for (;;) {
    struct pollfd pfd = {.fd = s->fd, .events = POLLIN};
    int pr = poll(&pfd, 1, timeout_ms);
    if (pr < 0) {
      if (errno == EINTR)
        continue;
      printf("h264_src: %s: poll failed: %s\n", s->name, strerror(errno));
      return H264_SRC_LOST;
    }
    if (pr == 0) {
      if (mono_ns() - s->last_au_ns > silent_ns) {
        printf("h264_src: %s: nothing from the camera for %.0fs\n", s->name,
               silent_ns / 1e9);
        return H264_SRC_LOST;
      }
      return H264_SRC_IDLE;
    }
    if (pfd.revents & (POLLERR | POLLHUP | POLLNVAL) &&
        !(pfd.revents & POLLIN)) {
      // An unplugged camera, usually. (POLLERR also when not streaming.)
      printf("h264_src: %s: the device reports an error; reopening\n",
             s->name);
      return H264_SRC_LOST;
    }

    struct v4l2_buffer buf = {0};
    buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    buf.memory = V4L2_MEMORY_MMAP;
    if (xioctl(s->fd, VIDIOC_DQBUF, &buf) < 0) {
      if (errno == EAGAIN)
        continue;
      printf("h264_src: %s: VIDIOC_DQBUF failed: %s\n", s->name,
             strerror(errno));
      return H264_SRC_LOST;
    }
    bool bad = buf.index >= s->n_buffers || buf.bytesused == 0 ||
               buf.bytesused > s->buffers[buf.index].length;
#ifdef V4L2_BUF_FLAG_ERROR
    // A USB transfer that went wrong: the picture is damaged. Sending it
    // would only give the decoder something to complain about.
    bad = bad || (buf.flags & V4L2_BUF_FLAG_ERROR);
#endif
    if (bad) {
      s->bad_buffers++;
      if (s->bad_buffers == 1 || s->bad_buffers % 100 == 0)
        printf("h264_src: %s: %llu damaged or empty buffers from the camera "
               "so far (dropped)\n",
               s->name, (unsigned long long)s->bad_buffers);
      if (buf.index >= s->n_buffers || xioctl(s->fd, VIDIOC_QBUF, &buf) < 0)
        return H264_SRC_LOST;
      continue;
    }
    s->held = buf;
    s->holding = true;
    s->last_au_ns = mono_ns();
    *au = (const uint8_t *)s->buffers[buf.index].start;
    *len = buf.bytesused;
    uint64_t t = driver_timestamp_ns(&buf);
    *capture_ns = t ? t : s->last_au_ns;
    return H264_SRC_AU;
  }
}

static bool camera_keyframe(H264Src *s) {
#ifdef V4L2_CID_MPEG_VIDEO_FORCE_KEY_FRAME
  if (!s->no_keyframe_control) {
    struct v4l2_control c = {.id = V4L2_CID_MPEG_VIDEO_FORCE_KEY_FRAME,
                             .value = 1};
    if (xioctl(s->fd, VIDIOC_S_CTRL, &c) == 0)
      return true;
    s->no_keyframe_control = true;
    printf("h264_src: %s has no force-key-frame control; restarting its "
           "stream instead whenever a keyframe is wanted\n",
           s->name);
  }
#endif
  // Stopping hands every buffer back to us; they all go in again.
  enum v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
  if (xioctl(s->fd, VIDIOC_STREAMOFF, &type) < 0) {
    printf("h264_src: %s: VIDIOC_STREAMOFF failed: %s\n", s->name,
           strerror(errno));
    return false;
  }
  return queue_all(s) && stream_on(s);
}

// ---- File -----------------------------------------------------------------

H264Src *h264_src_open_file(const char *path, uint32_t framerate) {
  FILE *f = fopen(path, "rbe");
  if (!f) {
    printf("h264_src: cannot open %s: %s\n", path, strerror(errno));
    return NULL;
  }
  H264Src *s = (H264Src *)calloc(1, sizeof *s);
  if (!s) {
    fclose(f);
    return NULL;
  }
  s->is_file = true;
  s->fd = -1;
  struct stat st;
  // 256MB is minutes of video; this is for clips.
  if (fstat(fileno(f), &st) != 0 || !S_ISREG(st.st_mode) ||
      st.st_size <= 0 || st.st_size > (256 << 20) ||
      !(s->file = (uint8_t *)malloc((size_t)st.st_size)) ||
      fread(s->file, 1, (size_t)st.st_size, f) != (size_t)st.st_size) {
    printf("h264_src: cannot read %s (a regular file up to 256MB)\n", path);
    fclose(f);
    h264_src_close(s);
    return NULL;
  }
  fclose(f);
  s->file_len = (size_t)st.st_size;
  snprintf(s->name, sizeof s->name, "%s", path);
  s->period_ns = 1000000000ull / (framerate ? framerate : 30);

  size_t first_end = h264_next_access_unit(s->file, s->file_len, 0);
  if (!h264_is_keyframe(s->file, first_end)) {
    printf("h264_src: %s is not Annex B H.264 starting with a keyframe (a "
           "raw .h264 stream, not an MP4)\n",
           path);
    h264_src_close(s);
    return NULL;
  }
  return s;
}

static H264SrcResult file_read(H264Src *s, const uint8_t **au, size_t *len,
                               uint64_t *capture_ns, int timeout_ms) {
  uint64_t now = mono_ns();
  if (s->next_due_ns > now) {
    uint64_t wait = s->next_due_ns - now;
    uint64_t limit = (uint64_t)(timeout_ms > 0 ? timeout_ms : 0) * 1000000ull;
    bool late = wait > limit;
    if (late)
      wait = limit;
    struct timespec ts = {(time_t)(wait / 1000000000ull),
                          (long)(wait % 1000000000ull)};
    nanosleep(&ts, NULL);
    if (late)
      return H264_SRC_IDLE;
    now = mono_ns();
  }
  if (s->file_pos >= s->file_len)
    s->file_pos = 0;
  size_t end = h264_next_access_unit(s->file, s->file_len, s->file_pos);
  *au = s->file + s->file_pos;
  *len = end - s->file_pos;
  *capture_ns = now;
  s->file_pos = end;
  // On the beat while it can be kept; from now once a read comes more
  // than a period late (no burst to catch up).
  if (s->next_due_ns == 0 || s->next_due_ns + s->period_ns < now)
    s->next_due_ns = now;
  s->next_due_ns += s->period_ns;
  return H264_SRC_AU;
}

// ---- Common ---------------------------------------------------------------

void h264_src_geometry(const H264Src *s, uint32_t *width, uint32_t *height) {
  *width = s->width;
  *height = s->height;
}

int h264_src_fd(const H264Src *s) { return s ? s->fd : -1; }

H264SrcResult h264_src_read(H264Src *s, const uint8_t **au, size_t *len,
                            uint64_t *capture_ns, int timeout_ms) {
  return s->is_file ? file_read(s, au, len, capture_ns, timeout_ms)
                 : camera_read(s, au, len, capture_ns, timeout_ms);
}

void h264_src_done(H264Src *s) {
  if (!s->holding)
    return;
  s->holding = false;
  // A failure shows up as the next read losing the camera.
  xioctl(s->fd, VIDIOC_QBUF, &s->held);
}

bool h264_src_keyframe(H264Src *s) {
  if (s->is_file) {
    s->file_pos = 0;
    return true;
  }
  if (s->holding)
    return false;
  return camera_keyframe(s);
}

void h264_src_close(H264Src *s) {
  if (!s)
    return;
  if (!s->is_file && s->fd >= 0) {
    enum v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    xioctl(s->fd, VIDIOC_STREAMOFF, &type);
    for (uint32_t i = 0; i < s->n_buffers; i++)
      munmap(s->buffers[i].start, s->buffers[i].length);
    close(s->fd);
  }
  free(s->file);
  free(s);
}
