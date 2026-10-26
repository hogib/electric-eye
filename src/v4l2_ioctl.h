#pragma once
#include <errno.h>
#include <linux/videodev2.h>
#include <stdint.h>
#include <sys/ioctl.h>

/*
 * ioctl() restarts are not automatic: a signal arriving mid-call surfaces as
 * EINTR and the request never reached the driver. Every V4L2 ioctl, on
 * either the capture or output side, goes through this so a stray signal
 * can't be mistaken for a device error.
 */
static inline int xioctl(int fd, unsigned long request, void *arg) {
  int r;
  do {
    r = ioctl(fd, request, arg);
  } while (r == -1 && errno == EINTR);
  return r;
}

/*
 * The capabilities of this device node alone. `capabilities` is the union
 * across every node the driver owns, so it can advertise a direction this
 * node lacks; `device_caps` narrows it to this one, when the driver flags
 * it valid. Kernel headers older than 3.3 (Debian 7's) have neither the
 * flag nor the field -- but the ABI put device_caps where reserved[0] was,
 * so a binary built against them still finds it there on a newer kernel.
 */
static inline uint32_t v4l2_node_caps(const struct v4l2_capability *cap) {
#ifdef V4L2_CAP_DEVICE_CAPS
  if (cap->capabilities & V4L2_CAP_DEVICE_CAPS)
    return cap->device_caps;
#else
  if (cap->capabilities & 0x80000000u)
    return cap->reserved[0];
#endif
  return cap->capabilities;
}
