#define _POSIX_C_SOURCE 200809L

#include "camera_ctrl.h"
#include "json_cursor.h"
#include "v4l2_ioctl.h"
#include <errno.h>
#include <stddef.h>
#include <linux/videodev2.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

bool camera_ctrl_parse(Cursor *c, CameraControls *out, char *err,
                       size_t err_cap) {
  if (!cur_expect(c, '{')) {
    snprintf(err, err_cap, "expected '{' for \"camera\"");
    return false;
  }
  skip_ws(c);

  char ch;
  if (cur_peek(c, &ch) && ch == '}') {
    c->pos++;
    return true; // an empty object is valid: it changes nothing
  }

  for (;;) {
    char key[32];
    skip_ws(c);
    if (!parse_string(c, key, sizeof key)) {
      snprintf(err, err_cap, "expected a quoted key in \"camera\"");
      return false;
    }
    skip_ws(c);
    if (!cur_expect(c, ':')) {
      snprintf(err, err_cap, "expected ':' after \"%s\" in \"camera\"", key);
      return false;
    }
    skip_ws(c);

    bool ok;
    int32_t lo = 0, hi = 0;
    int32_t *target = NULL;
    if (strcmp(key, "auto_exposure") == 0) {
      bool b;
      ok = parse_bool(c, &b);
      if (ok)
        out->auto_exposure = b ? 1 : 0;
    } else if (strcmp(key, "auto_white_balance") == 0) {
      bool b;
      ok = parse_bool(c, &b);
      if (ok)
        out->auto_white_balance = b ? 1 : 0;
    } else if (strcmp(key, "keep_framerate") == 0) {
      bool b;
      ok = parse_bool(c, &b);
      if (ok)
        out->keep_framerate = b ? 1 : 0;
    } else if (strcmp(key, "exposure") == 0) {
      target = &out->exposure;
      lo = 1;
      hi = 10000000; // 10s; past any sane video exposure
      ok = parse_i32(c, target);
    } else if (strcmp(key, "gain") == 0) {
      target = &out->gain;
      lo = 0;
      hi = 100;
      ok = parse_i32(c, target);
    } else if (strcmp(key, "white_balance") == 0) {
      target = &out->white_balance;
      lo = 1000;
      hi = 15000; // Kelvin, generously past any camera's range
      ok = parse_i32(c, target);
    } else if (strcmp(key, "brightness") == 0) {
      target = &out->brightness;
      lo = -100;
      hi = 100;
      ok = parse_i32(c, target);
    } else if (strcmp(key, "contrast") == 0) {
      target = &out->contrast;
      lo = 0;
      hi = 200;
      ok = parse_i32(c, target);
    } else if (strcmp(key, "saturation") == 0) {
      target = &out->saturation;
      lo = 0;
      hi = 200;
      ok = parse_i32(c, target);
    } else if (strcmp(key, "sharpness") == 0) {
      target = &out->sharpness;
      lo = 0;
      hi = 200;
      ok = parse_i32(c, target);
    } else {
      snprintf(err, err_cap, "unknown key \"%s\" in \"camera\"", key);
      return false;
    }

    if (!ok) {
      snprintf(err, err_cap, "invalid value for \"%s\" in \"camera\"", key);
      return false;
    }
    if (target && (*target < lo || *target > hi)) {
      snprintf(err, err_cap, "\"%s\" must be between %d and %d (got %d)", key, lo,
             hi, *target);
      return false;
    }

    skip_ws(c);
    if (!cur_peek(c, &ch)) {
      snprintf(err, err_cap, "unterminated \"camera\" object");
      return false;
    }
    if (ch == ',') {
      c->pos++;
      continue;
    }
    if (ch == '}') {
      c->pos++;
      return true;
    }
    snprintf(err, err_cap, "expected ',' or '}' after \"%s\" in \"camera\"", key);
    return false;
  }
}

bool camera_ctrl_parse_json(const char *json, size_t len, CameraControls *out,
                            char *err, size_t err_cap) {
  Cursor c = {.s = json, .len = len, .pos = 0};
  skip_ws(&c);
  if (!camera_ctrl_parse(&c, out, err, err_cap))
    return false;
  skip_ws(&c);
  if (c.pos != c.len) {
    snprintf(err, err_cap, "unexpected text after the \"camera\" object");
    return false;
  }
  return true;
}

