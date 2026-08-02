#pragma once
#include "video_frame.h"
#include <stdbool.h>
#include <stdint.h>

/*
 * Writes processed frames straight to a v4l2loopback device, replacing the
 * output ffmpeg subprocess and its pipe.
 *
 * Uses V4L2's write() I/O method rather than MMAP+QBUF/DQBUF streaming. For
 * an output device that is simply pushing already-formatted frames, write()
 * costs one syscall per frame and needs no buffer negotiation. (The capture
 * side is different -- there MMAP genuinely matters, because it lets the
 * driver DMA directly into memory we already own.)
 *
 * Host setup, unchanged from the ffmpeg version:
 *   sudo modprobe v4l2loopback video_nr=10 card_label="VirtualCam" \
 *        exclusive_caps=1
 */

typedef struct V4l2Out V4l2Out;

/*
 * Opens `path` and negotiates the output format.
 *
 * fourcc: a V4L2_PIX_FMT_* code. V4L2_PIX_FMT_YUV422P matches the I422 layout
 *   vf_create() builds. YUYV or YUV420 are the fallbacks if a downstream
 *   consumer refuses planar 4:2:2.
 *
 * Note that VIDIOC_S_FMT is a *negotiation*: the driver is free to hand back
 * dimensions or a frame size different from what was asked for. The returned
 * handle stores whatever the driver actually settled on, which is what
 * v4l2_out_write() will expect. Query it with v4l2_out_frame_size().
 *
 * Returns NULL on failure, having already reported the reason.
 */
V4l2Out *v4l2_out_open(const char *path, uint32_t width, uint32_t height,
                       uint32_t fourcc);

/*
 * The frame size in bytes the driver settled on during open. Compare this
 * against a frame's total plane size before the first write: a mismatch means
 * every frame written will be misaligned, which shows up as rolling or skewed
 * video rather than as an error.
 */
size_t v4l2_out_frame_size(const V4l2Out *out);

/*
 * Writes one frame. Returns false if the device went away or the frame does
 * not match the negotiated format; the caller should shut the pipeline down,
 * as consumer_loop already does on pipe failure.
 *
 * Relies on VideoFrame's planes being slices of a single allocation (see
 * vf_create), so all three go out in one write() rather than three.
 */
bool v4l2_out_write(V4l2Out *out, const VideoFrame *frame);

void v4l2_out_close(V4l2Out *out);
