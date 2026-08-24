// find_jpeg_end() decides where one frame ends in rpicam-vid's MJPEG pipe
// stream. Getting it wrong is quietly catastrophic: a premature end
// truncates every frame, a missed end desynchronizes the stream forever.
// It is pure (buffer in, length out), so it can be tested exhaustively.
//
// Includes the TU for access to the static. The stub below stands in for
// the parts of rpicam_in.c that would otherwise pull in a JPEG decoder and
// a child process -- nothing find_jpeg_end touches.
#define _POSIX_C_SOURCE 200809L
#include "rpicam_in.c"
#include "test_harness.h"

// Builds a syntactically valid JPEG: SOI, optional segments, SOS, entropy
// data, EOI. Real enough for a marker walker, without needing an encoder.
typedef struct {
  uint8_t buf[65536];
  size_t len;
} Jpeg;

static void jpeg_start(Jpeg *j) {
  j->len = 0;
  j->buf[j->len++] = 0xFF;
  j->buf[j->len++] = 0xD8; // SOI
}

// A marker segment with `payload_len` bytes of body, filled with `fill`.
static void jpeg_segment(Jpeg *j, uint8_t marker, size_t payload_len,
                         uint8_t fill) {
  j->buf[j->len++] = 0xFF;
  j->buf[j->len++] = marker;
  size_t seg = payload_len + 2; // the length field counts itself
  j->buf[j->len++] = (uint8_t)(seg >> 8);
  j->buf[j->len++] = (uint8_t)(seg & 0xFF);
  memset(j->buf + j->len, fill, payload_len);
  j->len += payload_len;
}

static void jpeg_sos_and_data(Jpeg *j, const uint8_t *entropy, size_t n) {
  jpeg_segment(j, 0xDA, 8, 0x00); // SOS header
  memcpy(j->buf + j->len, entropy, n);
  j->len += n;
}

static void jpeg_end(Jpeg *j) {
  j->buf[j->len++] = 0xFF;
  j->buf[j->len++] = 0xD9; // EOI
}

static void simple_jpeg(Jpeg *j) {
  jpeg_start(j);
  jpeg_segment(j, 0xDB, 64, 0x10);  // DQT
  jpeg_segment(j, 0xC0, 15, 0x20);  // SOF0
  jpeg_segment(j, 0xC4, 30, 0x30);  // DHT
  const uint8_t entropy[] = {0x12, 0x34, 0x56, 0x78, 0x9A};
  jpeg_sos_and_data(j, entropy, sizeof entropy);
  jpeg_end(j);
}

static void test_finds_end_of_simple_frame(void) {
  Jpeg j;
  simple_jpeg(&j);
  CHECK_EQ_INT(find_jpeg_end(j.buf, j.len), j.len);
}

// Every prefix shorter than the whole frame must report "need more data"
// (0) -- never a premature end, and never a bogus one. This is the case a
// naive scan for FF D9 gets wrong.
static void test_every_short_prefix_needs_more(void) {
  Jpeg j;
  simple_jpeg(&j);
  int premature = 0;
  for (size_t n = 2; n < j.len; n++) {
    if (find_jpeg_end(j.buf, n) != 0) {
      premature++;
      if (premature <= 3)
        printf("    (prefix %zu returned %zu, want 0)\n", n,
               find_jpeg_end(j.buf, n));
    }
  }
  CHECK_EQ_INT(premature, 0);
}

// An APP segment carrying a thumbnail or vendor blob can contain the bytes
// FF D9 as ordinary payload. A scanner that just searched for that pair
// would cut the frame short right here.
static void test_ff_d9_inside_app_segment_is_not_the_end(void) {
  Jpeg j;
  jpeg_start(&j);
  // APP1 whose payload literally contains FF D9.
  j.buf[j.len++] = 0xFF;
  j.buf[j.len++] = 0xE1;
  const uint8_t payload[] = {0xAA, 0xFF, 0xD9, 0xBB, 0xFF, 0xD9, 0xCC};
  size_t seg = sizeof payload + 2;
  j.buf[j.len++] = (uint8_t)(seg >> 8);
  j.buf[j.len++] = (uint8_t)(seg & 0xFF);
  memcpy(j.buf + j.len, payload, sizeof payload);
  j.len += sizeof payload;
  jpeg_segment(&j, 0xC0, 15, 0x20);
  const uint8_t entropy[] = {0x11, 0x22, 0x33};
  jpeg_sos_and_data(&j, entropy, sizeof entropy);
  jpeg_end(&j);

  // Must return the true end, not the FF D9 buried in the APP1 payload.
  CHECK_EQ_INT(find_jpeg_end(j.buf, j.len), j.len);
}

