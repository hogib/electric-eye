#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/*
 * The ffmpeg command lines hellion runs, and what it needs to know about
 * the ffmpeg installed here.
 *
 * ffmpeg does the H.264 encoding and decoding and the RTP framing; hellion
 * feeds it raw frames and moves its packets (udp_relay.c). Keeping the
 * codec in a child process keeps hellion free of libav's ABI and lets the
 * encoder be swapped per machine (h264_v4l2m2m on a Pi 4) without a
 * rebuild.
 *
 * Colour range: the frame bus is full range (what eeye's effects assume);
 * H.264 for QGC and every player is limited range. The encoder converts
 * full -> limited and the decoder limited -> full, explicitly, so neither
 * end guesses and the picture is neither washed out nor crushed.
 */

typedef struct {
  const char *encoder; // "libx264" (default), "h264_v4l2m2m", ...
  uint32_t width, height;
  uint32_t bitrate_kbps;
  uint32_t gop;         // frames between keyframes; also how soon a
                        // receiver that joins late gets a picture
  uint32_t framerate;   // nominal; frames are stamped at this rate
  int rtp_port;         // sends RTP to 127.0.0.1:rtp_port
} FfEncoder;

typedef struct {
  const char *sdp_path; // describes the RTP stream (ff_write_sdp)
  uint32_t width, height;
} FfDecoder;

// Command-line storage: argv points into `store`.
typedef struct {
  char *argv[64];
  int argc;
  char store[2048];
  size_t used;
} FfArgs;

// Whether ffmpeg is installed, and which of the renamed "pass timestamps
// through" options it takes (-fps_mode passthrough from 5.1, -vsync 0
// before; ffmpeg 9 has only the former). Probed once, then cached.
bool ff_available(void);

void ff_encoder_args(const FfEncoder *e, FfArgs *a);
void ff_decoder_args(const FfDecoder *d, FfArgs *a);

// Writes an SDP describing H.264 over RTP (payload type 96, which is what
// ffmpeg sends and QGC expects) arriving at 127.0.0.1:port.
bool ff_write_sdp(const char *path, int port);

// argv as one line, for logging.
void ff_describe(const FfArgs *a, char *out, size_t cap);
