#pragma once
#include "video_frame.h"
#include <stdbool.h>
#include <stdint.h>

/*
 * The v4l2loopback output: the pilot's picture as a webcam any program can
 * open (OBS, ffplay, a browser).
 *
 * It is optional. eeye's own output is the frame bus; this device is for
 * programs that only know webcams. So a device that is missing at startup,
 * or that goes away mid-run, is logged and retried -- it never stops the
 * pipeline. That also makes restarts independent: under exclusive_caps=1,
 * a reader holding the device can keep a restarted eeye from reopening it
 * for a moment, and that must not count against systemd's restart budget.
 *
 * eeye does not load the kernel module. install.sh loads it at boot with
 * the right options; loading it here needed CAP_SYS_MODULE, and unloading
 * it on exit could pull the device out from under another program.
 *
 * Retry policy, so the log stays readable over a long dive:
 * - device node missing: checked every 2s, silently after the first
 *   explanation of why it might be missing;
 * - node present but open fails (busy, wrong module options): backs off
 *   from 2s to 30s, since each attempt logs v4l2_out_open()'s reason;
 * - a write fails: closed at once, retried 2s later.
 */

typedef struct VirtualCam VirtualCam;

// `path` "" (or NULL) means no v4l2 output; returns NULL then, and every
// call below treats NULL as a no-op. Never touches the device itself: the
// first write opens it.
VirtualCam *virtual_cam_create(const char *path, uint32_t width,
                               uint32_t height);

// Writes one frame, opening the device first if it is due another try.
// Returns true only if the frame was written.
bool virtual_cam_write(VirtualCam *vc, const VideoFrame *frame);

// Whether the device is open and the last write succeeded.
bool virtual_cam_connected(const VirtualCam *vc);

void virtual_cam_destroy(VirtualCam *vc);
