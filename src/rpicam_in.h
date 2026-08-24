#pragma once
#include "video_frame.h"
#include <stdbool.h>
#include <stdint.h>

/*
 * Capture backend for Raspberry Pi CSI camera modules (the ribbon-cable
 * ones), as opposed to the USB/UVC webcams v4l2_in.c handles.
 *
 * Why this exists at all, rather than pointing v4l2_in.c at the Pi
 * camera's device node: on Pi 5 there is no node to point it at. The
 * rp1-cfe driver exposes the CSI sensor as *raw Bayer only* -- no MJPEG,
 * no YUYV -- and expects the pipeline to be configured through the Media
 * Controller API. Turning Bayer into a viewable image means black level,
 * demosaic, white balance, lens shading and tone mapping, which on this
 * platform is the ISP's job and is driven by libcamera. Raspberry Pi's own
 * guidance is not to drive Pi 5 cameras through V4L2 directly.
 *
 * So this backend delegates to `rpicam-vid`, the supported path, and reads
 * its MJPEG output from a pipe. That buys the whole ISP chain (and the
 * per-sensor tuning files) for free, which is emphatically not something
 * worth reimplementing here.
 *
 * MJPEG rather than `--codec yuv420`, deliberately: rpicam-vid's raw YUV
 * output is stride-padded (Y rows rounded up to a multiple of 64 bytes,
 * U/V to 32) with no framing or geometry in the stream, so a consumer has
 * to know the padding rule to unpack it -- the cause of a long tail of
 * "corrupt raw output" reports upstream. MJPEG is self-delimiting and
 * carries its own dimensions, so a frame either decodes correctly or fails
 * loudly. It costs a decode this project was already paying on the UVC
 * MJPEG path anyway.
 *
 * Note that rpicam-vid's MJPEG is 4:2:0 subsampled, not the 4:2:2 that
 * most UVC webcams produce -- jpeg_decode.c handles both.
 */

typedef struct RpicamIn RpicamIn;

/*
 * Spawns `rpicam-vid` streaming MJPEG at width x height x framerate and
 * returns a handle reading its output. Validates one real frame before
 * returning, so a wrong geometry or a missing camera fails once, here,
 * rather than as a confusing per-frame decode loop later.
 *
 * `downscale` follows the same contract as v4l2_in_open(): frames handed
 * back are width/downscale x height/downscale. It is applied during JPEG
 * decompression (see jpeg_decode.h), not as a separate pass.
 *
 * Returns NULL on failure, having already reported the reason.
 */
RpicamIn *rpicam_in_open(uint32_t width, uint32_t height,
                         uint32_t framerate_hint, uint32_t downscale);

/*
 * Reads and decodes the next complete JPEG into frame->raw_planes.
 *
 * Mirrors v4l2_in_capture()'s error contract: an isolated corrupt frame is
 * retried internally rather than surfaced, and false is returned only for
 * what a retry cannot fix -- the child process dying, the pipe closing, or
 * the stream's geometry disagreeing with what open() validated.
 */
bool rpicam_in_capture(RpicamIn *in, VideoFrame *frame);

/*
 * Stops the child process and releases the pipe. Safe on NULL.
 */
void rpicam_in_close(RpicamIn *in);

/*
 * Whether a usable `rpicam-vid` and an actual camera are present -- used
 * by the "auto" capture-source path to tell a Pi with a CSI camera from
 * one without. Runs `rpicam-vid --list-cameras` and reports whether it
 * found at least one. Never fatal; a false answer just means this backend
 * isn't the right one.
 */
bool rpicam_in_available(void);
