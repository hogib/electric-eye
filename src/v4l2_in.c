// _POSIX_C_SOURCE before any header include: -std=c23 puts glibc in strict
// ISO mode, which hides open()/mmap()/poll() and friends unless a
// feature-test macro asks for them explicitly. Same guard as
// config.c/eeye.c.
#define _POSIX_C_SOURCE 200809L

#include "v4l2_in.h"
#include "v4l2_ioctl.h"
#include "video_frame.h"
#include <errno.h>
#include <fcntl.h>
#include <linux/videodev2.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <turbojpeg.h>
#include <unistd.h>

constexpr uint32_t v4l2_in_max_buffers = 8;

struct V4l2In {
  int fd;
  uint32_t width;
  uint32_t height;
  uint32_t n_buffers;
  struct {
    void *start;
    size_t length;
  } buffers[8]; // sized by v4l2_in_max_buffers
  uint32_t capture_format; // V4L2_PIX_FMT_MJPEG or V4L2_PIX_FMT_YUYV
  tjhandle jpeg_decoder;   // only set up when capture_format is MJPEG
};

typedef enum {
  DECODE_OK,
  DECODE_TRANSIENT_FAIL,  // malformed/corrupt JPEG payload -- worth a retry
  DECODE_FORMAT_MISMATCH, // JPEG's actual format disagrees with what open()
                          // already validated -- not transient, don't retry
} DecodeResult;

static DecodeResult decode_mjpeg_frame(V4l2In *in, const uint8_t *jpeg_data,
                                       size_t jpeg_size, VideoFrame *frame) {
  int jw, jh, jsubsamp, jcolorspace;
  if (tjDecompressHeader3(in->jpeg_decoder, jpeg_data, jpeg_size, &jw, &jh,
                          &jsubsamp, &jcolorspace) < 0) {
    printf("v4l2_in: tjDecompressHeader3 failed: %s\n",
           tjGetErrorStr2(in->jpeg_decoder));
    return DECODE_TRANSIENT_FAIL;
  }

  if ((uint32_t)jw != frame->width || (uint32_t)jh != frame->height) {
    printf("v4l2_in: frame dimensions changed mid-stream: got %dx%d, "
           "expected %ux%u\n",
           jw, jh, frame->width, frame->height);
    return DECODE_FORMAT_MISMATCH;
  }
  if (jsubsamp != TJSAMP_422) {
    printf("v4l2_in: camera's JPEG stream is not 4:2:2 subsampled "
           "(libjpeg-turbo reports subsamp=%d); this capture path requires "
           "4:2:2 to match VideoFrame's I422 layout -- see the note in "
           "v4l2_in.h\n",
           jsubsamp);
    return DECODE_FORMAT_MISMATCH;
  }

  // Decodes directly into the frame's own raw planes -- no RGB conversion,
  // no intermediate buffer. This is the entire reason for requiring 4:2:2:
  // libjpeg-turbo's plane layout for a 4:2:2 source matches VideoFrame's
  // I422 layout exactly, so there's nothing to convert.
  uint8_t *planes[3] = {frame->raw_planes[0], frame->raw_planes[1],
                        frame->raw_planes[2]};
  int strides[3] = {(int)frame->stride[0], (int)frame->stride[1],
                    (int)frame->stride[2]};

  if (tjDecompressToYUVPlanes(in->jpeg_decoder, jpeg_data, jpeg_size, planes,
                              (int)frame->width, strides, (int)frame->height,
                              0) < 0) {
    printf("v4l2_in: tjDecompressToYUVPlanes failed: %s\n",
           tjGetErrorStr2(in->jpeg_decoder));
    return DECODE_TRANSIENT_FAIL;
  }

  return DECODE_OK;
}

