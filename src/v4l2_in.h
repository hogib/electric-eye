#pragma once
#include "video_frame.h"
#include <stdbool.h>
#include <stdint.h>

/*
 * Captures frames from a V4L2 camera device using MMAP streaming I/O
 * (REQBUFS/QBUF/DQBUF), replacing the producer's ffmpeg subprocess and its
 * pipe. Prefers MJPEG (decoded straight from the driver's DMA buffer into
 * a VideoFrame's raw_planes via libjpeg-turbo's tjDecompressToYUVPlanes --
 * no intermediate RGB conversion, no extra copy), falling back to packed
 * YUYV -- a plain deinterleave into the same raw_planes, no decode at all
 * -- if MJPEG isn't offered at exactly the requested resolution. Whichever
 * format actually got negotiated is fixed for the lifetime of the V4l2In
 * handle; v4l2_in_capture() dispatches to the matching path internally.
 *
 * Scope, deliberately: requires the camera's actual JPEG stream to be 4:2:2
 * subsampled (matching VideoFrame's I422 layout) and fails clearly at open
 * time if it isn't, rather than carrying a general-purpose chroma
 * resampling fallback for every possible subsampling.
 *
 * MJPEG frames missing a DHT (Huffman table) marker -- a real, documented
 * quirk on some UVC webcams that rely on the decoder already knowing the
 * standard JPEG tables -- are handled transparently: each frame is scanned
 * for an existing DHT before decode, and the standard tables are spliced
 * in only when one is actually missing (see jpeg_has_dht()/ensure_dht() in
 * v4l2_in.c), so a camera that already emits its own DHT pays only the
 * cost of that scan.
 */

typedef struct V4l2In V4l2In;

/*
 * Opens `path`, negotiates a capture format (MJPEG preferred, YUYV as
 * fallback -- see above) at width x height x framerate_hint (a
 * best-effort request -- V4L2 does not guarantee a driver honors an exact
 * framerate), and sets up MMAP buffers. Also validates one real frame
 * (MJPEG: subsampling, by decoding a header; YUYV: that a full frame's
 * worth of bytes actually arrived) before returning, so a persistent
 * format problem (e.g. a camera that's natively 4:2:0) fails once,
 * clearly, here -- rather than resurfacing as a confusing per-frame
 * decode failure once streaming is already underway.
 *
 * `downscale` (1, 2, 4, or 8 -- validated by config.c) is applied on the
 * way out of the decode, not as a separate pass afterward: the frames
 * handed to v4l2_in_read_frame() are width/downscale x height/downscale,
 * and nothing downstream ever sees a full-resolution frame. The MJPEG
 * path gets this nearly free by asking libjpeg-turbo to scale during
 * decompression (fewer IDCT coefficients, so decode itself gets cheaper);
 * the YUYV path box-averages each NxN block while deinterleaving.
 *
 * Returns NULL on failure, having already reported the reason.
 */
V4l2In *v4l2_in_open(const char *path, uint32_t width, uint32_t height,
                     uint32_t framerate_hint, uint32_t downscale);

/*
 * Best-effort "what can this camera actually do?" pass, run once at
 * startup before anything is sized from the answer. Asks the driver
 * (VIDIOC_ENUM_FRAMESIZES, over both MJPEG and YUYV) what it offers and
 * rewrites the width and height it is given to the closest usable match
 * for what was requested, so a camera that simply doesn't do the
 * configured resolution runs at a sensible nearby one instead of failing
 * to open forever.
 *
 * Ranking is aspect ratio first, pixel count second: a 4:3 substitute for
 * a 16:9 request changes the framing of every subsequent frame, which is
 * a worse surprise than the same framing at fewer pixels. An exact match
 * always wins outright and is chosen silently.
 *
 * Candidates are filtered by `downscale`'s divisibility rule (width a
 * multiple of 2*downscale, height a multiple of downscale) -- the same
 * rule config.c enforces on the requested size, applied here because the
 * negotiated size is what the box-average and I422 chroma math actually
 * run on. A camera whose every mode fails that rule is reported rather
 * than silently reverting downscale.
 *
 * This deliberately cannot fail the caller: it returns false (leaving
 * both values untouched) when the device is absent or refuses to
 * enumerate, because the camera legitimately may not be plugged in yet at
 * startup -- producer_loop's reconnect loop covers that case, and the
 * configured size stays in force. It is only ever advisory; v4l2_in_open()
 * still demands whatever size it is eventually given, exactly.
 */
bool v4l2_in_negotiate_size(const char *path, uint32_t *width,
                            uint32_t *height, uint32_t downscale);

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