int camera_ctrl_to_json(const CameraControls *c, char *out, size_t cap) {
  static const struct {
    const char *name;
    size_t offset;
    bool is_bool;
  } fields[] = {
      {"auto_exposure", offsetof(CameraControls, auto_exposure), true},
      {"auto_white_balance", offsetof(CameraControls, auto_white_balance), true},
      {"keep_framerate", offsetof(CameraControls, keep_framerate), true},
      {"exposure", offsetof(CameraControls, exposure), false},
      {"gain", offsetof(CameraControls, gain), false},
      {"white_balance", offsetof(CameraControls, white_balance), false},
      {"brightness", offsetof(CameraControls, brightness), false},
      {"contrast", offsetof(CameraControls, contrast), false},
      {"saturation", offsetof(CameraControls, saturation), false},
      {"sharpness", offsetof(CameraControls, sharpness), false},
  };
  size_t n = 0;
  int w = snprintf(out, cap, "{");
  if (w < 0 || (size_t)w >= cap)
    return -1;
  n = (size_t)w;
  bool first = true;
  for (size_t i = 0; i < sizeof fields / sizeof fields[0]; i++) {
    int32_t v = *(const int32_t *)((const char *)c + fields[i].offset);
    if (v == camera_ctrl_unset)
      continue;
    if (fields[i].is_bool)
      w = snprintf(out + n, cap - n, "%s\"%s\":%s", first ? "" : ",",
                   fields[i].name, v ? "true" : "false");
    else
      w = snprintf(out + n, cap - n, "%s\"%s\":%d", first ? "" : ",",
                   fields[i].name, (int)v);
    if (w < 0 || (size_t)w >= cap - n)
      return -1;
    n += (size_t)w;
    first = false;
  }
  w = snprintf(out + n, cap - n, "}");
  if (w < 0 || (size_t)w >= cap - n)
    return -1;
  return (int)(n + (size_t)w);
}

void camera_ctrl_init(CameraControls *c) {
  c->auto_exposure = camera_ctrl_unset;
  c->auto_white_balance = camera_ctrl_unset;
  c->exposure = camera_ctrl_unset;
  c->gain = camera_ctrl_unset;
  c->white_balance = camera_ctrl_unset;
  c->brightness = camera_ctrl_unset;
  c->contrast = camera_ctrl_unset;
  c->saturation = camera_ctrl_unset;
  c->sharpness = camera_ctrl_unset;
  c->keep_framerate = camera_ctrl_unset;
}

bool camera_ctrl_any_set(const CameraControls *c) {
  return c->auto_exposure != camera_ctrl_unset ||
         c->auto_white_balance != camera_ctrl_unset ||
         c->exposure != camera_ctrl_unset || c->gain != camera_ctrl_unset ||
         c->white_balance != camera_ctrl_unset ||
         c->brightness != camera_ctrl_unset ||
         c->contrast != camera_ctrl_unset ||
         c->saturation != camera_ctrl_unset ||
         c->sharpness != camera_ctrl_unset ||
         c->keep_framerate != camera_ctrl_unset;
}

bool camera_ctrl_equal(const CameraControls *a, const CameraControls *b) {
  return memcmp(a, b, sizeof *a) == 0;
}

// --- V4L2 ------------------------------------------------------------

// Maps a percent-style value onto whatever range this device actually
// reports. Ranges are per-camera -- the dev webcam's brightness is
// -64..64 and its gain 0..128, another will differ -- so a config
// carrying raw device numbers would not port between drones.
static int32_t scale_to_range(int32_t value, int32_t in_lo, int32_t in_hi,
                              int32_t dev_lo, int32_t dev_hi) {
  if (in_hi == in_lo)
    return dev_lo;
  int64_t span_in = (int64_t)in_hi - in_lo;
  int64_t span_dev = (int64_t)dev_hi - dev_lo;
  int64_t scaled =
      dev_lo + ((int64_t)(value - in_lo) * span_dev + span_in / 2) / span_in;
  if (scaled < dev_lo)
    scaled = dev_lo;
  if (scaled > dev_hi)
    scaled = dev_hi;
  return (int32_t)scaled;
}

typedef enum {
  MAP_DIRECT,  // value is already in the device's units
  MAP_PERCENT, // 0..100 across the device's range
  MAP_SIGNED,  // -100..100 across the device's range
  MAP_NEUTRAL, // 0..200 with 100 neutral, across the device's range
} CtrlMapping;