// YUYV is packed 4:2:2: every 2 horizontal pixels are 4 bytes, Y0 U0 Y1 V0
// -- two luma samples sharing one chroma pair, which is exactly I422's
// subsampling, just interleaved instead of planar. No decode needed, only
// a deinterleave: split those 4 bytes into VideoFrame's three separate
// planes.
static DecodeResult unpack_yuyv_frame(const uint8_t *yuyv, size_t yuyv_size,
                                      VideoFrame *frame) {
  size_t expected = (size_t)frame->width * frame->height * 2;
  if (yuyv_size < expected) {
    // Unlike a JPEG payload, YUYV has no self-describing length -- a short
    // buffer here means the driver hasn't handed over a full frame yet
    // (or a USB glitch truncated one), not a format problem, so this is
    // worth retrying rather than failing outright.
    printf("v4l2_in: YUYV frame too short: got %zu bytes, expected %zu\n",
           yuyv_size, expected);
    return DECODE_TRANSIENT_FAIL;
  }

  uint8_t *y_plane = frame->raw_planes[0];
  uint8_t *u_plane = frame->raw_planes[1];
  uint8_t *v_plane = frame->raw_planes[2];
  size_t y_stride = frame->stride[0];
  size_t c_stride = frame->stride[1]; // == stride[2]

  for (uint32_t row = 0; row < frame->height; ++row) {
    const uint8_t *src = yuyv + (size_t)row * frame->width * 2;
    uint8_t *y_row = y_plane + (size_t)row * y_stride;
    uint8_t *u_row = u_plane + (size_t)row * c_stride;
    uint8_t *v_row = v_plane + (size_t)row * c_stride;

    uint32_t x = 0, cx = 0;
    for (; x + 1 < frame->width; x += 2, ++cx) {
      y_row[x] = src[0];
      u_row[cx] = src[1];
      y_row[x + 1] = src[2];
      v_row[cx] = src[3];
      src += 4;
    }
    // An odd width leaves one trailing luma sample with no paired chroma
    // update of its own -- vf_create's chroma_width = (width+1)/2 already
    // reserves a slot for it, just copy the luma and leave that slot as
    // whatever the previous pair wrote.
    if (x < frame->width) {
      y_row[x] = src[0];
    }
  }

  return DECODE_OK;
}

bool v4l2_in_capture(V4l2In *in, VideoFrame *frame) {
  // A handful of retries absorbs an isolated corrupt USB transfer (real,
  // if uncommon, on actual hardware) without surfacing it as a pipeline
  // failure. A format mismatch is different -- retrying can't fix it, so
  // that path returns immediately below instead of burning through these.
  for (int attempt = 0; attempt < 5; attempt++) {
    struct pollfd pfd = {.fd = in->fd, .events = POLLIN, .revents = 0};
    int pr = poll(&pfd, 1, 2000);
    if (pr < 0) {
      if (errno == EINTR)
        continue;
      printf("v4l2_in: poll failed: %s\n", strerror(errno));
      return false;
    }
    if (pr == 0) {
      printf("v4l2_in: timed out waiting for a frame (attempt %d/5)\n",
             attempt + 1);
      continue;
    }

    struct v4l2_buffer buf = {0};
    buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    buf.memory = V4L2_MEMORY_MMAP;

    if (xioctl(in->fd, VIDIOC_DQBUF, &buf) < 0) {
      if (errno == EAGAIN)
        continue;
      printf("v4l2_in: VIDIOC_DQBUF failed: %s\n", strerror(errno));
      return false;
    }

    const uint8_t *data = (const uint8_t *)in->buffers[buf.index].start;
    DecodeResult result = (in->capture_format == V4L2_PIX_FMT_MJPEG)
                             ? decode_mjpeg_frame(in, data, buf.bytesused, frame)
                             : unpack_yuyv_frame(data, buf.bytesused, frame);

    // The buffer goes back to the driver regardless of decode outcome --
    // skip this and streaming stalls silently once every buffer has been
    // dequeued and none returned.
    if (xioctl(in->fd, VIDIOC_QBUF, &buf) < 0) {
      printf("v4l2_in: VIDIOC_QBUF failed: %s\n", strerror(errno));
      return false;
    }

    if (result == DECODE_OK)
      return true;
    if (result == DECODE_FORMAT_MISMATCH)
      return false; // not transient; see decode_mjpeg_frame's messages above

    printf("v4l2_in: dropped a frame (attempt %d/5)\n", attempt + 1);
  }

  printf("v4l2_in: too many consecutive capture failures; giving up\n");
  return false;
}

void v4l2_in_close(V4l2In *in) {
  if (!in)
    return;

  if (in->fd >= 0) {
    enum v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    xioctl(in->fd, VIDIOC_STREAMOFF, &type);
  }

  for (uint32_t i = 0; i < in->n_buffers; i++) {
    if (in->buffers[i].start && in->buffers[i].start != MAP_FAILED)
      munmap(in->buffers[i].start, in->buffers[i].length);
  }

  if (in->jpeg_decoder)
    tjDestroy(in->jpeg_decoder);

  if (in->fd >= 0)
    close(in->fd);

  free(in);
}