// Inside entropy-coded data a literal FF is byte-stuffed as FF 00, and
// restart markers FF D0-D7 appear routinely. Neither ends the frame.
static void test_stuffed_bytes_and_restart_markers(void) {
  Jpeg j;
  jpeg_start(&j);
  jpeg_segment(&j, 0xC0, 15, 0x20);
  const uint8_t entropy[] = {
      0x12, 0xFF, 0x00,       // stuffed literal FF
      0x34, 0xFF, 0xD0,       // restart marker 0
      0x56, 0xFF, 0xD7,       // restart marker 7
      0x78, 0xFF, 0x00, 0x9A, // another stuffed FF
  };
  jpeg_sos_and_data(&j, entropy, sizeof entropy);
  jpeg_end(&j);
  CHECK_EQ_INT(find_jpeg_end(j.buf, j.len), j.len);
}

// Back-to-back frames on a pipe: the first call must return exactly the
// first frame's length, leaving the rest untouched for the next call.
static void test_consecutive_frames_split_exactly(void) {
  Jpeg a, b;
  simple_jpeg(&a);
  jpeg_start(&b);
  jpeg_segment(&b, 0xDB, 32, 0x40);
  jpeg_segment(&b, 0xC0, 15, 0x50);
  const uint8_t entropy[] = {0xDE, 0xAD, 0xBE, 0xEF};
  jpeg_sos_and_data(&b, entropy, sizeof entropy);
  jpeg_end(&b);

  uint8_t stream[131072];
  memcpy(stream, a.buf, a.len);
  memcpy(stream + a.len, b.buf, b.len);
  size_t total = a.len + b.len;

  size_t first = find_jpeg_end(stream, total);
  CHECK_EQ_INT(first, a.len);
  size_t second = find_jpeg_end(stream + first, total - first);
  CHECK_EQ_INT(second, b.len);
  CHECK_EQ_INT(first + second, total);
}

// Data that isn't a JPEG at all must be reported as a stream-level error
// (SIZE_MAX), not as "need more bytes" -- otherwise the reader waits
// forever for a frame that will never come.
static void test_non_jpeg_is_rejected_not_buffered(void) {
  uint8_t junk[64];
  memset(junk, 'A', sizeof junk);
  CHECK(find_jpeg_end(junk, sizeof junk) == SIZE_MAX);

  // Right length, wrong magic.
  uint8_t almost[] = {0xFF, 0xD7, 0x00, 0x01};
  CHECK(find_jpeg_end(almost, sizeof almost) == SIZE_MAX);

  // A segment claiming an impossible length.
  uint8_t bad_len[] = {0xFF, 0xD8, 0xFF, 0xDB, 0x00, 0x01, 0x00, 0x00};
  CHECK(find_jpeg_end(bad_len, sizeof bad_len) == SIZE_MAX);
}

// Fewer than 2 bytes cannot even hold SOI: "need more", not an error.
static void test_tiny_inputs(void) {
  const uint8_t soi[] = {0xFF, 0xD8};
  CHECK_EQ_INT(find_jpeg_end(soi, 0), 0);
  CHECK_EQ_INT(find_jpeg_end(soi, 1), 0);
  CHECK_EQ_INT(find_jpeg_end(soi, 2), 0); // valid SOI, nothing after it yet
}

// A fill byte (FF FF) before a real marker is legal padding.
static void test_fill_bytes_before_marker(void) {
  Jpeg j;
  jpeg_start(&j);
  j.buf[j.len++] = 0xFF; // fill
  j.buf[j.len++] = 0xFF;
  jpeg_segment(&j, 0xC0, 15, 0x20);
  const uint8_t entropy[] = {0x01, 0x02};
  jpeg_sos_and_data(&j, entropy, sizeof entropy);
  jpeg_end(&j);
  CHECK_EQ_INT(find_jpeg_end(j.buf, j.len), j.len);
}

// Byte-by-byte feeding, the way the real reader accumulates from a pipe:
// the answer must be 0 until the very last byte arrives, then exact.
static void test_incremental_feed_is_stable(void) {
  Jpeg j;
  simple_jpeg(&j);
  size_t answer = 0;
  for (size_t n = 0; n <= j.len; n++) {
    answer = find_jpeg_end(j.buf, n);
    if (n < j.len)
      CHECK_EQ_INT(answer, 0);
  }
  CHECK_EQ_INT(answer, j.len);
}

int main(void) {
  printf("test_jpeg_framing:\n");
  RUN_TEST(test_finds_end_of_simple_frame);
  RUN_TEST(test_every_short_prefix_needs_more);
  RUN_TEST(test_ff_d9_inside_app_segment_is_not_the_end);
  RUN_TEST(test_stuffed_bytes_and_restart_markers);
  RUN_TEST(test_consecutive_frames_split_exactly);
  RUN_TEST(test_non_jpeg_is_rejected_not_buffered);
  RUN_TEST(test_tiny_inputs);
  RUN_TEST(test_fill_bytes_before_marker);
  RUN_TEST(test_incremental_feed_is_stable);
  TEST_MAIN_END();
}
