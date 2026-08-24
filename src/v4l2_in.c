// _POSIX_C_SOURCE before any header include: -std=c23 puts glibc in strict
// ISO mode, which hides open()/mmap()/poll() and friends unless a
// feature-test macro asks for them explicitly. Same guard as
// config.c/eeye.c.
#define _POSIX_C_SOURCE 200809L

#include "v4l2_in.h"
#include "jpeg_decode.h"
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
#include <unistd.h>

// Same architecture detection as point_opps.c/conv.c (duplicated rather
// than shared via a header, matching those files' existing convention).
#if defined(__aarch64__)
#include <arm_neon.h>
#define GS_NEON_AARCH64 1
#elif defined(__arm__) && defined(__ARM_NEON)
#include <arm_neon.h>
#define GS_NEON_ARM32 1
#endif

constexpr uint32_t v4l2_in_max_buffers = 8;

struct V4l2In {
  int fd;
  // The geometry the *camera* delivers -- not the geometry of the frames
  // handed back by v4l2_in_read_frame(), which are these divided by
  // downscale. Every format negotiation and raw-buffer size check below
  // uses these; every write into a VideoFrame uses the frame's own
  // (already-divided) dimensions.
  uint32_t width;
  uint32_t height;
  uint32_t downscale;
  uint32_t n_buffers;
  struct {
    void *start;
    size_t length;
  } buffers[8]; // sized by v4l2_in_max_buffers
  uint32_t capture_format; // V4L2_PIX_FMT_MJPEG or V4L2_PIX_FMT_YUYV
  // Only set up when capture_format is MJPEG; YUYV needs no decode at all.
  // See jpeg_decode.h -- shared with the rpicam backend, which faces the
  // same MJPEG-to-I422 problem from a pipe rather than an MMAP buffer.
  JpegDecoder *jpeg_decoder;
};

// YUYV is packed 4:2:2: every 2 horizontal pixels are 4 bytes, Y0 U0 Y1 V0
// -- two luma samples sharing one chroma pair, which is exactly I422's
// subsampling, just interleaved instead of planar. No decode needed, only
// a deinterleave: split those 4 bytes into VideoFrame's three separate
// planes.
// Box-averages each NxN block of a packed YUYV frame straight into the
// frame's planar I422 buffers, so downscaling costs one pass rather than a
// deinterleave followed by a separate resample.
//
// Kept entirely separate from the N == 1 path below rather than
// generalizing that loop: N == 1 is both the common case and the one whose
// NEON kernel has been verified bit-exact under qemu, and folding a
// runtime divisor into it would have meant re-verifying that work to buy
// nothing (a box average with N == 1 is just a copy).
//
// Chroma indexing is the one part that isn't a direct translation of the
// luma loop. Output chroma sample cx covers output pixels 2*cx and 2*cx+1,
// hence source pixels [2*cx*N, 2*cx*N + 2N), which is exactly the N
// four-byte YUYV groups [cx*N, cx*N + N) -- each group carrying one U (at
// byte 1) and one V (at byte 3). So U and V average N*N samples apiece,
// the same count as luma, just gathered a group at a time.
static void downscale_yuyv_frame(const uint8_t *yuyv, VideoFrame *frame,
                                 uint32_t cap_width, uint32_t n) {
  const size_t src_stride = (size_t)cap_width * 2;
  const uint32_t half = n * n / 2; // rounding term for the averages below

  uint8_t *y_plane = frame->raw_planes[0];
  uint8_t *u_plane = frame->raw_planes[1];
  uint8_t *v_plane = frame->raw_planes[2];
  const size_t y_stride = frame->stride[0];
  const size_t c_stride = frame->stride[1]; // == stride[2]
  const uint32_t out_w = frame->width;
  const uint32_t chroma_w = out_w / 2; // exact: config.c validated evenness

#pragma omp parallel for
  for (uint32_t oy = 0; oy < frame->height; ++oy) {
    const uint8_t *src_block = yuyv + (size_t)oy * n * src_stride;
    uint8_t *y_row = y_plane + (size_t)oy * y_stride;

    for (uint32_t ox = 0; ox < out_w; ++ox) {
      uint32_t sum = 0;
      for (uint32_t dy = 0; dy < n; ++dy) {
        const uint8_t *r = src_block + (size_t)dy * src_stride;
        for (uint32_t dx = 0; dx < n; ++dx)
          sum += r[(size_t)(ox * n + dx) * 2];
      }
      y_row[ox] = (uint8_t)((sum + half) / (n * n));
    }

    uint8_t *u_row = u_plane + (size_t)oy * c_stride;
    uint8_t *v_row = v_plane + (size_t)oy * c_stride;
    for (uint32_t cx = 0; cx < chroma_w; ++cx) {
      uint32_t u_sum = 0, v_sum = 0;
      for (uint32_t dy = 0; dy < n; ++dy) {
        const uint8_t *r = src_block + (size_t)dy * src_stride;
        for (uint32_t g = 0; g < n; ++g) {
          const uint8_t *grp = r + (size_t)(cx * n + g) * 4;
          u_sum += grp[1];
          v_sum += grp[3];
        }
      }
      u_row[cx] = (uint8_t)((u_sum + half) / (n * n));
      v_row[cx] = (uint8_t)((v_sum + half) / (n * n));
    }
  }
}

