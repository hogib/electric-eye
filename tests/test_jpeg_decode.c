// MJPEG decode into VideoFrame's I422 planes, for both subsamplings the
// pipeline accepts.
//
// The 4:2:0 path is the one worth pinning: rpicam-vid's MJPEG is 4:2:0,
// which has half-height chroma that has to be line-doubled into I422's
// full-height planes. Get that wrong and colour is vertically misaligned
// or half the chroma plane is stale -- both subtle enough to survive a
// glance at the picture.
//
// Test JPEGs are encoded here with libjpeg-turbo rather than shipped as
// binaries, so the fixtures are readable and can't drift from what they
// claim to be.
#include "jpeg_decode.h"
#include "test_harness.h"
#include "video_frame.h"
#include <stdlib.h>
#include <turbojpeg.h>

// Encodes planar YUV as a JPEG at the given subsampling. Returns a malloc'd
// buffer the caller frees.
static uint8_t *encode(const uint8_t *y, const uint8_t *u, const uint8_t *v,
                       uint32_t w, uint32_t h, int subsamp, size_t *out_size) {
  tjhandle enc = tjInitCompress();
  if (!enc)
    return NULL;
  const uint8_t *planes[3] = {y, u, v};
  int strides[3] = {(int)w, (int)tjPlaneWidth(1, (int)w, subsamp),
                    (int)tjPlaneWidth(2, (int)w, subsamp)};
  unsigned char *buf = NULL;
  unsigned long size = 0;
  // Quality 100 so decode differences come from the layout conversion
  // under test, not from quantization noise.
  int rc = tjCompressFromYUVPlanes(enc, planes, (int)w, strides, (int)h,
                                   subsamp, &buf, &size, 100, 0);
  tjDestroy(enc);
  if (rc < 0) {
    tjFree(buf);
    return NULL;
  }
  uint8_t *copy = malloc(size);
  memcpy(copy, buf, size);
  tjFree(buf);
  *out_size = size;
  return copy;
}

// Builds source planes at `subsamp`'s natural chroma dimensions.
typedef struct {
  uint8_t *y, *u, *v;
  uint32_t cw, ch;
} Source;

static Source make_source(uint32_t w, uint32_t h, int subsamp) {
  Source s;
  s.cw = (uint32_t)tjPlaneWidth(1, (int)w, subsamp);
  s.ch = (uint32_t)tjPlaneHeight(1, (int)h, subsamp);
  s.y = malloc((size_t)w * h);
  s.u = malloc((size_t)s.cw * s.ch);
  s.v = malloc((size_t)s.cw * s.ch);
  return s;
}

static void free_source(Source *s) { free(s->y); free(s->u); free(s->v); }

static void test_422_decodes_to_matching_planes(void) {
  const uint32_t W = 64, H = 32;
  Source s = make_source(W, H, TJSAMP_422);
  memset(s.y, 120, (size_t)W * H);
  memset(s.u, 90, (size_t)s.cw * s.ch);
  memset(s.v, 180, (size_t)s.cw * s.ch);

  size_t n;
  uint8_t *jpeg = encode(s.y, s.u, s.v, W, H, TJSAMP_422, &n);
  CHECK(jpeg != NULL);
  if (!jpeg) { free_source(&s); return; }

  JpegDecoder *dec = jpeg_decoder_create(W, H);
  VideoFrame *f = vf_create(W, H, 0);
  memset(f->raw_data, 0xA5, f->plane_sizes[0] + f->plane_sizes[1] +
                                f->plane_sizes[2]);

  CHECK_EQ_INT(jpeg_decode_frame(dec, jpeg, n, f), JPEG_DECODE_OK);

  // Flat input, so every decoded sample should sit at the source value
  // (within JPEG's rounding).
  for (size_t i = 0; i < f->plane_sizes[0]; i++)
    CHECK(abs((int)f->raw_planes[0][i] - 120) <= 2);
  for (size_t i = 0; i < f->plane_sizes[1]; i++)
    CHECK(abs((int)f->raw_planes[1][i] - 90) <= 2);
  for (size_t i = 0; i < f->plane_sizes[2]; i++)
    CHECK(abs((int)f->raw_planes[2][i] - 180) <= 2);

  vf_free(f); jpeg_decoder_destroy(dec); free(jpeg); free_source(&s);
}