// Attempts VIDIOC_S_FMT for one pixel format at exactly width x height,
// logging why on failure -- useful on its own merits (distinguishing "the
// driver flatly rejected this format" from "it granted a different
// resolution instead" matters when debugging an unfamiliar camera), and
// specifically because v4l2_in_open's final fallback-exhausted message
// doesn't repeat these details itself.
//
// Unlike v4l2_out.c's equivalent, a dimension mismatch here has to be
// treated the same as an outright failure: the frame pool is sized from
// width/height, and any capture format that doesn't land on them exactly
// dooms every subsequent frame (see v4l2_in_open's other caller-facing
// mismatch handling for the full reasoning).
static bool try_set_format(int fd, uint32_t fourcc, uint32_t width,
                          uint32_t height, struct v4l2_format *fmt_out) {
  struct v4l2_format fmt = {0};
  fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
  fmt.fmt.pix.width = width;
  fmt.fmt.pix.height = height;
  fmt.fmt.pix.pixelformat = fourcc;
  fmt.fmt.pix.field = V4L2_FIELD_NONE;

  if (xioctl(fd, VIDIOC_S_FMT, &fmt) < 0) {
    printf("v4l2_in: %.4s not accepted at %ux%u: %s\n", (const char *)&fourcc,
           width, height, strerror(errno));
    return false;
  }
  if (fmt.fmt.pix.pixelformat != fourcc) {
    printf("v4l2_in: %.4s was not granted (driver substituted a different "
           "pixel format)\n",
           (const char *)&fourcc);
    return false;
  }
  if (fmt.fmt.pix.width != width || fmt.fmt.pix.height != height) {
    printf("v4l2_in: %.4s at %ux%u got %ux%u instead\n", (const char *)&fourcc,
           width, height, fmt.fmt.pix.width, fmt.fmt.pix.height);
    return false;
  }

  *fmt_out = fmt;
  return true;
}