static JpegDecodeResult unpack_yuyv_frame(const uint8_t *yuyv, size_t yuyv_size,
                                      VideoFrame *frame, uint32_t cap_width,
                                      uint32_t cap_height, uint32_t downscale) {
  // Sized from the capture geometry, not the frame's: with downscale > 1
  // the driver still hands over full-resolution bytes, and it is only the
  // output that shrinks.
  size_t expected = (size_t)cap_width * cap_height * 2;
  if (yuyv_size < expected) {
    // Unlike a JPEG payload, YUYV has no self-describing length -- a short
    // buffer here means the driver hasn't handed over a full frame yet
    // (or a USB glitch truncated one), not a format problem, so this is
    // worth retrying rather than failing outright.
    printf("v4l2_in: YUYV frame too short: got %zu bytes, expected %zu\n",
           yuyv_size, expected);
    return JPEG_DECODE_TRANSIENT_FAIL;
  }

  if (downscale > 1) {
    downscale_yuyv_frame(yuyv, frame, cap_width, downscale);
    return JPEG_DECODE_OK;
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
#if defined(GS_NEON_AARCH64) || defined(GS_NEON_ARM32)
    // YUYV's 4-byte groups (Y0 U0 Y1 V0) are exactly a 4-channel
    // interleaved format, which vld4q_u8 exists to de-interleave: one
    // 64-byte load yields 16 lanes each of Y0, U, Y1, V. U and V land
    // already in planar order, one store each. Y needs Y0/Y1 re-interleaved
    // back into a single row (Y0[0],Y1[0],Y0[1],Y1[1],...) -- vst2q_u8
    // does exactly that interleave on the way out, so no manual zip is
    // needed. Each chunk covers 32 luma pixels (16 pixel pairs) from 64
    // source bytes.
    //
    // Verified bit-exact against the scalar loop below over pseudo-random
    // frame content at widths both divisible and not divisible by 32 (so
    // the scalar remainder tail is also exercised), cross-compiled for
    // aarch64 and run under qemu-user emulation; this file has not been
    // built or run on real hardware.
    for (; x + 31 < frame->width; x += 32, cx += 16) {
      uint8x16x4_t g = vld4q_u8(&src[x * 2]);
      vst1q_u8(&u_row[cx], g.val[1]);
      vst1q_u8(&v_row[cx], g.val[3]);
      uint8x16x2_t y_pair = {{g.val[0], g.val[2]}};
      vst2q_u8(&y_row[x], y_pair);
    }
#endif
    for (; x + 1 < frame->width; x += 2, ++cx) {
      y_row[x] = src[x * 2];
      u_row[cx] = src[x * 2 + 1];
      y_row[x + 1] = src[x * 2 + 2];
      v_row[cx] = src[x * 2 + 3];
    }
    // An odd width leaves one trailing luma sample with no paired chroma
    // update of its own -- vf_create's chroma_width = (width+1)/2 already
    // reserves a slot for it, just copy the luma and leave that slot as
    // whatever the previous pair wrote.
    if (x < frame->width) {
      y_row[x] = src[x * 2];
    }
  }

  return JPEG_DECODE_OK;
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
    JpegDecodeResult result =
        (in->capture_format == V4L2_PIX_FMT_MJPEG)
            ? jpeg_decode_frame(in->jpeg_decoder, data, buf.bytesused, frame)
            : unpack_yuyv_frame(data, buf.bytesused, frame, in->width,
                                in->height, in->downscale);

    // The buffer goes back to the driver regardless of decode outcome --
    // skip this and streaming stalls silently once every buffer has been
    // dequeued and none returned.
    if (xioctl(in->fd, VIDIOC_QBUF, &buf) < 0) {
      printf("v4l2_in: VIDIOC_QBUF failed: %s\n", strerror(errno));
      return false;
    }

    if (result == JPEG_DECODE_OK)
      return true;
    if (result == JPEG_DECODE_FORMAT_MISMATCH)
      return false; // not transient; the decode path already said why

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

  jpeg_decoder_destroy(in->jpeg_decoder);

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

// How far a candidate's aspect ratio may sit from the best one found and
// still be considered on equal footing, in the same 1/10000 units
// aspect_error() returns. 100 is 1%, comfortably wider than the rounding
// noise between nominally-identical ratios (a 4:3 mode listed as 352x288
// is really 11:9, ~1.9% off, and correctly does *not* get grouped with a
// true 4:3 mode by this).
constexpr int64_t aspect_group_tolerance = 100;

// |w/h - want_w/want_h|, scaled by 10000 to stay in integer math. Cross-
// multiplied so neither division can round before the comparison.
static int64_t aspect_error(uint32_t w, uint32_t h, uint32_t want_w,
                            uint32_t want_h) {
  int64_t num = (int64_t)w * want_h - (int64_t)want_w * h;
  if (num < 0)
    num = -num;
  return (num * 10000) / ((int64_t)h * want_h);
}

typedef struct {
  uint32_t width, height;
  bool found;
  bool exact;
  // Ranking keys, only meaningful once found is true.
  int64_t aspect;
  uint64_t pixel_gap;
} SizeChoice;

// Folds one candidate mode into the running best. Ordering: an exact match
// beats everything; otherwise the smallest aspect error wins, and among
// candidates whose aspect errors are within aspect_group_tolerance of each
// other, the closest pixel count wins.
static void consider_size(SizeChoice *best, uint32_t w, uint32_t h,
                          uint32_t want_w, uint32_t want_h,
                          uint32_t downscale) {
  if (w == 0 || h == 0)
    return;
  // The same rule config.c applies to the requested size, and it has to
  // stay the same rule: this is the *negotiated* size the I422 chroma
  // halving and the YUYV box average actually run on, so a mode accepted
  // here but rejected there would produce a frame whose chroma planes
  // don't line up with its luma.
  //
  // Both dimensions, not just width: height doubled along with width when
  // 4:2:0 support landed, because that path line-doubles half-height
  // chroma into full-height planes, which only lands exactly on an even
  // output height.
  if (w % (downscale * 2u) != 0 || h % (downscale * 2u) != 0)
    return;

  bool exact = (w == want_w && h == want_h);
  int64_t asp = aspect_error(w, h, want_w, want_h);
  uint64_t want_px = (uint64_t)want_w * want_h;
  uint64_t px = (uint64_t)w * h;
  uint64_t gap = px > want_px ? px - want_px : want_px - px;

  if (!best->found || exact) {
    // First viable candidate, or an exact hit that ends the search.
    if (best->exact && !exact)
      return;
    *best = (SizeChoice){.width = w,
                         .height = h,
                         .found = true,
                         .exact = exact,
                         .aspect = asp,
                         .pixel_gap = gap};
    return;
  }
  if (best->exact)
    return;

  if (asp + aspect_group_tolerance < best->aspect ||
      (asp <= best->aspect + aspect_group_tolerance && gap < best->pixel_gap)) {
    *best = (SizeChoice){.width = w,
                         .height = h,
                         .found = true,
                         .exact = false,
                         // Keep the better (smaller) of the two aspect
                         // errors as the group's yardstick, so a later
                         // candidate is compared against the best ratio
                         // seen rather than against whichever one happened
                         // to win on pixel count.
                         .aspect = asp < best->aspect ? asp : best->aspect,
                         .pixel_gap = gap};
  }
}

// Walks VIDIOC_ENUM_FRAMESIZES for one pixel format. Drivers report either
// a discrete list or a single stepwise/continuous range; both are handled,
// the latter by deriving one best-fit candidate rather than materializing
// what can be an enormous range.
static void enumerate_sizes(int fd, uint32_t fourcc, SizeChoice *best,
                            uint32_t want_w, uint32_t want_h,
                            uint32_t downscale) {
  struct v4l2_frmsizeenum fse = {0};
  fse.pixel_format = fourcc;

  for (fse.index = 0; xioctl(fd, VIDIOC_ENUM_FRAMESIZES, &fse) == 0;
       fse.index++) {
    if (fse.type == V4L2_FRMSIZE_TYPE_DISCRETE) {
      consider_size(best, fse.discrete.width, fse.discrete.height, want_w,
                    want_h, downscale);
      continue;
    }

    // Stepwise/continuous: clamp the request into range, then round down
    // onto both the driver's step grid and our divisibility rule. Rounding
    // down (rather than to nearest) can only ever land back inside the
    // range, which rounding up could overshoot. Continuous is just
    // stepwise with a step of 1, and the kernel reports it that way.
    uint32_t sw = fse.stepwise.step_width ? fse.stepwise.step_width : 1;
    uint32_t sh = fse.stepwise.step_height ? fse.stepwise.step_height : 1;
    uint32_t w = want_w, h = want_h;
    if (w < fse.stepwise.min_width)
      w = fse.stepwise.min_width;
    if (w > fse.stepwise.max_width)
      w = fse.stepwise.max_width;
    if (h < fse.stepwise.min_height)
      h = fse.stepwise.min_height;
    if (h > fse.stepwise.max_height)
      h = fse.stepwise.max_height;
    w -= (w - fse.stepwise.min_width) % sw;
    h -= (h - fse.stepwise.min_height) % sh;
    // Walk down the step grid until the divisibility rule is satisfied
    // too. Bounded by the range itself, and cheap: the rule's period is at
    // most 16 pixels, so this gives up almost immediately when a step size
    // makes the two grids incompatible.
    while (w >= fse.stepwise.min_width && w % (downscale * 2u) != 0)
      w -= sw;
    while (h >= fse.stepwise.min_height && h % downscale != 0)
      h -= sh;
    if (w >= fse.stepwise.min_width && h >= fse.stepwise.min_height)
      consider_size(best, w, h, want_w, want_h, downscale);

    break; // a stepwise/continuous report is always the only entry
  }
}

bool v4l2_in_negotiate_size(const char *path, uint32_t *width,
                            uint32_t *height, uint32_t downscale) {
  if (downscale == 0)
    downscale = 1;

  int fd = open(path, O_RDWR | O_NONBLOCK);
  if (fd < 0) {
    // Not an error worth reporting here: the camera being absent at
    // startup is an expected, already-handled state (producer_loop retries
    // and logs its own message). Staying quiet avoids implying the
    // configured resolution was rejected when it was never tested.
    return false;
  }

  SizeChoice best = {0};
  enumerate_sizes(fd, V4L2_PIX_FMT_MJPEG, &best, *width, *height, downscale);
  enumerate_sizes(fd, V4L2_PIX_FMT_YUYV, &best, *width, *height, downscale);
  close(fd);

  if (!best.found) {
    // Either the driver doesn't implement ENUM_FRAMESIZES (legal, and some
    // don't) or every mode it offers fails the divisibility rule. Both
    // leave the configured size in force -- v4l2_in_open will then either
    // succeed anyway or report the exact mismatch itself, which is a
    // better-targeted message than anything guessable from here.
    printf("v4l2_in: %s offered no frame size usable at downscale %u; "
           "keeping the configured %ux%u\n",
           path, downscale, *width, *height);
    return false;
  }

  if (best.exact)
    return true; // configured size is available; nothing to say

  printf("v4l2_in: %s does not offer %ux%u; using %ux%u instead (closest "
         "available at the same aspect ratio, and divisible by downscale "
         "%u)\n",
         path, *width, *height, best.width, best.height, downscale);
  *width = best.width;
  *height = best.height;
  return true;
}

V4l2In *v4l2_in_open(const char *path, uint32_t width, uint32_t height,
                     uint32_t framerate_hint, uint32_t downscale) {
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
  in->downscale = downscale ? downscale : 1;
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
  // calloc(), which jpeg_decoder_destroy() already tolerates.
  if (in->capture_format == V4L2_PIX_FMT_MJPEG) {
    in->jpeg_decoder = jpeg_decoder_create(in->width, in->height);
    if (!in->jpeg_decoder) {
      v4l2_in_close(in);
      return NULL;
    }
  }

  // Probe: capture and decode one real frame before declaring open()
  // successful. This is what turns "camera's JPEG isn't actually 4:2:2"
  // into one clear failure right here, instead of a confusing per-frame
  // failure loop once producer_loop is already running.
  // Sized like every other frame this will fill -- at the *output*
  // geometry, not the capture geometry -- so the probe exercises the same
  // scaled-decode/box-average path the real frames take. Probing at full
  // size would leave the downscale path itself untested until the first
  // real frame, which is exactly what this probe exists to avoid.
  VideoFrame *probe =
      vf_create(in->width / in->downscale, in->height / in->downscale, 0);
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
