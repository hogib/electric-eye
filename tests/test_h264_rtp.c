// hellion's H.264 RTP packetizer (RFC 6184): the packets byte by byte, and
// a stream of them decoded by ffmpeg through the topside's own relay and
// decoder command line, compared with ffmpeg decoding the same H.264 from
// a file.
#define _POSIX_C_SOURCE 200809L
#include "h264_rtp.c"
#include "h264_src.c"

#include "child_proc.h"
#include "ffmpeg.h"
#include "udp_relay.h"
#include "test_harness.h"
#include <arpa/inet.h>
#include <netinet/in.h>
#include <stdlib.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

// ---- Capturing what is emitted --------------------------------------------

enum { cap_max = 512 };

typedef struct {
  size_t count;
  size_t len[cap_max];
  uint8_t pkt[cap_max][h264rtp_max_packet];
} Capture;

static void capture(void *ctx, const uint8_t *pkt, size_t len) {
  Capture *c = (Capture *)ctx;
  if (c->count < cap_max && len <= h264rtp_max_packet) {
    memcpy(c->pkt[c->count], pkt, len);
    c->len[c->count] = len;
  }
  c->count++;
}

static Capture cap; // 750 KB: not on the stack

static void start(H264Rtp *p, uint16_t seq, size_t max_packet) {
  memset(&cap, 0, sizeof cap);
  h264rtp_init(p, 0x11223344u, seq, max_packet, capture, &cap);
}

static unsigned pkt_seq(size_t i) {
  return (unsigned)(cap.pkt[i][2] << 8 | cap.pkt[i][3]);
}
static uint32_t pkt_ts(size_t i) {
  const uint8_t *h = cap.pkt[i];
  return (uint32_t)h[4] << 24 | (uint32_t)h[5] << 16 | (uint32_t)h[6] << 8 |
         h[7];
}
static bool pkt_marker(size_t i) { return (cap.pkt[i][1] & 0x80) != 0; }
static const uint8_t *payload(size_t i) { return cap.pkt[i] + 12; }
static size_t payload_len(size_t i) { return cap.len[i] - 12; }

// Appends a start code (3 or 4 bytes) and a NAL to an Annex B buffer.
static size_t put_nal(uint8_t *buf, size_t at, int start_code_len,
                      const uint8_t *nal, size_t len) {
  if (start_code_len == 4)
    buf[at++] = 0;
  buf[at++] = 0;
  buf[at++] = 0;
  buf[at++] = 1;
  memcpy(buf + at, nal, len);
  return at + len;
}

// A NAL of `len` bytes with the given header byte and a recognisable,
// zero-free body.
static void fill_nal(uint8_t *nal, size_t len, uint8_t header) {
  nal[0] = header;
  for (size_t i = 1; i < len; i++)
    nal[i] = (uint8_t)(1 + i % 251);
}

// ---- The packets ----------------------------------------------------------

static void test_single_nal(void) {
  H264Rtp p;
  start(&p, 1000, 0);
  static const uint8_t slice[] = {0x41, 0x9a, 0x24, 0x6c, 0x41};
  uint8_t au[32];
  size_t n = put_nal(au, 0, 4, slice, sizeof slice);
  CHECK(!h264rtp_send(&p, au, n, 0xdeadbeefu));

  CHECK_EQ_INT(cap.count, 1);
  CHECK_EQ_INT(cap.len[0], 12 + sizeof slice);
  const uint8_t *h = cap.pkt[0];
  CHECK_EQ_INT(h[0], 0x80);        // version 2, nothing else
  CHECK_EQ_INT(h[1], 0x80 | 96);   // marker, payload type 96
  CHECK_EQ_INT(pkt_seq(0), 1000);
  CHECK_EQ_INT(pkt_ts(0), 0xdeadbeefu);
  static const uint8_t ssrc[] = {0x11, 0x22, 0x33, 0x44};
  CHECK_MEM_EQ(h + 8, ssrc, 4);
  CHECK_MEM_EQ(payload(0), slice, sizeof slice);
  CHECK_EQ_INT(p.packets, 1);
  CHECK_EQ_INT(p.bytes, 12 + sizeof slice);
  CHECK_EQ_INT(p.access_units, 1);
  CHECK_EQ_INT(p.keyframes, 0);
}

