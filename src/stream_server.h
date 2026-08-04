#pragma once
#include "video_frame.h"
#include <stdbool.h>
#include <stdint.h>

/*
 * A minimal, deliberately dumb live-preview video source: JPEG-encodes
 * frames (via libturbojpeg, already a project dependency -- see v4l2_in.c's
 * decode side) and writes each one, length-prefixed, to whichever single
 * client is currently connected over a plain TCP socket. No HTTP and no
 * framing beyond that one length header -- serving this to a browser (as
 * MJPEG or otherwise) is the other end of the wire's problem, not this
 * module's.
 *
 * This is a live-preview tap, not the source of record: the untouched
 * camera frame is already written locally via Config's record_path (see
 * consumer_loop in video_threads.c), so what goes out over this socket can
 * be lossy and throttled without losing anything that matters.
 *
 * Costs nothing when nobody is watching: stream_server_send_frame() checks
 * for a connected client before doing any JPEG work at all, so an unwatched
 * stream is just one idle listening socket, not a background encode loop.
 *
 * Wire format, repeated for each frame: a 4-byte big-endian frame length,
 * then that many bytes of one complete JPEG file (SOI...EOI).
 */

typedef struct StreamServer StreamServer;

// Opens a listening TCP socket on `port` (all interfaces). Never blocks.
// Returns NULL on failure (already logged); the caller should treat this as
// non-fatal -- the local pipeline (virtual cam, recording) doesn't depend
// on it.
StreamServer *stream_server_open(uint16_t port);

/*
 * Opportunistically accepts a pending connection (replacing any existing
 * client -- this is a single-viewer server, last connection wins), then, if
 * a client is connected, JPEG-encodes `frame` at `quality` (1-100; see
 * turbojpeg.h's tjCompress2 for what this scale means) and writes it to
 * that client.
 *
 * Returns false, without touching any client connection, if there is no
 * client connected -- so a caller can tell "no one is watching" apart from
 * "the encode/send itself failed" if it cares to. Also returns false, and
 * drops the connection, if the send fails or stalls past a bounded timeout
 * (see send_timeout_ms in stream_server.c): this is called from the same
 * thread that also feeds the local virtual camera, so a stuck or slow
 * network peer must never be able to stall that for long.
 */
bool stream_server_send_frame(StreamServer *s, const VideoFrame *frame,
                              int quality);

void stream_server_close(StreamServer *s);
