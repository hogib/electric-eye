#include "h264_rtp.h"
#include <string.h>

enum {
  nal_idr = 5,
  nal_sps = 7,
  nal_pps = 8,
  nal_aud = 9,
  nal_filler = 12,
  nal_last_single = 23, // 24..31 are RTP's own packet types, 0 is unspecified
  nal_fu_a = 28,
};

void h264rtp_init(H264Rtp *p, uint32_t ssrc, uint16_t first_seq,
                  size_t max_packet, H264RtpEmit emit, void *ctx) {
  memset(p, 0, sizeof *p);
  if (max_packet == 0)
    max_packet = h264rtp_default_packet;
  if (max_packet < h264rtp_min_packet)
    max_packet = h264rtp_min_packet;
  if (max_packet > h264rtp_max_packet)
    max_packet = h264rtp_max_packet;
  p->emit = emit;
  p->ctx = ctx;
  p->ssrc = ssrc;
  p->seq = first_seq;
  p->max_packet = max_packet;
}

uint32_t h264rtp_timestamp(uint64_t ns) {
  // 90000 / 1e9 = 9 / 100000. The product overflows after 65 years of ns.
  return (uint32_t)(ns * 9u / 100000u);
}

// Sends p->pkt, whose payload (`payload` bytes after the header) is in
// place already.
static void emit(H264Rtp *p, size_t payload, uint32_t ts, bool marker) {
  uint8_t *h = p->pkt;
  h[0] = 0x80; // version 2; no padding, extension or CSRCs
  h[1] = (uint8_t)(h264rtp_payload_type | (marker ? 0x80 : 0));
  h[2] = (uint8_t)(p->seq >> 8);
  h[3] = (uint8_t)p->seq;
  h[4] = (uint8_t)(ts >> 24);
  h[5] = (uint8_t)(ts >> 16);
  h[6] = (uint8_t)(ts >> 8);
  h[7] = (uint8_t)ts;
  h[8] = (uint8_t)(p->ssrc >> 24);
  h[9] = (uint8_t)(p->ssrc >> 16);
  h[10] = (uint8_t)(p->ssrc >> 8);
  h[11] = (uint8_t)p->ssrc;
  p->seq++;
  p->packets++;
  p->bytes += h264rtp_header + payload;
  p->emit(p->ctx, p->pkt, h264rtp_header + payload);
}

// One NAL unit (header byte first, no start code): alone in a packet if
// it fits, as FU-A fragments if not. `marker`: it ends the access unit.
static void send_nal(H264Rtp *p, const uint8_t *nal, size_t len, uint32_t ts,
                     bool marker) {
  size_t room = p->max_packet - h264rtp_header;
  uint8_t *out = p->pkt + h264rtp_header;
  if (len <= room) {
    memcpy(out, nal, len);
    emit(p, len, ts, marker);
    return;
  }
  // FU-A: the NAL header is not sent as such. Its F and NRI bits go into
  // each fragment's FU indicator and its type into the FU header, whose
  // top bits flag the first (S) and last (E) fragment. A NAL that needs
  // fragmenting always makes two or more, so S and E are never both set
  // (RFC 6184 5.8 forbids it).
  uint8_t indicator = (uint8_t)((nal[0] & 0xe0) | nal_fu_a);
  uint8_t type = nal[0] & 0x1f;
  const uint8_t *data = nal + 1;
  size_t left = len - 1;
  size_t chunk = room - 2;
  bool first = true;
  while (left) {
    size_t n = left < chunk ? left : chunk;
    bool last = n == left;
    out[0] = indicator;
    out[1] = (uint8_t)(type | (first ? 0x80 : 0) | (last ? 0x40 : 0));
    memcpy(out + 2, data, n);
    emit(p, n + 2, ts, marker && last);
    data += n;
    left -= n;
    first = false;
  }
}

// The next NAL unit at or after *pos in an Annex B buffer, start code and
// padding stripped. False when there are no more.
static bool next_nal(const uint8_t *b, size_t len, size_t *pos,
                     const uint8_t **nal, size_t *nal_len) {
  size_t i = *pos;
  for (;;) {
    // The start code: 00 00 01. (The four-byte form is this one after a
    // zero, which the previous NAL's end strips.)
    for (;; i++) {
      if (i + 3 > len)
        return false;
      if (b[i] == 0 && b[i + 1] == 0 && b[i + 2] == 1)
        break;
    }
    size_t start = i + 3;
    size_t end = start;
    while (end + 3 <= len && !(b[end] == 0 && b[end + 1] == 0 && b[end + 2] == 1))
      end++;
    if (end + 3 > len)
      end = len; // no further start code: the NAL runs to the end
    i = end;
    // A NAL unit never ends in a zero byte (its last byte holds the stop
    // bit), so zeros here are padding or the next start code's first byte.
    while (end > start && b[end - 1] == 0)
      end--;
    if (end > start) {
      *nal = b + start;
      *nal_len = end - start;
      *pos = i;
      return true;
    }
  }
}

static void keep(uint8_t *dst, size_t *dst_len, const uint8_t *nal,
                 size_t len) {
  // One too big to keep must not leave an older one to be repeated.
  *dst_len = len <= h264rtp_max_param ? len : 0;
  memcpy(dst, nal, *dst_len);
}

bool h264rtp_send(H264Rtp *p, const uint8_t *au, size_t len,
                  uint32_t timestamp) {
  // Each NAL is held back until the next one is known, so the last can
  // carry the marker bit.
  const uint8_t *held = NULL;
  size_t held_len = 0;
  bool sps_sent = false, pps_sent = false, keyframe = false;

  size_t pos = 0;
  const uint8_t *nal;
  size_t nal_len;
  while (next_nal(au, len, &pos, &nal, &nal_len)) {
    unsigned type = nal[0] & 0x1f;
    if (type == nal_aud || type == nal_filler)
      continue;
    // The forbidden_zero_bit set means the NAL is damaged.
    if ((nal[0] & 0x80) || type == 0 || type > nal_last_single) {
      p->dropped_nals++;
      continue;
    }
    if (held) {
      send_nal(p, held, held_len, timestamp, false);
      held = NULL;
    }
    if (type == nal_sps) {
      keep(p->sps, &p->sps_len, nal, nal_len);
      sps_sent = true;
    } else if (type == nal_pps) {
      keep(p->pps, &p->pps_len, nal, nal_len);
      pps_sent = true;
    } else if (type == nal_idr && !keyframe) {
      keyframe = true;
      bool repeated = false;
      if (!sps_sent && p->sps_len) {
        send_nal(p, p->sps, p->sps_len, timestamp, false);
        repeated = true;
      }
      if (!pps_sent && p->pps_len) {
        send_nal(p, p->pps, p->pps_len, timestamp, false);
        repeated = true;
      }
      if (repeated)
        p->params_repeated++;
    }
    held = nal;
    held_len = nal_len;
  }
  if (!held)
    return false;
  send_nal(p, held, held_len, timestamp, true);
  p->access_units++;
  if (keyframe)
    p->keyframes++;
  return keyframe;
}