// Several NALs in one access unit: both start code lengths, zero padding
// after a NAL, junk before the first start code. One packet each, the
// same timestamp, the marker on the last only.
static void test_access_unit(void) {
  H264Rtp p;
  start(&p, 7, 0);
  static const uint8_t sps[] = {0x67, 0x42, 0xc0, 0x1f, 0x8c};
  static const uint8_t pps[] = {0x68, 0xce, 0x3c, 0x80};
  static const uint8_t sei[] = {0x06, 0x05, 0x04, 0x80};
  static const uint8_t idr[] = {0x65, 0x88, 0x84, 0x21};
  uint8_t au[96] = {0xff, 0xfe}; // junk a camera has no business sending
  size_t n = 2;
  n = put_nal(au, n, 4, sps, sizeof sps);
  n = put_nal(au, n, 3, pps, sizeof pps);
  au[n++] = 0; // padding: trailing zero bytes belong to no NAL
  au[n++] = 0;
  n = put_nal(au, n, 4, sei, sizeof sei);
  n = put_nal(au, n, 3, idr, sizeof idr);
  au[n++] = 0;
  CHECK(h264rtp_send(&p, au, n, 90000));

  CHECK_EQ_INT(cap.count, 4);
  CHECK_EQ_INT(payload_len(0), sizeof sps);
  CHECK_MEM_EQ(payload(0), sps, sizeof sps);
  CHECK_EQ_INT(payload_len(1), sizeof pps);
  CHECK_MEM_EQ(payload(1), pps, sizeof pps);
  CHECK_EQ_INT(payload_len(2), sizeof sei);
  CHECK_MEM_EQ(payload(2), sei, sizeof sei);
  CHECK_EQ_INT(payload_len(3), sizeof idr);
  CHECK_MEM_EQ(payload(3), idr, sizeof idr);
  for (size_t i = 0; i < 4; i++) {
    CHECK_EQ_INT(pkt_seq(i), 7 + i);
    CHECK_EQ_INT(pkt_ts(i), 90000);
    CHECK_EQ_INT(pkt_marker(i), i == 3);
  }
  CHECK_EQ_INT(p.keyframes, 1);
  CHECK_EQ_INT(p.params_repeated, 0); // it brought its own
}

// Puts FU-A fragments back together; false if they are not a well-formed
// run for one NAL.
static bool reassemble(size_t first, size_t count, uint8_t *out,
                       size_t *out_len) {
  size_t n = 0;
  for (size_t i = first; i < first + count; i++) {
    const uint8_t *pl = payload(i);
    if (payload_len(i) < 3 || (pl[0] & 0x1f) != 28)
      return false;
    bool s = pl[1] & 0x80, e = pl[1] & 0x40;
    if (s != (i == first) || e != (i == first + count - 1) || (pl[1] & 0x20))
      return false;
    if (i == first)
      out[n++] = (uint8_t)((pl[0] & 0xe0) | (pl[1] & 0x1f));
    else if (pl[0] != payload(first)[0] ||
             (pl[1] & 0x1f) != (payload(first)[1] & 0x1f))
      return false;
    memcpy(out + n, pl + 2, payload_len(i) - 2);
    n += payload_len(i) - 2;
  }
  *out_len = n;
  return true;
}

static void test_fragmentation(void) {
  enum { max_packet = 200, room = max_packet - 12, nal_len = 1000 };
  H264Rtp p;
  start(&p, 0, max_packet);
  static uint8_t nal[nal_len], au[nal_len + 8], back[nal_len + 8];
  fill_nal(nal, nal_len, 0x65); // IDR, NRI 3
  size_t n = put_nal(au, 0, 4, nal, nal_len);
  CHECK(h264rtp_send(&p, au, n, 1234));

  // 999 bytes after the NAL header, 186 to a fragment: 6 fragments.
  CHECK_EQ_INT(cap.count, 6);
  for (size_t i = 0; i < cap.count && i < cap_max; i++) {
    CHECK(cap.len[i] <= max_packet);
    CHECK_EQ_INT(cap.len[i], i < 5 ? max_packet : 12 + 2 + 999 - 5 * 186);
    CHECK_EQ_INT(payload(i)[0], 0x60 | 28); // the NAL's NRI, type FU-A
    CHECK_EQ_INT(pkt_marker(i), i == 5);
    CHECK_EQ_INT(pkt_seq(i), i);
    CHECK_EQ_INT(pkt_ts(i), 1234);
  }
  size_t back_len = 0;
  CHECK(reassemble(0, 6, back, &back_len));
  CHECK_EQ_INT(back_len, nal_len);
  CHECK_MEM_EQ(back, nal, nal_len);

  // The boundary: a NAL that exactly fills a packet travels alone; one
  // byte more makes two fragments (never one with both S and E set).
  start(&p, 0, max_packet);
  fill_nal(nal, room, 0x41);
  n = put_nal(au, 0, 3, nal, room);
  h264rtp_send(&p, au, n, 0);
  CHECK_EQ_INT(cap.count, 1);
  CHECK_EQ_INT(cap.len[0], max_packet);
  CHECK_MEM_EQ(payload(0), nal, room);

  start(&p, 0, max_packet);
  fill_nal(nal, room + 1, 0x41);
  n = put_nal(au, 0, 3, nal, room + 1);
  h264rtp_send(&p, au, n, 0);
  CHECK_EQ_INT(cap.count, 2);
  CHECK_EQ_INT(payload(0)[1], 0x80 | 1); // S, type 1
  CHECK_EQ_INT(payload(1)[1], 0x40 | 1); // E
  CHECK(!pkt_marker(0));
  CHECK(pkt_marker(1));
  CHECK(reassemble(0, 2, back, &back_len));
  CHECK_EQ_INT(back_len, room + 1);
  CHECK_MEM_EQ(back, nal, room + 1);

  // A fragmented NAL that is not the access unit's last has no marker at
  // all; the marker is the access unit's, not the NAL's.
  start(&p, 0, max_packet);
  fill_nal(nal, 400, 0x41);
  n = put_nal(au, 0, 3, nal, 400);
  static const uint8_t slice2[] = {0x41, 0x11, 0x22};
  n = put_nal(au, n, 3, slice2, sizeof slice2);
  h264rtp_send(&p, au, n, 0);
  CHECK_EQ_INT(cap.count, 4);
  for (size_t i = 0; i < 4; i++)
    CHECK_EQ_INT(pkt_marker(i), i == 3);
}

