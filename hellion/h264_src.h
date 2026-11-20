#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/*
 * A source of H.264 that is already encoded, one access unit (picture) at
 * a time, as Annex B. Nothing here decodes: what comes out goes to the
 * RTP packetizer (h264_rtp.h) as it is.
 *
 * - A camera that encodes H.264 itself (UVC, V4L2_PIX_FMT_H264; on many
 *   it is a second /dev/video node beside the MJPEG one). Each V4L2 buffer
 *   is one access unit. Unlike v4l2_in.c's MJPEG capture, which keeps only
 *   the newest buffer, every buffer is taken in order: a picture that is
 *   skipped breaks the ones predicted from it until the next keyframe.
 * - An Annex B file (.h264), played in a loop at a given rate: for tests
 *   and benches with no such camera.
 */

typedef struct H264Src H264Src;

// Opens the camera at `device` for H.264 at width x height (the driver may
// give another size; see h264_src_geometry) and about `framerate`. NULL
// with the reason printed.
H264Src *h264_src_open_camera(const char *device, uint32_t width,
                              uint32_t height, uint32_t framerate);

// Opens an Annex B file, which must start with a keyframe (and its SPS and
// PPS, to be of any use). NULL with the reason printed.
H264Src *h264_src_open_file(const char *path, uint32_t framerate);

// The size the camera was set to. 0 x 0 for a file (not parsed).
void h264_src_geometry(const H264Src *s, uint32_t *width, uint32_t *height);

// The camera's descriptor, for camera controls; -1 for a file.
int h264_src_fd(const H264Src *s);

typedef enum {
  H264_SRC_AU,   // *au is one access unit
  H264_SRC_IDLE, // nothing within the timeout
  H264_SRC_LOST, // the camera is gone or has stopped; close and reopen
} H264SrcResult;

/*
 * The next access unit, waiting up to timeout_ms. On H264_SRC_AU, *au
 * points into the source's own buffer (for a camera, the driver's) and
 * stays valid until h264_src_done(), which must be called before the next
 * read. *capture_ns is CLOCK_MONOTONIC: the driver's stamp when it gives
 * one on that clock, else the time of the read.
 */
H264SrcResult h264_src_read(H264Src *s, const uint8_t **au, size_t *len,
                            uint64_t *capture_ns, int timeout_ms);
void h264_src_done(H264Src *s);

/*
 * Asks for a keyframe soon, for a receiver that has just joined. A camera
 * is asked with V4L2's force-key-frame control; one without it has its
 * stream stopped and started again, which makes a camera begin afresh
 * with SPS, PPS and a keyframe at the cost of a short gap. A file starts
 * over. Not while an access unit is held (between read and done). False
 * if neither worked; the camera's own next keyframe will have to do.
 */
bool h264_src_keyframe(H264Src *s);

void h264_src_close(H264Src *s);

/*
 * Where the access unit after the one starting at `pos` begins in an
 * Annex B buffer, or `len` if that one is the last. A new access unit
 * begins at the first NAL after a slice that is not itself a slice of the
 * same picture: a delimiter, SPS, PPS or SEI, or a slice with
 * first_mb_in_slice 0.
 */
size_t h264_next_access_unit(const uint8_t *buf, size_t len, size_t pos);

// Whether an access unit holds a keyframe (an IDR slice).
bool h264_is_keyframe(const uint8_t *au, size_t len);
