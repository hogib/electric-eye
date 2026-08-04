#pragma once
#include <stdbool.h>
#include <stdint.h>

/*
 * Auto-manages the v4l2loopback kernel module so `sudo modprobe
 * v4l2loopback ...` no longer has to be run by hand before every launch.
 *
 * virtual_cam_ensure_loaded() checks whether v4l2loopback is already loaded
 * (via /sys/module/v4l2loopback) and, if not, loads it with the given
 * video_nr/card_label via modprobe(8) -- this requires the process to have
 * CAP_SYS_MODULE (root, or a passwordless sudo/polkit rule scoped to
 * modprobe). If the module is already loaded, it is left exactly as found:
 * the kernel doesn't re-apply module parameters to an already-loaded
 * module, and doing so would risk pulling the device out from under some
 * *other* consumer that set it up independently -- see virtual_cam_unload().
 *
 * Either way, this also waits (briefly, bounded) for device_path (e.g.
 * "/dev/video10") to actually appear before returning -- module insertion
 * and udev's creation of the /dev node are two separate asynchronous
 * steps, so a caller that opens device_path immediately after this
 * returns can otherwise lose that race.
 *
 * Returns true once device_path exists and is ready to open (whether the
 * module already was, or because this call loaded it), false if loading
 * failed or device_path never appeared -- either way already logged with a
 * manual-fallback command.
 */
bool virtual_cam_ensure_loaded(uint32_t video_nr, const char *card_label,
                               const char *device_path);

/*
 * Unloads v4l2loopback via `modprobe -r`, but only if this same process is
 * the one that loaded it in virtual_cam_ensure_loaded() -- a no-op
 * otherwise (module was already present before this process started, or
 * loading never succeeded), so it's always safe to call unconditionally at
 * shutdown, including via atexit().
 *
 * Failure (e.g. EBUSY because some other process still has the device
 * open) is logged, not fatal -- shutdown proceeds either way.
 */
void virtual_cam_unload(void);