static void test_packet_size_limits(void) {
  H264Rtp p;
  start(&p, 0, 0);
  CHECK_EQ_INT(p.max_packet, 1200);
  start(&p, 0, 10);
  CHECK_EQ_INT(p.max_packet, h264rtp_min_packet);
  start(&p, 0, 100000);
  CHECK_EQ_INT(p.max_packet, h264rtp_max_packet);

  // At the default no packet is over 1200 bytes, the size the tether
  // path has always used.
  start(&p, 0, 0);
  static uint8_t nal[50000], au[50008];
  fill_nal(nal, sizeof nal, 0x65);
  size_t n = put_nal(au, 0, 4, nal, sizeof nal);
  h264rtp_send(&p, au, n, 0);
  CHECK(cap.count > 40 && cap.count <= cap_max);
  for (size_t i = 0; i < cap.count && i < cap_max; i++)
    CHECK(cap.len[i] <= 1200);
}

static void test_sequence_wrap(void) {
  H264Rtp p;
  start(&p, 65534, 0);
  static const uint8_t slice[] = {0x41, 0x01};
  uint8_t au[16];
  size_t n = put_nal(au, 0, 3, slice, sizeof slice);
  for (int i = 0; i < 4; i++)
    h264rtp_send(&p, au, n, (uint32_t)i);
  CHECK_EQ_INT(cap.count, 4);
  CHECK_EQ_INT(pkt_seq(0), 65534);
  CHECK_EQ_INT(pkt_seq(1), 65535);
  CHECK_EQ_INT(pkt_seq(2), 0);
  CHECK_EQ_INT(pkt_seq(3), 1);
}

