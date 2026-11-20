#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/*
 * H.264 into RTP packets (RFC 6184), for a stream that is already encoded:
 * a camera's own H.264. With this the ROV needs no ffmpeg and no encode.
 *
 * In: one access unit (one picture) at a time, as Annex B: NAL units
 * separated by 00 00 01 start codes, which is how a UVC camera hands each
 * V4L2 buffer over. Out: the packets, through a callback, in order.
 *
 * What goes on the wire is what hellion's ffmpeg encoder sent, so the
 * receiving side (ffmpeg.c's SDP, the decoder, the recorder, QGC) needs
 * no change:
 * - packetization-mode 1, non-interleaved: a NAL unit that fits travels
 *   alone (single NAL unit packet); a bigger one is split into FU-A
 *   fragments. No aggregation packets (STAP-A): every receiver must take
 *   them, but they save 40 bytes a second and ffmpeg never sent any;
 * - payload type 96, a 90 kHz clock, the marker bit on the last packet of
 *   each access unit;
 * - SPS and PPS before every keyframe. There is no SDP exchange to carry
 *   them out of band, and a receiver that joins late (QGC opened
 *   mid-dive, a recording started) cannot decode without them. A camera
 *   that sends them only once has them repeated from a copy kept here.
 *
 * Access unit delimiters and filler data are not sent: they carry no
 * picture, and the receiving relay adds the delimiter its decoder wants
 * (udp_relay_add_decoder).
 *
 * No allocation, no I/O, no threads: the caller owns the socket and the
 * clock. Not safe to call from two threads at once.
 */

enum {
  h264rtp_header = 12,
  // The whole packet, RTP header included. 1200 fits a 1500-byte MTU with
  // room for tunnel or VPN overhead; it is what the ffmpeg path used.
  h264rtp_default_packet = 1200,
  h264rtp_min_packet = 64,
  h264rtp_max_packet = 1472, // 1500 - IPv4 - UDP
  h264rtp_payload_type = 96,
  // SPS and PPS are a few dozen bytes; one that does not fit is sent on
  // but not kept.
  h264rtp_max_param = 256,
};

// One finished packet. `pkt` is only valid during the call.
typedef void (*H264RtpEmit)(void *ctx, const uint8_t *pkt, size_t len);

typedef struct {
  H264RtpEmit emit;
  void *ctx;
  uint32_t ssrc;
  uint16_t seq; // of the next packet
  size_t max_packet;

  // The newest SPS and PPS seen, to repeat before a keyframe that comes
  // without them.
  uint8_t sps[h264rtp_max_param], pps[h264rtp_max_param];
  size_t sps_len, pps_len;

  uint64_t packets;
  uint64_t bytes; // of packets, RTP headers included
  uint64_t access_units;
  uint64_t keyframes;
  uint64_t params_repeated; // keyframes that needed the kept SPS/PPS
  uint64_t dropped_nals;    // malformed, or a type that cannot be sent

  uint8_t pkt[h264rtp_max_packet];
} H264Rtp;

// `max_packet` is clamped to h264rtp_min_packet..h264rtp_max_packet; 0
// means the default. `ssrc` and `first_seq` should be random per stream
// (RFC 3550): a receiver tells a restarted sender by the SSRC changing.
void h264rtp_init(H264Rtp *p, uint32_t ssrc, uint16_t first_seq,
                  size_t max_packet, H264RtpEmit emit, void *ctx);

/*
 * Packetizes one access unit. Every packet of it carries `timestamp`, and
 * the last one the marker bit. Bytes before the first start code and
 * zero padding are ignored; an access unit with nothing to send emits
 * nothing. Returns whether it held a keyframe (an IDR slice).
 */
bool h264rtp_send(H264Rtp *p, const uint8_t *au, size_t len,
                  uint32_t timestamp);

// A time in nanoseconds on RTP's 90 kHz video clock.
uint32_t h264rtp_timestamp(uint64_t ns);