typedef struct {
  uint32_t id;
  const char *name;
  int32_t value;
  CtrlMapping mapping;
  bool is_auto_toggle; // written before the manual controls, see below
  // Set by default rather than by the operator: a camera without it is
  // normal and not worth a log line on every open.
  bool quiet_if_missing;
} PendingCtrl;

// Writes one control, clamping to the device's own range rather than
// failing when a value sits outside it: a config written for one camera
// should degrade sensibly on another, not refuse to apply.
//
// Returns true if the control was set (or deliberately skipped as
// unsupported), false only on a real failure worth reporting.
static bool apply_one(int fd, const PendingCtrl *pc, bool auto_still_on) {
  struct v4l2_queryctrl q = {.id = pc->id};
  if (xioctl(fd, VIDIOC_QUERYCTRL, &q) < 0) {
    if (!pc->quiet_if_missing)
      printf("camera: this camera has no \"%s\" control; skipping it\n",
             pc->name);
    return true; // not a failure -- cameras legitimately differ
  }
  if (q.flags & V4L2_CTRL_FLAG_DISABLED) {
    printf("camera: \"%s\" is disabled on this camera; skipping it\n",
           pc->name);
    return true;
  }

  int32_t target;
  switch (pc->mapping) {
  case MAP_PERCENT:
    target = scale_to_range(pc->value, 0, 100, q.minimum, q.maximum);
    break;
  case MAP_SIGNED:
    target = scale_to_range(pc->value, -100, 100, q.minimum, q.maximum);
    break;
  case MAP_NEUTRAL:
    target = scale_to_range(pc->value, 0, 200, q.minimum, q.maximum);
    break;
  case MAP_DIRECT:
  default:
    target = pc->value;
    break;
  }

  if (target < q.minimum || target > q.maximum) {
    int32_t clamped = target < q.minimum ? q.minimum : q.maximum;
    printf("camera: \"%s\" %d is outside this camera's range %d..%d; "
           "using %d\n",
           pc->name, target, q.minimum, q.maximum, clamped);
    target = clamped;
  }

  struct v4l2_control ctrl = {.id = pc->id, .value = target};
  if (xioctl(fd, VIDIOC_S_CTRL, &ctrl) < 0) {
    if (errno == EACCES || errno == EPERM) {
      // The trap this whole ordering exists for. A camera rejects a
      // manual value while its auto mode is on, and reports the *same*
      // errno a genuine permissions problem gives -- so saying
      // "permission denied" here would send an operator off to check
      // their video group membership when the real fault is sequencing.
      if (!pc->is_auto_toggle && auto_still_on) {
        printf("camera: \"%s\" was refused because this camera needs its "
               "automatic mode turned off first -- set the matching "
               "auto_* option to false alongside it\n",
               pc->name);
      } else {
        printf("camera: \"%s\" was refused by the driver (it may be "
               "read-only on this camera, or controlled automatically)\n",
               pc->name);
      }
      return false;
    }
    printf("camera: could not set \"%s\": %s\n", pc->name, strerror(errno));
    return false;
  }

  // Some UVC cameras accept a write and then ignore it. Reading back is
  // cheap and turns a silent no-op into something visible.
  struct v4l2_control readback = {.id = pc->id};
  if (xioctl(fd, VIDIOC_G_CTRL, &readback) == 0 &&
      readback.value != target) {
    printf("camera: \"%s\" was set to %d but reads back as %d; this "
           "camera may not fully support it\n",
           pc->name, target, readback.value);
  }
  return true;
}