// A camera that sends SPS and PPS once: every later keyframe gets them
// from the copy, in that order, ahead of the slice.
static void test_parameter_sets_repeated(void) {
  H264Rtp p;
  start(&p, 0, 0);
  static const uint8_t sps[] = {0x67, 0x42, 0xc0, 0x1f};
  static const uint8_t pps[] = {0x68, 0xce, 0x3c};
  static const uint8_t sps2[] = {0x67, 0x64, 0x00, 0x28, 0xac};
  static const uint8_t idr[] = {0x65, 0x88};
  static const uint8_t idr_slice2[] = {0x65, 0x08};
  static const uint8_t non_idr[] = {0x41, 0x9a};
  uint8_t au[64];
  size_t n;

  // A keyframe before any parameter set was seen: sent as it is.
  n = put_nal(au, 0, 4, idr, sizeof idr);
  CHECK(h264rtp_send(&p, au, n, 0));
  CHECK_EQ_INT(cap.count, 1);
  CHECK_EQ_INT(p.params_repeated, 0);

  // The parameter sets, by themselves.
  start(&p, 0, 0);
  n = put_nal(au, 0, 4, sps, sizeof sps);
  n = put_nal(au, n, 4, pps, sizeof pps);
  CHECK(!h264rtp_send(&p, au, n, 0));
  CHECK_EQ_INT(cap.count, 2);
  CHECK(pkt_marker(1));

  // A two-slice keyframe without them: SPS, PPS, slice, slice.
  n = put_nal(au, 0, 4, idr, sizeof idr);
  n = put_nal(au, n, 4, idr_slice2, sizeof idr_slice2);
  CHECK(h264rtp_send(&p, au, n, 3000));
  CHECK_EQ_INT(cap.count, 6);
  CHECK_EQ_INT(payload_len(2), sizeof sps);
  CHECK_MEM_EQ(payload(2), sps, sizeof sps);
  CHECK_EQ_INT(payload_len(3), sizeof pps);
  CHECK_MEM_EQ(payload(3), pps, sizeof pps);
  CHECK_MEM_EQ(payload(4), idr, sizeof idr);
  CHECK_MEM_EQ(payload(5), idr_slice2, sizeof idr_slice2);
  for (size_t i = 2; i < 6; i++) {
    CHECK_EQ_INT(pkt_ts(i), 3000);
    CHECK_EQ_INT(pkt_marker(i), i == 5);
  }
  CHECK_EQ_INT(p.params_repeated, 1);

  // Other pictures get nothing added.
  n = put_nal(au, 0, 4, non_idr, sizeof non_idr);
  CHECK(!h264rtp_send(&p, au, n, 6000));
  CHECK_EQ_INT(cap.count, 7);

  // A keyframe that brings a new SPS and no PPS: its own SPS, once, and
  // the kept PPS before the slice.
  n = put_nal(au, 0, 4, sps2, sizeof sps2);
  n = put_nal(au, n, 4, idr, sizeof idr);
  CHECK(h264rtp_send(&p, au, n, 9000));
  CHECK_EQ_INT(cap.count, 10);
  CHECK_MEM_EQ(payload(7), sps2, sizeof sps2);
  CHECK_EQ_INT(payload_len(8), sizeof pps);
  CHECK_MEM_EQ(payload(8), pps, sizeof pps);
  CHECK_MEM_EQ(payload(9), idr, sizeof idr);

  // And the new SPS is the one repeated from then on.
  n = put_nal(au, 0, 4, idr, sizeof idr);
  h264rtp_send(&p, au, n, 12000);
  CHECK_EQ_INT(cap.count, 13);
  CHECK_EQ_INT(payload_len(10), sizeof sps2);
  CHECK_MEM_EQ(payload(10), sps2, sizeof sps2);
  CHECK_EQ_INT(p.keyframes, 3);

  // An SPS too big to keep is sent on, and the older one is not repeated
  // in its place afterwards.
  static uint8_t big[h264rtp_max_param + 1 + 8], big_au[sizeof big + 8];
  fill_nal(big, h264rtp_max_param + 1, 0x67);
  n = put_nal(big_au, 0, 4, big, h264rtp_max_param + 1);
  h264rtp_send(&p, big_au, n, 15000);
  CHECK_EQ_INT(cap.count, 14);
  CHECK_EQ_INT(payload_len(13), h264rtp_max_param + 1);
  n = put_nal(au, 0, 4, idr, sizeof idr);
  h264rtp_send(&p, au, n, 18000);
  CHECK_EQ_INT(cap.count, 16); // the PPS and the slice
  CHECK_MEM_EQ(payload(14), pps, sizeof pps);
}

// Delimiters and filler are not sent, and neither is anything that is not
// a NAL unit RTP can carry.
static void test_dropped_nals(void) {
  H264Rtp p;
  start(&p, 0, 0);
  static const uint8_t aud[] = {0x09, 0xf0};
  static const uint8_t filler[] = {0x0c, 0xff, 0xff, 0x80};
  static const uint8_t slice[] = {0x41, 0x9a};
  static const uint8_t forbidden[] = {0xc1, 0x9a};   // forbidden_zero_bit
  static const uint8_t stap[] = {0x18, 0x00, 0x01};  // type 24: RTP's own
  static const uint8_t unspecified[] = {0x60, 0x01}; // type 0
  uint8_t au[96];
  size_t n = put_nal(au, 0, 4, aud, sizeof aud);
  n = put_nal(au, n, 3, slice, sizeof slice);
  n = put_nal(au, n, 3, filler, sizeof filler);
  n = put_nal(au, n, 3, forbidden, sizeof forbidden);
  n = put_nal(au, n, 3, stap, sizeof stap);
  n = put_nal(au, n, 3, unspecified, sizeof unspecified);
  h264rtp_send(&p, au, n, 0);
  // The slice is the last NAL sent, so it has the marker, although three
  // dropped ones follow it.
  CHECK_EQ_INT(cap.count, 1);
  CHECK_MEM_EQ(payload(0), slice, sizeof slice);
  CHECK(pkt_marker(0));
  CHECK_EQ_INT(p.dropped_nals, 3);

  // Only a delimiter: nothing to send, and not an access unit.
  start(&p, 0, 0);
  n = put_nal(au, 0, 4, aud, sizeof aud);
  CHECK(!h264rtp_send(&p, au, n, 0));
  CHECK_EQ_INT(cap.count, 0);
  CHECK_EQ_INT(p.access_units, 0);
}

