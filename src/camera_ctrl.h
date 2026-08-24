#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/*
 * Camera controls -- exposure, gain, white balance and the usual image
 * adjustments -- expressed once, portably, and applied by whichever
 * capture backend is in use.
 *
 * This matters more underwater than anywhere else: auto exposure hunts in
 * low contrast, auto white balance fails once everything is uniformly
 * blue-green, and no effect in the chain can recover detail the sensor
 * never captured in the first place.
 *
 * PORTABILITY
 * -----------
 * The two backends are not merely different APIs, they disagree on units
 * and ranges. V4L2 exposes integer controls whose ranges are per-device
 * (the dev webcam's brightness is -64..64, its gain 0..128; another camera
 * will differ) and its exposure is in 100us units. rpicam-vid takes float
 * CLI flags with fixed semantics and exposure in 1us.
 *
 * So the fields below are in stable, documented units of their own, and
 * each backend converts. Percent-style ranges with an explicit neutral,
 * rather than raw device units, are what let one config work on both a USB
 * webcam and a Pi camera -- the same reasoning behind light_level's
 * neutral-at-128 in config.h.
 *
 * UNSET vs ZERO
 * -------------
 * Every field is optional, and "not mentioned in the config" has to mean
 * "leave the camera alone" rather than "set to 0" -- otherwise omitting
 * brightness would silently drive it to one end of its range. Hence the
 * sentinel below rather than a plain 0 default.
 */

// Distinguishes "the operator did not mention this" from any value they
// could legitimately ask for. Chosen well outside every field's range.
constexpr int32_t camera_ctrl_unset = INT32_MIN;

typedef struct {
  // Auto toggles. These must be applied *before* their manual siblings:
  // a camera rejects a manual value while its auto mode is on, and
  // reports EPERM for it -- the same errno a genuine permissions problem
  // gives (verified on the dev webcam). See camera_ctrl_apply.
  int32_t auto_exposure;      // 0 = manual, 1 = auto
  int32_t auto_white_balance; // 0 = manual, 1 = auto

  int32_t exposure;      // microseconds; ignored by the camera while auto
  int32_t gain;          // 0-100, percent of the device's own range
  int32_t white_balance; // Kelvin (~2800-6500 typical)

  // Percent-style, 100 = neutral / leave as-is, so a config carrying
  // these means the same thing on any camera.
  int32_t brightness; // -100..100, 0 = neutral
  int32_t contrast;   // 0..200, 100 = neutral
  int32_t saturation; // 0..200, 100 = neutral
  int32_t sharpness;  // 0..200, 100 = neutral
} CameraControls;

// All fields unset -- the state meaning "change nothing".
void camera_ctrl_init(CameraControls *c);

// Whether any field is actually set. Lets callers skip the whole apply
// path (and its logging) when no controls are configured at all.
bool camera_ctrl_any_set(const CameraControls *c);

// Field-by-field equality, for deciding whether a reloaded config
// actually changed anything -- issuing ioctls per frame for an unchanged
// config would be pointless syscall traffic.
bool camera_ctrl_equal(const CameraControls *a, const CameraControls *b);

/*
 * Applies `c` to an open V4L2 device.
 *
 * Best-effort by design, and that is not laziness: cameras differ in
 * which controls they expose, so one unsupported or out-of-range field
 * must not abandon the rest. Unsupported controls are skipped, values
 * outside the device's own range are clamped to it, and both are logged
 * once rather than silently.
 *
 * Ordering is not optional. Auto toggles are written first, in the same
 * pass, because a manual value written while auto is on fails with EPERM
 * -- indistinguishable from a real permissions error unless you know to
 * expect it. When that happens anyway, it is reported as an ordering
 * problem, never as "check your video group".
 *
 * Returns the number of controls successfully applied.
 */
int camera_ctrl_apply(int fd, const CameraControls *c);

/*
 * Builds rpicam-vid arguments for `c`, appending to `argv` (which must
 * have room for `argv_cap` entries including the NULL rpicam_in.c adds).
 * Strings are written into `storage`, a caller-owned scratch buffer, so
 * this allocates nothing.
 *
 * Returns the number of argv entries written, or -1 if the buffers are
 * too small.
 */
int camera_ctrl_rpicam_args(const CameraControls *c, char *argv[],
                            size_t argv_cap, char *storage,
                            size_t storage_cap);
