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

// The reader resumes the scan where the last read left off instead of
// rescanning from byte 0. Fed in every chunk size -- so that chunk
// boundaries land on every byte, including between an FF and its marker
// byte -- it must agree with the one-shot scan exactly.
static void test_resumed_scan_matches_one_shot(void) {
  Jpeg j;
  jpeg_start(&j);
  jpeg_segment(&j, 0xDB, 64, 0x10);
  // Entropy data dense with the tricky cases: stuffed FF 00, restart
  // markers, fill bytes, and an FF right before EOI.
  uint8_t entropy[300];
  for (size_t i = 0; i < sizeof entropy; i += 6) {
    entropy[i] = 0x11;
    entropy[i + 1] = 0xFF;
    entropy[i + 2] = 0x00;
    entropy[i + 3] = 0xFF;
    entropy[i + 4] = (uint8_t)(0xD0 + i % 8);
    entropy[i + 5] = 0xFF;
  }
  entropy[sizeof entropy - 1] = 0x00; // ends on a stuffed FF 00 pair
  jpeg_sos_and_data(&j, entropy, sizeof entropy);
  jpeg_end(&j);
  const size_t whole = find_jpeg_end(j.buf, j.len);
  CHECK_EQ_INT(whole, j.len);

  for (size_t chunk = 1; chunk <= 40; chunk++) {
    size_t resume = 0, have = 0, answer = 0;
    while (answer == 0 && have < j.len) {
      have = have + chunk > j.len ? j.len : have + chunk;
      answer = find_jpeg_end_resumable(j.buf, have, &resume);
      if (have < j.len)
        CHECK_EQ_INT(answer, 0);
    }
    CHECK_EQ_INT(answer, whole);
  }
}

// --- yuv420 capture -----------------------------------------------------

// Writes one stride-padded yuv420 frame the way rpicam-vid lays it out:
// Y rows padded to 64 bytes, U and V rows to 32, padding filled with a
// value that must never reach the output.
static size_t make_yuv420(uint8_t *out, uint32_t w, uint32_t h, uint8_t seed) {
  size_t ys = (w + 63) & ~63u, cs = (w / 2 + 31) & ~31u;
  memset(out, 0xEE, ys * h + 2 * cs * (h / 2));
  for (uint32_t y = 0; y < h; y++)
    for (uint32_t x = 0; x < w; x++)
      out[y * ys + x] = (uint8_t)(x * 3 + y * 5 + seed);
  uint8_t *u = out + ys * h, *v = u + cs * (h / 2);
  for (uint32_t y = 0; y < h / 2; y++)
    for (uint32_t x = 0; x < w / 2; x++) {
      u[y * cs + x] = (uint8_t)(x * 7 + y + seed);
      v[y * cs + x] = (uint8_t)(255 - x - y * 9 - seed);
    }
  return ys * h + 2 * cs * (h / 2);
}

// Reads frames from a real pipe through capture_yuv420 and checks them
// against an independent unpack: padding stripped, chroma line-doubled at
// downscale 1, n x n/2 box-averaged otherwise.
static void check_yuv420_capture(uint32_t w, uint32_t h, uint32_t n) {
  int fds[2];
  CHECK(pipe(fds) == 0);
  RpicamIn in = {.fd = fds[0], .width = w, .height = h, .downscale = n,
                 .format = FORMAT_YUV420};
  in.y_stride = (w + 63) & ~63u;
  in.c_stride = (w / 2 + 31) & ~31u;
  in.yuv_frame_bytes = in.y_stride * h + 2 * in.c_stride * (h / 2);
  in.buf = malloc(in.yuv_frame_bytes);
  in.buf_cap = in.yuv_frame_bytes;

  uint8_t *src = malloc(in.yuv_frame_bytes * 2);
  make_yuv420(src, w, h, 1);
  make_yuv420(src + in.yuv_frame_bytes, w, h, 2);
  // Small frames only: both must fit in the pipe without a reader.
  CHECK(write(fds[1], src, in.yuv_frame_bytes * 2) ==
        (ssize_t)(in.yuv_frame_bytes * 2));

  VideoFrame *f = vf_create(w / n, h / n, 0);
  for (int k = 0; k < 2; k++) {
    const uint8_t *ys = src + k * in.yuv_frame_bytes;
    const uint8_t *us = ys + in.y_stride * h;
    const uint8_t *vs = us + in.c_stride * (h / 2);
    CHECK(capture_yuv420(&in, f));
    int bad = 0;
    for (uint32_t oy = 0; oy < h / n && !bad; oy++) {
      for (uint32_t ox = 0; ox < w / n; ox++) {
        uint32_t sum = 0;
        for (uint32_t dy = 0; dy < n; dy++)
          for (uint32_t dx = 0; dx < n; dx++)
            sum += ys[(oy * n + dy) * in.y_stride + ox * n + dx];
        uint8_t want = (uint8_t)((sum + n * n / 2) / (n * n));
        if (f->raw_planes[0][oy * f->stride[0] + ox] != want)
          bad = 1;
      }
      for (uint32_t cx = 0; cx < w / n / 2; cx++) {
        for (int p = 1; p <= 2; p++) {
          const uint8_t *cp = p == 1 ? us : vs;
          uint32_t sum = 0, cnt = 0;
          // Output row oy covers source luma rows [oy*n, oy*n + n), i.e.
          // chroma rows [oy*n/2, (oy*n + n)/2) -- one row, shared by two
          // output rows, at n == 1.
          uint32_t r0 = oy * n / 2, r1 = (oy * n + n + 1) / 2;
          for (uint32_t r = r0; r < r1; r++)
            for (uint32_t dx = 0; dx < n; dx++, cnt++)
              sum += cp[r * in.c_stride + cx * n + dx];
          uint8_t want = (uint8_t)((sum + cnt / 2) / cnt);
          if (f->raw_planes[p][oy * f->stride[p] + cx] != want)
            bad = 1;
        }
      }
    }
    CHECK(!bad);
    CHECK_EQ_INT(f->source_jpeg_len, 0);
  }
  vf_free(f);
  free(src);
  free(in.buf);
  close(fds[0]);
  close(fds[1]);
}

static void test_yuv420_capture_strips_padding_and_doubles_chroma(void) {
  check_yuv420_capture(64, 16, 1); // no padding at all
  check_yuv420_capture(80, 12, 1); // Y and chroma rows both padded
}

static void test_yuv420_capture_downscales(void) {
  check_yuv420_capture(64, 16, 2);
  check_yuv420_capture(96, 16, 4);
  check_yuv420_capture(128, 16, 8);
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
  RUN_TEST(test_resumed_scan_matches_one_shot);
  RUN_TEST(test_yuv420_capture_strips_padding_and_doubles_chroma);
  RUN_TEST(test_yuv420_capture_downscales);
  TEST_MAIN_END();
}