static void test_not_annex_b(void) {
  H264Rtp p;
  start(&p, 0, 0);
  static const uint8_t cases[][8] = {
      {0},                               // (used with length 0)
      {0x00},
      {0x00, 0x00},
      {0x00, 0x00, 0x01},                // a start code and nothing after
      {0x00, 0x00, 0x00, 0x01},
      {0x00, 0x00, 0x01, 0x00, 0x00},    // ... and only padding after
      {0x41, 0x9a, 0x24, 0x6c},          // no start code at all
      {0x00, 0x00, 0x01, 0x00, 0x00, 0x01, 0x00}, // empty NALs
  };
  static const size_t lens[] = {0, 1, 2, 3, 4, 5, 4, 7};
  for (size_t i = 0; i < sizeof lens / sizeof lens[0]; i++)
    CHECK(!h264rtp_send(&p, cases[i], lens[i], 0));
  CHECK(!h264rtp_send(&p, NULL, 0, 0));
  CHECK_EQ_INT(cap.count, 0);
  CHECK_EQ_INT(p.seq, 0);
}

static void test_timestamp(void) {
  CHECK_EQ_INT(h264rtp_timestamp(0), 0);
  CHECK_EQ_INT(h264rtp_timestamp(1000000000ull), 90000);
  CHECK_EQ_INT(h264rtp_timestamp(33333333ull), 2999);
  // 30 fps: 3000 ticks a frame, give or take the rounding.
  uint32_t a = h264rtp_timestamp(5 * 33333333ull);
  uint32_t b = h264rtp_timestamp(6 * 33333333ull);
  CHECK(b - a == 3000 || b - a == 2999);
  // It wraps (every 13 hours) rather than saturating.
  uint64_t wrap_ns = (uint64_t)1 << 32; // ticks
  wrap_ns = wrap_ns * 100000 / 9 + 100000;
  CHECK(h264rtp_timestamp(wrap_ns) < 90000);
}

// ---- Access units ---------------------------------------------------------

// h264_src.c's splitter, which an h264file: source walks its file with.
static void test_access_unit_split(void) {
  static const uint8_t aud[] = {0x09, 0xf0};
  static const uint8_t sps[] = {0x67, 0x42, 0xc0, 0x1f};
  static const uint8_t pps[] = {0x68, 0xce, 0x3c};
  static const uint8_t sei[] = {0x06, 0x05, 0x01, 0x80};
  static const uint8_t idr_a[] = {0x65, 0x88, 0x84}; // first_mb 0
  static const uint8_t idr_b[] = {0x65, 0x08, 0x84}; // a later slice
  static const uint8_t p_a[] = {0x41, 0x9a, 0x24};
  static const uint8_t p_b[] = {0x41, 0x1a, 0x24};
  uint8_t b[160];
  size_t n = 0, start[6];
  // 0: AUD SPS PPS SEI IDR IDR'   (a keyframe in two slices)
  start[0] = n;
  n = put_nal(b, n, 4, aud, sizeof aud);
  n = put_nal(b, n, 4, sps, sizeof sps);
  n = put_nal(b, n, 3, pps, sizeof pps);
  n = put_nal(b, n, 3, sei, sizeof sei);
  n = put_nal(b, n, 3, idr_a, sizeof idr_a);
  n = put_nal(b, n, 3, idr_b, sizeof idr_b);
  // 1: P P'   (no delimiter: the slice's first_mb_in_slice starts it)
  start[1] = n;
  n = put_nal(b, n, 4, p_a, sizeof p_a);
  n = put_nal(b, n, 3, p_b, sizeof p_b);
  // 2: P      (a three-byte start code at the boundary)
  start[2] = n;
  n = put_nal(b, n, 3, p_a, sizeof p_a);
  // 3: SEI P  (the SEI belongs to the picture after it)
  start[3] = n;
  n = put_nal(b, n, 4, sei, sizeof sei);
  n = put_nal(b, n, 3, p_a, sizeof p_a);
  // 4: AUD P
  start[4] = n;
  n = put_nal(b, n, 4, aud, sizeof aud);
  n = put_nal(b, n, 4, p_a, sizeof p_a);
  start[5] = n;

  size_t pos = 0;
  for (int i = 0; i < 5; i++) {
    CHECK_EQ_INT(pos, start[i]);
    size_t end = h264_next_access_unit(b, n, pos);
    CHECK_EQ_INT(end, start[i + 1]);
    CHECK_EQ_INT(h264_is_keyframe(b + pos, end - pos), i == 0);
    pos = end;
  }
  CHECK_EQ_INT(h264_next_access_unit(b, n, n), n);
  CHECK_EQ_INT(h264_next_access_unit(b, 0, 0), 0);
  // No start code at all: one "access unit" of everything, not a keyframe.
  static const uint8_t junk[] = {1, 2, 3, 4, 5, 6};
  CHECK_EQ_INT(h264_next_access_unit(junk, sizeof junk, 0), sizeof junk);
  CHECK(!h264_is_keyframe(junk, sizeof junk));
  // A start code as the last bytes: nothing is read past the end.
  static const uint8_t cut[] = {0x00, 0x00, 0x01, 0x41, 0x9a, 0x00, 0x00, 0x01};
  CHECK_EQ_INT(h264_next_access_unit(cut, sizeof cut, 0), sizeof cut);
  CHECK(!h264_is_keyframe(cut, sizeof cut));
}