V4l2In *v4l2_in_open(const char *path, uint32_t width, uint32_t height,
                     uint32_t framerate_hint) {
  int fd = open(path, O_RDWR | O_NONBLOCK);
  if (fd < 0) {
    switch (errno) {
    case ENOENT:
      printf("No such device: %s\n", path);
      break;
    case EACCES:
      printf("Permission denied opening %s\n"
             "Add your user to the 'video' group and log back in:\n"
             "  sudo usermod -aG video $USER\n",
             path);
      break;
    default:
      printf("Failed to open %s: %s\n", path, strerror(errno));
      break;
    }
    return NULL;
  }

  struct v4l2_capability cap = {0};
  if (xioctl(fd, VIDIOC_QUERYCAP, &cap) < 0) {
    printf("%s is not a V4L2 device (VIDIOC_QUERYCAP: %s)\n", path,
           strerror(errno));
    close(fd);
    return NULL;
  }

  // device_caps describes this specific node; capabilities is the union
  // across every node the driver owns. Same distinction as v4l2_out.c.
  uint32_t caps = (cap.capabilities & V4L2_CAP_DEVICE_CAPS) ? cap.device_caps
                                                            : cap.capabilities;
  if (!(caps & V4L2_CAP_VIDEO_CAPTURE)) {
    printf("%s does not support video capture (caps 0x%08x)\n", path, caps);
    close(fd);
    return NULL;
  }
  if (!(caps & V4L2_CAP_STREAMING)) {
    printf("%s does not support streaming I/O (caps 0x%08x); MMAP capture "
           "needs it\n",
           path, caps);
    close(fd);
    return NULL;
  }

  // MJPEG first: at USB2's ~24.6 MB/s isochronous ceiling, most webcams
  // only offer higher resolutions/framerates through it, reserving raw
  // YUYV for lower modes (see the framerate/bandwidth discussion this
  // capture path was designed around). YUYV needs no decode at all, so
  // it's still preferred over MJPEG at whatever resolution both are
  // actually available -- try_set_format's exact-match requirement means
  // this only ever falls through to YUYV when MJPEG genuinely isn't
  // offered at this resolution, not as a blanket preference.
  struct v4l2_format fmt;
  uint32_t capture_format;

  if (try_set_format(fd, V4L2_PIX_FMT_MJPEG, width, height, &fmt)) {
    capture_format = V4L2_PIX_FMT_MJPEG;
  } else if (try_set_format(fd, V4L2_PIX_FMT_YUYV, width, height, &fmt)) {
    capture_format = V4L2_PIX_FMT_YUYV;
    printf("%s: MJPEG not available at exactly %ux%u; falling back to "
           "YUYV\n",
           path, width, height);
  } else {
    // try_set_format() already logged why each attempt failed.
    printf("%s: neither MJPEG nor YUYV is available at exactly %ux%u\n",
           path, width, height);
    close(fd);
    return NULL;
  }

  // Best-effort: V4L2 does not require a driver to honor an exact
  // framerate, so a failure here is not treated as fatal.
  struct v4l2_streamparm parm = {0};
  parm.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
  parm.parm.capture.timeperframe.numerator = 1;
  parm.parm.capture.timeperframe.denominator = framerate_hint;
  xioctl(fd, VIDIOC_S_PARM, &parm);

  struct v4l2_requestbuffers req = {0};
  req.count = 4;
  req.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
  req.memory = V4L2_MEMORY_MMAP;

  if (xioctl(fd, VIDIOC_REQBUFS, &req) < 0) {
    printf("Failed to request capture buffers on %s: %s\n", path,
           strerror(errno));
    close(fd);
    return NULL;
  }
  if (req.count < 2) {
    printf("%s only granted %u capture buffer(s); need at least 2 for "
           "streaming\n",
           path, req.count);
    close(fd);
    return NULL;
  }
  if (req.count > v4l2_in_max_buffers) {
    printf("%s granted %u capture buffers, more than the %u this code "
           "supports\n",
           path, req.count, v4l2_in_max_buffers);
    close(fd);
    return NULL;
  }

  V4l2In *in = (V4l2In *)calloc(1, sizeof(V4l2In));
  if (!in) {
    close(fd);
    return NULL;
  }
  in->fd = fd;
  in->width = fmt.fmt.pix.width;
  in->height = fmt.fmt.pix.height;
  in->n_buffers = req.count;
  in->capture_format = capture_format;

  for (uint32_t i = 0; i < in->n_buffers; i++) {
    struct v4l2_buffer buf = {0};
    buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    buf.memory = V4L2_MEMORY_MMAP;
    buf.index = i;

    if (xioctl(fd, VIDIOC_QUERYBUF, &buf) < 0) {
      printf("VIDIOC_QUERYBUF failed for buffer %u: %s\n", i,
             strerror(errno));
      v4l2_in_close(in);
      return NULL;
    }

    void *start = mmap(NULL, buf.length, PROT_READ | PROT_WRITE, MAP_SHARED,
                       fd, buf.m.offset);
    if (start == MAP_FAILED) {
      printf("mmap failed for buffer %u: %s\n", i, strerror(errno));
      v4l2_in_close(in);
      return NULL;
    }
    in->buffers[i].start = start;
    in->buffers[i].length = buf.length;

    if (xioctl(fd, VIDIOC_QBUF, &buf) < 0) {
      printf("VIDIOC_QBUF failed for buffer %u: %s\n", i, strerror(errno));
      v4l2_in_close(in);
      return NULL;
    }
  }

  enum v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
  if (xioctl(fd, VIDIOC_STREAMON, &type) < 0) {
    printf("VIDIOC_STREAMON failed on %s: %s\n", path, strerror(errno));
    v4l2_in_close(in);
    return NULL;
  }

  // YUYV needs no decoder at all -- in->jpeg_decoder stays NULL from
  // calloc(), which v4l2_in_close() already checks for before tjDestroy().
  if (in->capture_format == V4L2_PIX_FMT_MJPEG) {
    in->jpeg_decoder = tjInitDecompress();
    if (!in->jpeg_decoder) {
      printf("tjInitDecompress failed: %s\n", tjGetErrorStr());
      v4l2_in_close(in);
      return NULL;
    }
  }

  // Probe: capture and decode one real frame before declaring open()
  // successful. This is what turns "camera's JPEG isn't actually 4:2:2"
  // into one clear failure right here, instead of a confusing per-frame
  // failure loop once producer_loop is already running.
  VideoFrame *probe = vf_create(in->width, in->height, 0);
  if (!probe) {
    printf("Failed to allocate probe frame\n");
    v4l2_in_close(in);
    return NULL;
  }
  bool probe_ok = v4l2_in_capture(in, probe);
  vf_free(probe);
  if (!probe_ok) {
    printf("Initial capture probe failed; see the error above\n");
    v4l2_in_close(in);
    return NULL;
  }

  return in;
}