int camera_ctrl_apply(int fd, const CameraControls *c) {
  if (fd < 0 || !camera_ctrl_any_set(c))
    return 0;

  PendingCtrl pending[10];
  size_t n = 0;

  // Auto toggles first, unconditionally. V4L2_CID_EXPOSURE_AUTO is a menu
  // (V4L2_EXPOSURE_MANUAL = 1, V4L2_EXPOSURE_APERTURE_PRIORITY = 3) rather
  // than a bool, so the config's true/false maps onto those rather than
  // onto 1/0.
  if (c->auto_exposure != camera_ctrl_unset) {
    pending[n++] = (PendingCtrl){
        .id = V4L2_CID_EXPOSURE_AUTO,
        .name = "auto_exposure",
        .value = c->auto_exposure ? V4L2_EXPOSURE_APERTURE_PRIORITY
                                  : V4L2_EXPOSURE_MANUAL,
        .mapping = MAP_DIRECT,
        .is_auto_toggle = true};
  }
  if (c->auto_white_balance != camera_ctrl_unset) {
    pending[n++] = (PendingCtrl){.id = V4L2_CID_AUTO_WHITE_BALANCE,
                                 .name = "auto_white_balance",
                                 .value = c->auto_white_balance ? 1 : 0,
                                 .mapping = MAP_DIRECT,
                                 .is_auto_toggle = true};
  }

  // Whether auto exposure may lower the frame rate. V4L2 names it the
  // other way round: EXPOSURE_AUTO_PRIORITY 1 lets exposure win.
  if (c->keep_framerate != camera_ctrl_unset) {
    pending[n++] = (PendingCtrl){.id = V4L2_CID_EXPOSURE_AUTO_PRIORITY,
                                 .name = "keep_framerate",
                                 .value = c->keep_framerate ? 0 : 1,
                                 .mapping = MAP_DIRECT,
                                 .is_auto_toggle = true,
                                 .quiet_if_missing = true};
  }

  // Then the manual values those toggles gate.
  if (c->exposure != camera_ctrl_unset) {
    // V4L2 counts exposure in 100us units; this config is in microseconds
    // (rpicam-vid's unit, and the more precise of the two) so it converts
    // here. Rounded rather than truncated so a value near a boundary does
    // not land a step low.
    pending[n++] = (PendingCtrl){.id = V4L2_CID_EXPOSURE_ABSOLUTE,
                                 .name = "exposure",
                                 .value = (c->exposure + 50) / 100,
                                 .mapping = MAP_DIRECT,
                                 .is_auto_toggle = false};
  }
  if (c->white_balance != camera_ctrl_unset) {
    pending[n++] = (PendingCtrl){.id = V4L2_CID_WHITE_BALANCE_TEMPERATURE,
                                 .name = "white_balance",
                                 .value = c->white_balance,
                                 .mapping = MAP_DIRECT,
                                 .is_auto_toggle = false};
  }
  if (c->gain != camera_ctrl_unset) {
    pending[n++] = (PendingCtrl){.id = V4L2_CID_GAIN,
                                 .name = "gain",
                                 .value = c->gain,
                                 .mapping = MAP_PERCENT,
                                 .is_auto_toggle = false};
  }
  if (c->brightness != camera_ctrl_unset) {
    pending[n++] = (PendingCtrl){.id = V4L2_CID_BRIGHTNESS,
                                 .name = "brightness",
                                 .value = c->brightness,
                                 .mapping = MAP_SIGNED,
                                 .is_auto_toggle = false};
  }
  if (c->contrast != camera_ctrl_unset) {
    pending[n++] = (PendingCtrl){.id = V4L2_CID_CONTRAST,
                                 .name = "contrast",
                                 .value = c->contrast,
                                 .mapping = MAP_NEUTRAL,
                                 .is_auto_toggle = false};
  }
  if (c->saturation != camera_ctrl_unset) {
    pending[n++] = (PendingCtrl){.id = V4L2_CID_SATURATION,
                                 .name = "saturation",
                                 .value = c->saturation,
                                 .mapping = MAP_NEUTRAL,
                                 .is_auto_toggle = false};
  }
  if (c->sharpness != camera_ctrl_unset) {
    pending[n++] = (PendingCtrl){.id = V4L2_CID_SHARPNESS,
                                 .name = "sharpness",
                                 .value = c->sharpness,
                                 .mapping = MAP_NEUTRAL,
                                 .is_auto_toggle = false};
  }

  // Whether an auto mode is still on, for diagnosing an EPERM below. A
  // config that never mentions auto_exposure leaves whatever the camera
  // was already doing, which on most webcams is auto -- so an unmentioned
  // toggle counts as still-on for the purpose of that message.
  bool exposure_auto_on = (c->auto_exposure == camera_ctrl_unset) ||
                          (c->auto_exposure != 0);
  bool wb_auto_on = (c->auto_white_balance == camera_ctrl_unset) ||
                    (c->auto_white_balance != 0);

  int applied = 0;
  for (size_t i = 0; i < n; ++i) {
    bool auto_on = false;
    if (pending[i].id == V4L2_CID_EXPOSURE_ABSOLUTE)
      auto_on = exposure_auto_on;
    else if (pending[i].id == V4L2_CID_WHITE_BALANCE_TEMPERATURE)
      auto_on = wb_auto_on;
    if (apply_one(fd, &pending[i], auto_on))
      applied++;
  }
  return applied;
}