// ---- ffmpeg decodes it ----------------------------------------------------

static void sleep_ms(int ms) {
  struct timespec ts = {ms / 1000, (long)(ms % 1000) * 1000000L};
  nanosleep(&ts, NULL);
}

static uint8_t *read_file(const char *path, size_t *len) {
  FILE *f = fopen(path, "rb");
  if (!f)
    return NULL;
  fseek(f, 0, SEEK_END);
  long n = ftell(f);
  fseek(f, 0, SEEK_SET);
  uint8_t *buf = n > 0 ? (uint8_t *)malloc((size_t)n) : NULL;
  if (buf && fread(buf, 1, (size_t)n, f) != (size_t)n) {
    free(buf);
    buf = NULL;
  }
  fclose(f);
  *len = buf ? (size_t)n : 0;
  return buf;
}

static bool run(char *const argv[]) {
  ChildProc c;
  int status = -1;
  if (!child_spawn(&c, "test", argv, CHILD_NULL_STDIN))
    return false;
  if (!child_wait(&c, 30000, &status)) {
    printf("    %s did not finish\n", argv[0]);
    child_stop(&c, 500);
    return false;
  }
  return WIFEXITED(status) && WEXITSTATUS(status) == 0;
}

// Where each access unit of an Annex B stream starts: at the first NAL
// after a slice that is either not a slice (SEI, SPS, PPS, delimiter) or a
// slice starting a new picture (first_mb_in_slice == 0, whose
// Exp-Golomb code is a single 1 bit).
static size_t split_access_units(const uint8_t *b, size_t len, size_t *starts,
                                 size_t max) {
  size_t count = 0;
  bool have_slice = false, open = false;
  for (size_t i = 0; i + 4 < len; i++) {
    if (!(b[i] == 0 && b[i + 1] == 0 && b[i + 2] == 1))
      continue;
    unsigned type = b[i + 3] & 0x1f;
    bool slice = type == 1 || type == 5;
    bool starts_picture = slice && (b[i + 4] & 0x80);
    if (!open || (have_slice && (!slice || starts_picture))) {
      size_t at = i > 0 && b[i - 1] == 0 ? i - 1 : i;
      if (count < max)
        starts[count] = at;
      count++;
      open = true;
      have_slice = false;
    }
    if (slice)
      have_slice = true;
    i += 3;
  }
  return count;
}

// Copies an access unit without its SPS and PPS; returns the new length.
static size_t without_parameter_sets(const uint8_t *au, size_t len,
                                     uint8_t *out) {
  size_t pos = 0, n = 0, nal_len;
  const uint8_t *nal;
  while (next_nal(au, len, &pos, &nal, &nal_len)) {
    unsigned type = nal[0] & 0x1f;
    if (type != nal_sps && type != nal_pps)
      n = put_nal(out, n, 4, nal, nal_len);
  }
  return n;
}

typedef struct {
  int fd;
  struct sockaddr_in to;
  uint64_t sent;
} Sender;

static void send_packet(void *ctx, const uint8_t *pkt, size_t len) {
  Sender *s = (Sender *)ctx;
  if (sendto(s->fd, pkt, len, 0, (struct sockaddr *)&s->to, sizeof s->to) ==
      (ssize_t)len)
    s->sent++;
}

// Whether something has bound UDP `port` on loopback (ffmpeg, once its
// RTP input is open): binding it ourselves then fails.
static bool port_taken(int port) {
  int fd = socket(AF_INET, SOCK_DGRAM, 0);
  struct sockaddr_in a = {.sin_family = AF_INET,
                          .sin_port = htons((uint16_t)port),
                          .sin_addr.s_addr = htonl(INADDR_LOOPBACK)};
  bool taken = bind(fd, (struct sockaddr *)&a, sizeof a) != 0;
  close(fd);
  return taken;
}

