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
 * Costs nothing when nobody is watching: stream_server_offer_frame()
 * checks for a connected client before copying anything, so an unwatched
 * stream is just one idle listening socket and a sleeping thread.
 *
 * Encoding and sending happen on the server's own thread, never on the
 * caller's: the caller is consumer_loop, which also feeds the local
 * virtual camera, and a viewer on a marginal tether must not be able to
 * stall that. The caller only copies the frame into a one-slot mailbox;
 * if the stream thread is still busy with an earlier frame, the newer one
 * replaces whatever is waiting -- a live preview wants the latest frame,
 * not every frame.
 *
 * Wire format, repeated for each frame: a 4-byte big-endian frame length,
 * then that many bytes of one complete JPEG file (SOI...EOI).
 */

typedef struct StreamServer StreamServer;

// Opens a listening TCP socket on `port` (all interfaces) and starts the
// stream thread. Never blocks. Returns NULL on failure (already logged);
// the caller should treat this as non-fatal -- the local pipeline (virtual
// cam, recording) doesn't depend on it.
StreamServer *stream_server_open(uint16_t port);

/*
 * Hands `frame` to the stream thread to JPEG-encode at `quality` (1-100;
 * see turbojpeg.h's tjCompress2 for what this scale means) and send to the
 * connected viewer. Copies the frame's work planes, so the caller may
 * recycle the frame as soon as this returns.
 *
 * Returns false, doing nothing, if no viewer is connected. Never blocks on
 * the network: a viewer that cannot take a whole frame within
 * send_timeout_ms (see stream_server.c) is dropped by the stream thread.
 */
bool stream_server_offer_frame(StreamServer *s, const VideoFrame *frame,
                               int quality);

// Stops the stream thread and closes every socket.
void stream_server_close(StreamServer *s);