// --- rpicam-vid ------------------------------------------------------

// rpicam takes its controls as spawn-time CLI flags rather than ioctls,
// so this only builds argv; rpicam_in.c decides when to restart the child
// to pick them up.
int camera_ctrl_rpicam_args(const CameraControls *c, char *argv[],
                            size_t argv_cap, char *storage,
                            size_t storage_cap) {
  size_t argc = 0;
  size_t used = 0;

  // Appends "--flag" plus a formatted value, or fails if either buffer is
  // exhausted -- silently truncating the argument list would hand the
  // camera a different configuration than the operator asked for.
  #define PUSH_FLAG(flag, fmt, val)                                          \
    do {                                                                     \
      if (argc + 2 > argv_cap)                                               \
        return -1;                                                           \
      argv[argc++] = (char *)(flag);                                         \
      int wrote = snprintf(storage + used, storage_cap - used, fmt, val);     \
      if (wrote < 0 || (size_t)wrote >= storage_cap - used)                   \
        return -1;                                                           \
      argv[argc++] = storage + used;                                         \
      used += (size_t)wrote + 1;                                             \
    } while (0)

  // Exposure. rpicam has no separate auto toggle: passing --shutter *is*
  // manual mode, and omitting it leaves the ISP's own auto exposure on.
  // So an explicit auto_exposure:false with no exposure value has nothing
  // to send, which is why the value is what gates this rather than the
  // toggle.
  if (c->auto_exposure != camera_ctrl_unset && c->auto_exposure == 0 &&
      c->exposure != camera_ctrl_unset) {
    PUSH_FLAG("--shutter", "%d", c->exposure); // already microseconds
  }

  if (c->gain != camera_ctrl_unset) {
    // rpicam --gain is a multiplier starting at 1.0; the config's 0-100
    // maps onto a useful 1.0-16.0 rather than the sensor's full range,
    // which at the top end is noise.
    double gain = 1.0 + (c->gain / 100.0) * 15.0;
    if (argc + 2 > argv_cap)
      return -1;
    argv[argc++] = (char *)"--gain";
    int wrote = snprintf(storage + used, storage_cap - used, "%.2f", gain);
    if (wrote < 0 || (size_t)wrote >= storage_cap - used)
      return -1;
    argv[argc++] = storage + used;
    used += (size_t)wrote + 1;
  }

  if (c->auto_white_balance != camera_ctrl_unset && c->auto_white_balance) {
    if (argc + 2 > argv_cap)
      return -1;
    argv[argc++] = (char *)"--awb";
    argv[argc++] = (char *)"auto";
  } else if (c->white_balance != camera_ctrl_unset) {
    // rpicam selects white balance by named mode rather than by Kelvin,
    // so the config's temperature picks the nearest mode. Coarser than
    // V4L2's continuous control, but it is what this backend offers.
    const char *mode = "auto";
    int32_t k = c->white_balance;
    if (k < 3200)
      mode = "incandescent";
    else if (k < 4000)
      mode = "tungsten";
    else if (k < 4800)
      mode = "fluorescent";
    else if (k < 5600)
      mode = "indoor";
    else if (k < 6200)
      mode = "daylight";
    else
      mode = "cloudy";
    if (argc + 2 > argv_cap)
      return -1;
    argv[argc++] = (char *)"--awb";
    argv[argc++] = (char *)mode;
  }

  // The image adjustments. rpicam takes floats with documented neutrals,
  // which is exactly what this config's percent-with-neutral ranges were
  // chosen to map onto cleanly.
  if (c->brightness != camera_ctrl_unset)
    PUSH_FLAG("--brightness", "%.2f", c->brightness / 100.0);
  if (c->contrast != camera_ctrl_unset)
    PUSH_FLAG("--contrast", "%.2f", c->contrast / 100.0);
  if (c->saturation != camera_ctrl_unset)
    PUSH_FLAG("--saturation", "%.2f", c->saturation / 100.0);
  if (c->sharpness != camera_ctrl_unset)
    PUSH_FLAG("--sharpness", "%.2f", c->sharpness / 100.0);

  #undef PUSH_FLAG
  return (int)argc;
}