// The core 4:2:0 case: half-height chroma must fill the full-height I422
// planes, with no unwritten bytes left behind.
static void test_420_fills_every_chroma_byte(void) {
  const uint32_t W = 64, H = 32;
  Source s = make_source(W, H, TJSAMP_420);
  memset(s.y, 100, (size_t)W * H);
  memset(s.u, 70, (size_t)s.cw * s.ch);
  memset(s.v, 200, (size_t)s.cw * s.ch);

  size_t n;
  uint8_t *jpeg = encode(s.y, s.u, s.v, W, H, TJSAMP_420, &n);
  CHECK(jpeg != NULL);
  if (!jpeg) { free_source(&s); return; }

  JpegDecoder *dec = jpeg_decoder_create(W, H);
  VideoFrame *f = vf_create(W, H, 0);
  // Poison: any byte the decode fails to write stays 0xA5, so a
  // half-filled chroma plane cannot pass unnoticed.
  memset(f->raw_data, 0xA5, f->plane_sizes[0] + f->plane_sizes[1] +
                                f->plane_sizes[2]);

  CHECK_EQ_INT(jpeg_decode_frame(dec, jpeg, n, f), JPEG_DECODE_OK);

  for (size_t i = 0; i < f->plane_sizes[1]; i++)
    CHECK(abs((int)f->raw_planes[1][i] - 70) <= 2);
  for (size_t i = 0; i < f->plane_sizes[2]; i++)
    CHECK(abs((int)f->raw_planes[2][i] - 200) <= 2);

  vf_free(f); jpeg_decoder_destroy(dec); free(jpeg); free_source(&s);
}

// Line doubling means output chroma rows 2k and 2k+1 come from the same
// source row, so they must be byte-identical. A vertical misalignment or
// an off-by-one row would break this even while every byte is written.
static void test_420_chroma_rows_are_doubled_in_pairs(void) {
  const uint32_t W = 32, H = 32;
  Source s = make_source(W, H, TJSAMP_420);
  memset(s.y, 128, (size_t)W * H);
  // A strong vertical chroma gradient: each source row distinct, so any
  // row mix-up shows up immediately.
  for (uint32_t cy = 0; cy < s.ch; cy++)
    for (uint32_t cx = 0; cx < s.cw; cx++) {
      s.u[(size_t)cy * s.cw + cx] = (uint8_t)(cy * 16);
      s.v[(size_t)cy * s.cw + cx] = (uint8_t)(255 - cy * 16);
    }

  size_t n;
  uint8_t *jpeg = encode(s.y, s.u, s.v, W, H, TJSAMP_420, &n);
  CHECK(jpeg != NULL);
  if (!jpeg) { free_source(&s); return; }

  JpegDecoder *dec = jpeg_decoder_create(W, H);
  VideoFrame *f = vf_create(W, H, 0);
  CHECK_EQ_INT(jpeg_decode_frame(dec, jpeg, n, f), JPEG_DECODE_OK);

  const size_t cstride = f->stride[1];
  for (uint32_t k = 0; k < H / 2; k++) {
    const uint8_t *r0 = f->raw_planes[1] + (size_t)(2 * k) * cstride;
    const uint8_t *r1 = f->raw_planes[1] + (size_t)(2 * k + 1) * cstride;
    CHECK_MEM_EQ(r1, r0, cstride);
    const uint8_t *s0 = f->raw_planes[2] + (size_t)(2 * k) * cstride;
    const uint8_t *s1 = f->raw_planes[2] + (size_t)(2 * k + 1) * cstride;
    CHECK_MEM_EQ(s1, s0, cstride);
  }

  vf_free(f); jpeg_decoder_destroy(dec); free(jpeg); free_source(&s);
}

// Chroma must not end up vertically flipped or shifted: the gradient's
// direction in the output has to match the source's.
static void test_420_chroma_is_not_vertically_shifted(void) {
  const uint32_t W = 32, H = 32;
  Source s = make_source(W, H, TJSAMP_420);
  memset(s.y, 128, (size_t)W * H);
  for (uint32_t cy = 0; cy < s.ch; cy++)
    for (uint32_t cx = 0; cx < s.cw; cx++) {
      s.u[(size_t)cy * s.cw + cx] = (uint8_t)(cy * 16);
      s.v[(size_t)cy * s.cw + cx] = 128;
    }

  size_t n;
  uint8_t *jpeg = encode(s.y, s.u, s.v, W, H, TJSAMP_420, &n);
  CHECK(jpeg != NULL);
  if (!jpeg) { free_source(&s); return; }

  JpegDecoder *dec = jpeg_decoder_create(W, H);
  VideoFrame *f = vf_create(W, H, 0);
  CHECK_EQ_INT(jpeg_decode_frame(dec, jpeg, n, f), JPEG_DECODE_OK);

  const size_t cstride = f->stride[1];
  // Top output rows come from source row 0 (value 0); bottom rows from the
  // last source row (value (ch-1)*16).
  CHECK(f->raw_planes[1][0] < 40);
  const uint8_t *last = f->raw_planes[1] + (size_t)(H - 1) * cstride;
  CHECK(last[0] > (uint8_t)((s.ch - 1) * 16) - 40);
  // ...and it must increase monotonically down the plane, not wander.
  int decreases = 0;
  for (uint32_t y = 1; y < H; y++) {
    int prev = f->raw_planes[1][(size_t)(y - 1) * cstride];
    int cur = f->raw_planes[1][(size_t)y * cstride];
    if (cur + 2 < prev)
      decreases++;
  }
  CHECK_EQ_INT(decreases, 0);

  vf_free(f); jpeg_decoder_destroy(dec); free(jpeg); free_source(&s);
}

