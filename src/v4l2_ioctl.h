#pragma once
#include <errno.h>
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
