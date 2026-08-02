#pragma once
#include "video_frame.h"
#include <stdbool.h>
#include <stdint.h>

/*
 * Captures MJPEG frames from a V4L2 camera device using MMAP streaming I/O
 * (REQBUFS/QBUF/DQBUF), replacing the producer's ffmpeg subprocess and its
 * pipe. Decodes straight from the driver's DMA buffer into a VideoFrame's
 * raw_planes via libjpeg-turbo's tjDecompressToYUVPlanes -- no intermediate
 * RGB conversion, no extra copy.
 *
 * Scope, deliberately: requires the camera's actual JPEG stream to be 4:2:2
 * subsampled (matching VideoFrame's I422 layout) and fails clearly at open
 * time if it isn't, rather than carrying a general-purpose chroma
 * resampling fallback for every possible subsampling. Also does not inject
 * a missing Huffman table (DHT) for cameras that omit one from their MJPEG
 * stream -- a real, documented quirk on some UVC webcams, left out here
 * because doing it right means embedding ~420 bytes of exact standard
 * table data. If every frame fails to decode with a libjpeg-turbo error
 * about a missing Huffman table, that's the symptom -- the fix is a
 * contained addition to v4l2_in.c, not an architecture change.
 */

typedef struct V4l2In V4l2In;

/*
 * Opens `path`, negotiates MJPEG at width x height x framerate_hint (a
 * best-effort request -- V4L2 does not guarantee a driver honors an exact
 * framerate), and sets up MMAP buffers. Also validates the camera's actual
 * JPEG subsampling by decoding one real frame's header before returning,
 * so a persistent format mismatch (e.g. a camera that's natively 4:2:0)
 * fails once, clearly, here -- rather than resurfacing as a confusing
 * per-frame decode failure once streaming is already underway.
 *
 * Returns NULL on failure, having already reported the reason.
 */
V4l2In *v4l2_in_open(const char *path, uint32_t width, uint32_t height,
                     uint32_t framerate_hint);

/*
 * Blocks until the driver has a filled buffer (bounded by an internal
 * poll timeout), decodes it into frame->raw_planes, and returns the
 * driver's buffer so streaming continues.
 *
 * An isolated corrupt/malformed JPEG (a real possibility -- occasional
 * USB transfer glitches happen on real hardware) is retried internally a
 * few times rather than surfaced as failure, so a single bad frame from
 * the camera doesn't take the whole pipeline down. Returns false only for
 * what a retry can't fix: a device-level error, or the frame's actual
 * format disagreeing with what v4l2_in_open() already validated.
 */
bool v4l2_in_capture(V4l2In *in, VideoFrame *frame);

void v4l2_in_close(V4l2In *in);