// U and V must not be swapped -- an easy mistake in a two-plane copy, and
// one that shows up as wrong hues rather than obvious corruption.
static void test_420_does_not_swap_u_and_v(void) {
  const uint32_t W = 32, H = 32;
  Source s = make_source(W, H, TJSAMP_420);
  memset(s.y, 128, (size_t)W * H);
  memset(s.u, 40, (size_t)s.cw * s.ch);   // distinctly low
  memset(s.v, 210, (size_t)s.cw * s.ch);  // distinctly high

  size_t n;
  uint8_t *jpeg = encode(s.y, s.u, s.v, W, H, TJSAMP_420, &n);
  CHECK(jpeg != NULL);
  if (!jpeg) { free_source(&s); return; }

  JpegDecoder *dec = jpeg_decoder_create(W, H);
  VideoFrame *f = vf_create(W, H, 0);
  CHECK_EQ_INT(jpeg_decode_frame(dec, jpeg, n, f), JPEG_DECODE_OK);
  CHECK(f->raw_planes[1][0] < 100); // U stayed low
  CHECK(f->raw_planes[2][0] > 150); // V stayed high

  vf_free(f); jpeg_decoder_destroy(dec); free(jpeg); free_source(&s);
}

// Downscale happens inside the decode; both subsamplings must honour it
// and produce a fully-written frame at the reduced size.
static void test_downscale_during_decode(void) {
  const uint32_t W = 64, H = 64;
  const int subsamps[] = {TJSAMP_422, TJSAMP_420};
  for (int si = 0; si < 2; si++) {
    Source s = make_source(W, H, subsamps[si]);
    memset(s.y, 150, (size_t)W * H);
    memset(s.u, 80, (size_t)s.cw * s.ch);
    memset(s.v, 170, (size_t)s.cw * s.ch);
    size_t n;
    uint8_t *jpeg = encode(s.y, s.u, s.v, W, H, subsamps[si], &n);
    CHECK(jpeg != NULL);
    if (!jpeg) { free_source(&s); continue; }

    for (uint32_t ds = 2; ds <= 8; ds *= 2) {
      JpegDecoder *dec = jpeg_decoder_create(W, H);
      VideoFrame *f = vf_create(W / ds, H / ds, 0);
      memset(f->raw_data, 0xA5, f->plane_sizes[0] + f->plane_sizes[1] +
                                    f->plane_sizes[2]);
      CHECK_EQ_INT(jpeg_decode_frame(dec, jpeg, n, f), JPEG_DECODE_OK);
      CHECK_EQ_INT(f->width, W / ds);
      for (size_t i = 0; i < f->plane_sizes[1]; i++)
        CHECK(abs((int)f->raw_planes[1][i] - 80) <= 3);
      vf_free(f);
      jpeg_decoder_destroy(dec);
    }
    free(jpeg);
    free_source(&s);
  }
}

// A source whose dimensions disagree with what the decoder was created for
// means the camera changed format mid-stream: not transient, so it must be
// reported as a mismatch rather than retried forever.
static void test_dimension_change_is_format_mismatch(void) {
  const uint32_t W = 64, H = 32;
  Source s = make_source(W, H, TJSAMP_422);
  memset(s.y, 100, (size_t)W * H);
  memset(s.u, 128, (size_t)s.cw * s.ch);
  memset(s.v, 128, (size_t)s.cw * s.ch);
  size_t n;
  uint8_t *jpeg = encode(s.y, s.u, s.v, W, H, TJSAMP_422, &n);
  CHECK(jpeg != NULL);
  if (!jpeg) { free_source(&s); return; }

  JpegDecoder *dec = jpeg_decoder_create(W * 2, H); // expects a wider frame
  VideoFrame *f = vf_create(W * 2, H, 0);
  CHECK_EQ_INT(jpeg_decode_frame(dec, jpeg, n, f), JPEG_DECODE_FORMAT_MISMATCH);

  vf_free(f); jpeg_decoder_destroy(dec); free(jpeg); free_source(&s);
}

