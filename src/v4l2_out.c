#include "v4l2_out.h"
#include "video_frame.h"
#include <errno.h>
#include <fcntl.h>
#include <linux/videodev2.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

struct V4l2Out {
  int fd;
  size_t frame_size; // What the driver settled on, not what we asked for.
  uint32_t width;
  uint32_t height;
};

/*
 * ioctl() restarts are not automatic: a signal arriving mid-call surfaces as
 * EINTR and the request never reached the driver. Every ioctl here goes
 * through this wrapper so a stray signal can't be mistaken for a device error.
 */
static int xioctl(int fd, unsigned long request, void *arg) {
  int r;
  do {
    r = ioctl(fd, request, arg);
  } while (r == -1 && errno == EINTR);
  return r;
}

V4l2Out *v4l2_out_open(const char *path, uint32_t width, uint32_t height,
                       uint32_t fourcc) {
  // O_RDWR rather than O_WRONLY: V4L2 only requires drivers to accept O_RDWR,
  // and support for write-only opens is optional and inconsistent.
  int fd = open(path, O_RDWR);
  if (fd < 0) {
    switch (errno) {
    case ENOENT:
      printf("No such device: %s\n"
             "The v4l2loopback module does not appear to be loaded. Try:\n"
             "  sudo modprobe v4l2loopback video_nr=10 "
             "card_label=\"VirtualCam\" exclusive_caps=1\n",
             path);
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

  // device_caps describes this specific node; capabilities is the union across
  // every node the driver owns, so it can advertise OUTPUT for a node that
  // does not have it. Only trust device_caps when the driver says it is valid.
  uint32_t caps = (cap.capabilities & V4L2_CAP_DEVICE_CAPS) ? cap.device_caps
                                                            : cap.capabilities;
  if (!(caps & V4L2_CAP_VIDEO_OUTPUT)) {
    // With exclusive_caps=1 the loopback advertises OUTPUT only until a
    // consumer opens it for reading, after which later openers see CAPTURE.
    printf("%s does not accept video output (caps 0x%08x).\n"
           "If a consumer already has the device open, exclusive_caps=1 will "
           "have flipped it to capture-only.\n",
           path, caps);
    close(fd);
    return NULL;
  }

  // Zeroing matters: v4l2_format carries reserved fields the driver requires
  // to be zero, and stack garbage there earns an EINVAL at best.
  struct v4l2_format fmt = {0};
  fmt.type = V4L2_BUF_TYPE_VIDEO_OUTPUT;
  fmt.fmt.pix.width = width;
  fmt.fmt.pix.height = height;
  fmt.fmt.pix.pixelformat = fourcc;
  fmt.fmt.pix.field = V4L2_FIELD_NONE;
  fmt.fmt.pix.bytesperline = width; // Y-plane stride for planar formats.
  // The effects operate on full-range luma (see y_plain_max_jpeg == 255), so
  // declaring a limited-range colorspace here would invite consumers to remap
  // 16-235 and wash the picture out.
  fmt.fmt.pix.colorspace = V4L2_COLORSPACE_JPEG;

  if (xioctl(fd, VIDIOC_S_FMT, &fmt) < 0) {
    printf("Failed to set output format on %s: %s\n", path, strerror(errno));
    close(fd);
    return NULL;
  }

  // S_FMT negotiates rather than assigns: the driver is free to hand back
  // different dimensions or a different frame size. Believe what came back.
  if (fmt.fmt.pix.width != width || fmt.fmt.pix.height != height) {
    printf("Driver adjusted output geometry: asked %ux%u, got %ux%u\n", width,
           height, fmt.fmt.pix.width, fmt.fmt.pix.height);
  }
  if (fmt.fmt.pix.pixelformat != fourcc) {
    printf("Driver refused the requested pixel format on %s.\n", path);
    close(fd);
    return NULL;
  }
  if (fmt.fmt.pix.sizeimage == 0) {
    printf("Driver reported a zero frame size on %s.\n", path);
    close(fd);
    return NULL;
  }

  V4l2Out *out = (V4l2Out *)calloc(1, sizeof(V4l2Out));
  if (!out) {
    close(fd);
    return NULL;
  }

  out->fd = fd;
  out->frame_size = fmt.fmt.pix.sizeimage;
  out->width = fmt.fmt.pix.width;
  out->height = fmt.fmt.pix.height;

  return out;
}

size_t v4l2_out_frame_size(const V4l2Out *out) {
  return out ? out->frame_size : 0;
}

bool v4l2_out_write(V4l2Out *out, const VideoFrame *frame) {
  if (!out || !frame || !frame->pixel_data)
    return false;

  size_t total = frame->plane_sizes[0] + frame->plane_sizes[1] +
                 frame->plane_sizes[2];

  // A size disagreement desyncs every subsequent frame and shows up as rolling
  // or skewed video rather than as an error, so refuse rather than write.
  if (total != out->frame_size) {
    printf("Frame size mismatch: driver expects %zu bytes, frame holds %zu\n",
           out->frame_size, total);
    return false;
  }

  // vf_create() carves all three planes out of one allocation, so the whole
  // frame goes out in a single write() instead of one call per plane.
  const uint8_t *p = frame->pixel_data;
  size_t remaining = total;

  while (remaining > 0) {
    ssize_t n = write(out->fd, p, remaining);
    if (n < 0) {
      if (errno == EINTR)
        continue; // Interrupted before writing anything; retry.
      printf("Write to virtual camera failed: %s\n", strerror(errno));
      return false;
    }
    p += (size_t)n;
    remaining -= (size_t)n; // write() may satisfy only part of the request.
  }

  return true;
}

void v4l2_out_close(V4l2Out *out) {
  if (!out)
    return;

  if (out->fd >= 0)
    close(out->fd);

  free(out);
}