// An H.264 stream as a camera would hand it over, one access unit at a
// time, through the packetizer, a receiving relay set up as the topside's
// (it adds the frame-end delimiters) and the topside's decoder command
// line. The pictures that come out must be the ones ffmpeg decodes from
// the file itself, up to the last. Not from the first: with no SDP
// parameter sets ffmpeg has to see a keyframe before it knows what the
// stream is, and (told not to buffer) it does not keep what it read until
// then. That is so with any sender; what matters is that it then decodes
// every picture.
//
// The stream has what the packetizer has to get right: keyframes far
// over one packet (FU-A), two slices a picture (the marker belongs to the
// second), and SPS/PPS at the start only, so that every later keyframe
// gets them from the packetizer's copy. (x264 writes them at every
// keyframe; they are taken out of all but the first here.)
static void test_ffmpeg_decodes_it(void) {
  if (!ff_available()) {
    printf("    (skipped: ffmpeg is not installed)\n");
    return;
  }
  enum { w = 320, h = 240, frames = 60, gop = 15, frame_bytes = w * h * 2 };
  char dir[] = "/tmp/eeye-test-h264rtp-XXXXXX";
  if (!mkdtemp(dir)) {
    CHECK(!"mkdtemp");
    return;
  }
  char h264[96], ref_path[96], sdp[96], vf[128], nframes[16];
  snprintf(h264, sizeof h264, "%s/in.h264", dir);
  snprintf(ref_path, sizeof ref_path, "%s/ref.yuv", dir);
  snprintf(sdp, sizeof sdp, "%s/in.sdp", dir);
  snprintf(nframes, sizeof nframes, "%d", frames);
  snprintf(vf, sizeof vf,
           "scale=w=%d:h=%d:in_range=tv:out_range=pc,format=yuv422p", w, h);

  char *const encode[] = {
      "ffmpeg", "-hide_banner", "-nostdin", "-loglevel", "error", "-f",
      "lavfi", "-i", "testsrc2=s=320x240:r=30", "-frames:v", nframes,
      "-c:v", "libx264", "-preset", "ultrafast", "-tune", "zerolatency",
      "-pix_fmt", "yuv420p", "-g", "15", "-keyint_min", "15", "-sc_threshold",
      "0", "-x264-params",
      "slices=2:aud=1", "-bsf:v", "h264_mp4toannexb", "-f",
      "h264", "-y", h264, NULL};
  // The same conversion as the decoder command line's (ff_decoder_args),
  // so the two outputs can be compared byte for byte.
  char *const decode[] = {"ffmpeg", "-hide_banner", "-nostdin", "-loglevel",
                          "error", "-i", h264, "-vf", vf, "-f", "rawvideo",
                          "-y", ref_path, NULL};
  if (!run(encode) || !run(decode)) {
    printf("    (skipped: this ffmpeg cannot make the test stream; no "
           "libx264?)\n");
    goto cleanup;
  }

  size_t es_len = 0, ref_len = 0;
  uint8_t *es = read_file(h264, &es_len);
  uint8_t *ref = read_file(ref_path, &ref_len);
  uint8_t *got = (uint8_t *)malloc((size_t)frames * frame_bytes);
  // An access unit less its parameter sets; every NAL may grow by one
  // byte (a three-byte start code rewritten as four).
  uint8_t *au_buf = (uint8_t *)malloc(es_len * 2 + 16);
  static size_t starts[frames + 8];
  size_t aus = es ? split_access_units(es, es_len, starts, frames + 8) : 0;
  CHECK(es && ref && got && au_buf);
  CHECK_EQ_INT(ref_len, (size_t)frames * frame_bytes);
  CHECK_EQ_INT(aus, frames);
  // h264_src.c's splitter finds the same boundaries as the one above.
  for (size_t i = 0, pos = 0; es && i < aus && i < frames; i++) {
    CHECK_EQ_INT(pos, starts[i]);
    pos = h264_next_access_unit(es, es_len, pos);
    if (i + 1 == aus)
      CHECK_EQ_INT(pos, es_len);
  }
  if (!es || !ref || !got || !au_buf || aus != frames ||
      ref_len != (size_t)frames * frame_bytes)
    goto free_bufs;

  // Topside: relay -> decoder, as pipeline.c sets them up.
  int dec_port = udp_free_port_pair();
  char err[160], dest[32];
  snprintf(dest, sizeof dest, "127.0.0.1:%d", dec_port);
  UdpRelay *relay = udp_relay_open("127.0.0.1:0", err, sizeof err);
  CHECK(relay && udp_relay_add_decoder(relay, dest, err, sizeof err) &&
        udp_relay_start(relay));
  CHECK(ff_write_sdp(sdp, dec_port));
  FfDecoder d = {.sdp_path = sdp, .width = w, .height = h};
  FfArgs args;
  ff_decoder_args(&d, &args);
  ChildProc dec;
  CHECK(child_spawn(&dec, "test", args.argv,
                    CHILD_PIPE_STDOUT | CHILD_NULL_STDIN | CHILD_QUIET_STDERR));
  for (int i = 0; i < 500 && !port_taken(dec_port); i++)
    sleep_ms(10);
  CHECK(port_taken(dec_port));

  // "ROV": the packetizer, sending to the relay.
  Sender s = {.fd = socket(AF_INET, SOCK_DGRAM, 0),
              .to = {.sin_family = AF_INET,
                     .sin_port = htons((uint16_t)udp_relay_port(relay)),
                     .sin_addr.s_addr = htonl(INADDR_LOOPBACK)}};
  H264Rtp p;
  h264rtp_init(&p, 0x5eed1234u, 65500, 0, send_packet, &s); // seq wraps too

  size_t got_bytes = 0, want = (size_t)frames * frame_bytes;
  for (size_t i = 0; i < aus; i++) {
    size_t end = i + 1 < aus ? starts[i + 1] : es_len;
    const uint8_t *au = es + starts[i];
    size_t au_len = end - starts[i];
    if (i > 0) {
      au_len = without_parameter_sets(au, au_len, au_buf);
      au = au_buf;
    }
    h264rtp_send(&p, au, au_len, (uint32_t)(i * 3000));
    // Pace it like a camera (faster), reading what has been decoded so
    // far so the decoder never blocks on a full pipe.
    size_t n = 0;
    while (got_bytes < want &&
           child_read_some(dec.out_fd, got + got_bytes, want - got_bytes, 10,
                           &n) == CHILD_IO_OK)
      got_bytes += n;
  }
  // The last picture is out within a moment of its last packet (the
  // relay's delimiter ends it); nothing more comes after that.
  size_t n = 0;
  while (got_bytes < want &&
         child_read_some(dec.out_fd, got + got_bytes, want - got_bytes, 1500,
                         &n) == CHILD_IO_OK)
    got_bytes += n;
  size_t decoded = got_bytes / frame_bytes;

  RelayStats rs;
  udp_relay_stats(relay, &rs);
  printf("    %zu access units (%llu keyframes, %llu given the kept "
         "SPS/PPS) in %llu packets; relay saw %llu, lost %llu; the last %zu "
         "of %d frames decoded\n",
         aus, (unsigned long long)p.keyframes,
         (unsigned long long)p.params_repeated, (unsigned long long)p.packets,
         (unsigned long long)rs.packets, (unsigned long long)rs.lost, decoded,
         frames);
  CHECK_EQ_INT(s.sent, p.packets);
  CHECK_EQ_INT(rs.packets, p.packets);
  CHECK_EQ_INT(rs.lost, 0);
  CHECK_EQ_INT(p.access_units, frames);
  CHECK_EQ_INT(p.keyframes, frames / gop);
  CHECK_EQ_INT(p.params_repeated, frames / gop - 1); // the first had its own
  CHECK(p.packets > 2 * (uint64_t)frames); // some were fragmented
  // Whole frames, at least one keyframe's worth and what follows it (so
  // at least one keyframe that had its SPS/PPS from the packetizer's
  // copy), and each one exactly the reference's.
  CHECK_EQ_INT(got_bytes % frame_bytes, 0);
  CHECK(decoded >= gop && decoded <= frames);
  for (size_t f = 0; f < decoded && decoded <= frames; f++) {
    size_t ref_f = frames - decoded + f;
    if (memcmp(got + f * frame_bytes, ref + ref_f * frame_bytes,
               frame_bytes) != 0) {
      printf("    frame %zu differs from the reference decode\n", ref_f);
      CHECK(!"decoded frame differs");
      break;
    }
  }

  child_stop(&dec, 500);
  udp_relay_close(relay);
  close(s.fd);
free_bufs:
  free(es);
  free(ref);
  free(got);
  free(au_buf);
cleanup:
  unlink(h264);
  unlink(ref_path);
  unlink(sdp);
  rmdir(dir);
}

int main(void) {
  setvbuf(stdout, NULL, _IOLBF, 0);
  RUN_TEST(test_single_nal);
  RUN_TEST(test_access_unit);
  RUN_TEST(test_fragmentation);
  RUN_TEST(test_packet_size_limits);
  RUN_TEST(test_sequence_wrap);
  RUN_TEST(test_parameter_sets_repeated);
  RUN_TEST(test_dropped_nals);
  RUN_TEST(test_not_annex_b);
  RUN_TEST(test_timestamp);
  RUN_TEST(test_access_unit_split);
  RUN_TEST(test_ffmpeg_decodes_it);
  TEST_MAIN_END();
}