// 4:4:4 is neither of the two supported layouts and must be refused
// clearly rather than producing misaligned chroma.
static void test_unsupported_subsampling_is_rejected(void) {
  const uint32_t W = 32, H = 32;
  Source s = make_source(W, H, TJSAMP_444);
  memset(s.y, 100, (size_t)W * H);
  memset(s.u, 128, (size_t)s.cw * s.ch);
  memset(s.v, 128, (size_t)s.cw * s.ch);
  size_t n;
  uint8_t *jpeg = encode(s.y, s.u, s.v, W, H, TJSAMP_444, &n);
  CHECK(jpeg != NULL);
  if (!jpeg) { free_source(&s); return; }

  JpegDecoder *dec = jpeg_decoder_create(W, H);
  VideoFrame *f = vf_create(W, H, 0);
  CHECK_EQ_INT(jpeg_decode_frame(dec, jpeg, n, f), JPEG_DECODE_FORMAT_MISMATCH);

  vf_free(f); jpeg_decoder_destroy(dec); free(jpeg); free_source(&s);
}

// Garbage must be a transient failure (worth a retry -- an isolated bad
// USB transfer is real), not a format mismatch that tears the stream down.
static void test_corrupt_payload_is_transient(void) {
  uint8_t junk[512];
  memset(junk, 0x5A, sizeof junk);
  junk[0] = 0xFF;
  junk[1] = 0xD8; // looks like a JPEG, isn't one

  JpegDecoder *dec = jpeg_decoder_create(32, 32);
  VideoFrame *f = vf_create(32, 32, 0);
  CHECK_EQ_INT(jpeg_decode_frame(dec, junk, sizeof junk, f),
               JPEG_DECODE_TRANSIENT_FAIL);
  vf_free(f); jpeg_decoder_destroy(dec);
}

// Decoding many frames through one decoder must not drift: the scratch
// buffers are reused across calls, so a stale-state bug shows up on the
// second and later frames rather than the first.
static void test_decoder_reuse_is_stable(void) {
  const uint32_t W = 32, H = 32;
  Source s = make_source(W, H, TJSAMP_420);
  memset(s.y, 111, (size_t)W * H);
  memset(s.u, 60, (size_t)s.cw * s.ch);
  memset(s.v, 190, (size_t)s.cw * s.ch);
  size_t n;
  uint8_t *jpeg = encode(s.y, s.u, s.v, W, H, TJSAMP_420, &n);
  CHECK(jpeg != NULL);
  if (!jpeg) { free_source(&s); return; }

  JpegDecoder *dec = jpeg_decoder_create(W, H);
  VideoFrame *first = vf_create(W, H, 0);
  CHECK_EQ_INT(jpeg_decode_frame(dec, jpeg, n, first), JPEG_DECODE_OK);

  for (int i = 0; i < 10; i++) {
    VideoFrame *f = vf_create(W, H, 0);
    CHECK_EQ_INT(jpeg_decode_frame(dec, jpeg, n, f), JPEG_DECODE_OK);
    CHECK_MEM_EQ(f->raw_planes[1], first->raw_planes[1], f->plane_sizes[1]);
    CHECK_MEM_EQ(f->raw_planes[2], first->raw_planes[2], f->plane_sizes[2]);
    vf_free(f);
  }
  vf_free(first); jpeg_decoder_destroy(dec); free(jpeg); free_source(&s);
}

int main(void) {
  printf("test_jpeg_decode:\n");
  RUN_TEST(test_422_decodes_to_matching_planes);
  RUN_TEST(test_420_fills_every_chroma_byte);
  RUN_TEST(test_420_chroma_rows_are_doubled_in_pairs);
  RUN_TEST(test_420_chroma_is_not_vertically_shifted);
  RUN_TEST(test_420_does_not_swap_u_and_v);
  RUN_TEST(test_downscale_during_decode);
  RUN_TEST(test_dimension_change_is_format_mismatch);
  RUN_TEST(test_unsupported_subsampling_is_rejected);
  RUN_TEST(test_corrupt_payload_is_transient);
  RUN_TEST(test_decoder_reuse_is_stable);
  TEST_MAIN_END();
}
