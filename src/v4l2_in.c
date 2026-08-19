// _POSIX_C_SOURCE before any header include: -std=c23 puts glibc in strict
// ISO mode, which hides open()/mmap()/poll() and friends unless a
// feature-test macro asks for them explicitly. Same guard as
// config.c/eeye.c.
#define _POSIX_C_SOURCE 200809L

#include "v4l2_in.h"
#include "v4l2_ioctl.h"
#include "video_frame.h"
#include <errno.h>
#include <fcntl.h>
#include <linux/videodev2.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <turbojpeg.h>
#include <unistd.h>

// Same architecture detection as point_opps.c/conv.c (duplicated rather
// than shared via a header, matching those files' existing convention).
#if defined(__aarch64__)
#include <arm_neon.h>
#define GS_NEON_AARCH64 1
#elif defined(__arm__) && defined(__ARM_NEON)
#include <arm_neon.h>
#define GS_NEON_ARM32 1
#endif

constexpr uint32_t v4l2_in_max_buffers = 8;

struct V4l2In {
  int fd;
  // The geometry the *camera* delivers -- not the geometry of the frames
  // handed back by v4l2_in_read_frame(), which are these divided by
  // downscale. Every format negotiation and raw-buffer size check below
  // uses these; every write into a VideoFrame uses the frame's own
  // (already-divided) dimensions.
  uint32_t width;
  uint32_t height;
  uint32_t downscale;
  uint32_t n_buffers;
  struct {
    void *start;
    size_t length;
  } buffers[8]; // sized by v4l2_in_max_buffers
  uint32_t capture_format; // V4L2_PIX_FMT_MJPEG or V4L2_PIX_FMT_YUYV
  tjhandle jpeg_decoder;   // only set up when capture_format is MJPEG

  // Scratch space for a DHT-augmented copy of a captured frame, grown
  // lazily via realloc() the first time a frame actually needs it (a
  // camera that already emits DHT, the common case, never touches this).
  // See jpeg_has_dht()/inject_standard_dht() below.
  uint8_t *dht_scratch;
  size_t dht_scratch_cap;
};

typedef enum {
  DECODE_OK,
  DECODE_TRANSIENT_FAIL,  // malformed/corrupt JPEG payload -- worth a retry
  DECODE_FORMAT_MISMATCH, // JPEG's actual format disagrees with what open()
                          // already validated -- not transient, don't retry
} DecodeResult;

// Standard JPEG Huffman tables (ITU-T.81 Annex K.3), used by essentially
// every baseline JPEG/MJPEG encoder that doesn't ship custom ones --
// including, per a documented real-world quirk, some UVC webcams that omit
// the DHT marker segment entirely from their MJPEG stream and rely on the
// decoder already knowing these. libjpeg-turbo makes no such assumption; a
// camera that omits DHT fails tjDecompressHeader3/ToYUVPlanes on every
// single frame without this.
//
// Sourced from ffmpeg's libavcodec/mjpeg.c (avpriv_mjpeg_bits_*/
// avpriv_mjpeg_val_*), which cites the same JPEG standard section K.3.
// Cross-checked here independently of that source by confirming each
// bits[] array's sum (over indices 1-16; index 0 is an unused placeholder)
// equals its paired val[] array's length: 12/12 for both DC tables, 162/162
// for both AC tables -- the well-known standard table sizes.
static const uint8_t std_dc_luminance_bits[17] = {
    0, 0, 1, 5, 1, 1, 1, 1, 1, 1, 0, 0, 0, 0, 0, 0, 0};
static const uint8_t std_dc_luminance_val[12] = {0, 1, 2, 3, 4,  5,
                                                 6, 7, 8, 9, 10, 11};

static const uint8_t std_dc_chrominance_bits[17] = {
    0, 0, 3, 1, 1, 1, 1, 1, 1, 1, 1, 1, 0, 0, 0, 0, 0};
static const uint8_t std_dc_chrominance_val[12] = {0, 1, 2, 3, 4,  5,
                                                    6, 7, 8, 9, 10, 11};

static const uint8_t std_ac_luminance_bits[17] = {
    0, 0, 2, 1, 3, 3, 2, 4, 3, 5, 5, 4, 4, 0, 0, 1, 0x7d};
static const uint8_t std_ac_luminance_val[162] = {
    0x01, 0x02, 0x03, 0x00, 0x04, 0x11, 0x05, 0x12, 0x21, 0x31, 0x41, 0x06,
    0x13, 0x51, 0x61, 0x07, 0x22, 0x71, 0x14, 0x32, 0x81, 0x91, 0xa1, 0x08,
    0x23, 0x42, 0xb1, 0xc1, 0x15, 0x52, 0xd1, 0xf0, 0x24, 0x33, 0x62, 0x72,
    0x82, 0x09, 0x0a, 0x16, 0x17, 0x18, 0x19, 0x1a, 0x25, 0x26, 0x27, 0x28,
    0x29, 0x2a, 0x34, 0x35, 0x36, 0x37, 0x38, 0x39, 0x3a, 0x43, 0x44, 0x45,
    0x46, 0x47, 0x48, 0x49, 0x4a, 0x53, 0x54, 0x55, 0x56, 0x57, 0x58, 0x59,
    0x5a, 0x63, 0x64, 0x65, 0x66, 0x67, 0x68, 0x69, 0x6a, 0x73, 0x74, 0x75,
    0x76, 0x77, 0x78, 0x79, 0x7a, 0x83, 0x84, 0x85, 0x86, 0x87, 0x88, 0x89,
    0x8a, 0x92, 0x93, 0x94, 0x95, 0x96, 0x97, 0x98, 0x99, 0x9a, 0xa2, 0xa3,
    0xa4, 0xa5, 0xa6, 0xa7, 0xa8, 0xa9, 0xaa, 0xb2, 0xb3, 0xb4, 0xb5, 0xb6,
    0xb7, 0xb8, 0xb9, 0xba, 0xc2, 0xc3, 0xc4, 0xc5, 0xc6, 0xc7, 0xc8, 0xc9,
    0xca, 0xd2, 0xd3, 0xd4, 0xd5, 0xd6, 0xd7, 0xd8, 0xd9, 0xda, 0xe1, 0xe2,
    0xe3, 0xe4, 0xe5, 0xe6, 0xe7, 0xe8, 0xe9, 0xea, 0xf1, 0xf2, 0xf3, 0xf4,
    0xf5, 0xf6, 0xf7, 0xf8, 0xf9, 0xfa};

static const uint8_t std_ac_chrominance_bits[17] = {
    0, 0, 2, 1, 2, 4, 4, 3, 4, 7, 5, 4, 4, 0, 1, 2, 0x77};
static const uint8_t std_ac_chrominance_val[162] = {
    0x00, 0x01, 0x02, 0x03, 0x11, 0x04, 0x05, 0x21, 0x31, 0x06, 0x12, 0x41,
    0x51, 0x07, 0x61, 0x71, 0x13, 0x22, 0x32, 0x81, 0x08, 0x14, 0x42, 0x91,
    0xa1, 0xb1, 0xc1, 0x09, 0x23, 0x33, 0x52, 0xf0, 0x15, 0x62, 0x72, 0xd1,
    0x0a, 0x16, 0x24, 0x34, 0xe1, 0x25, 0xf1, 0x17, 0x18, 0x19, 0x1a, 0x26,
    0x27, 0x28, 0x29, 0x2a, 0x35, 0x36, 0x37, 0x38, 0x39, 0x3a, 0x43, 0x44,
    0x45, 0x46, 0x47, 0x48, 0x49, 0x4a, 0x53, 0x54, 0x55, 0x56, 0x57, 0x58,
    0x59, 0x5a, 0x63, 0x64, 0x65, 0x66, 0x67, 0x68, 0x69, 0x6a, 0x73, 0x74,
    0x75, 0x76, 0x77, 0x78, 0x79, 0x7a, 0x82, 0x83, 0x84, 0x85, 0x86, 0x87,
    0x88, 0x89, 0x8a, 0x92, 0x93, 0x94, 0x95, 0x96, 0x97, 0x98, 0x99, 0x9a,
    0xa2, 0xa3, 0xa4, 0xa5, 0xa6, 0xa7, 0xa8, 0xa9, 0xaa, 0xb2, 0xb3, 0xb4,
    0xb5, 0xb6, 0xb7, 0xb8, 0xb9, 0xba, 0xc2, 0xc3, 0xc4, 0xc5, 0xc6, 0xc7,
    0xc8, 0xc9, 0xca, 0xd2, 0xd3, 0xd4, 0xd5, 0xd6, 0xd7, 0xd8, 0xd9, 0xda,
    0xe2, 0xe3, 0xe4, 0xe5, 0xe6, 0xe7, 0xe8, 0xe9, 0xea, 0xf2, 0xf3, 0xf4,
    0xf5, 0xf6, 0xf7, 0xf8, 0xf9, 0xfa};

// Scans JPEG marker segments starting right after SOI, stopping at the
// first SOS (entropy-coded scan data follows immediately, and marker
// segments never appear inside it -- so it's safe to stop looking) or as
// soon as a DHT (0xFFC4) is found. A naive byte-scan for the literal
// sequence FF C4 would risk a false positive: application-data segments
// (JFIF/EXIF/comments) frequently carry vendor binary blobs that could
// coincidentally contain that byte pair. Walking segment lengths instead
// sidesteps that entirely.
static bool jpeg_has_dht(const uint8_t *data, size_t size) {
  if (size < 4 || data[0] != 0xFF || data[1] != 0xD8) // SOI
    return false; // not a JPEG at all -- let the decoder report that

  size_t pos = 2;
  while (pos + 4 <= size) {
    if (data[pos] != 0xFF) {
      ++pos; // resync; shouldn't happen in well-formed marker data
      continue;
    }
    uint8_t marker = data[pos + 1];
    if (marker == 0xFF) {
      ++pos; // fill byte before the real marker code
      continue;
    }
    if (marker == 0x01 || (marker >= 0xD0 && marker <= 0xD8)) {
      pos += 2; // markers with no length field (TEM, RSTn, SOI)
      continue;
    }
    if (marker == 0xDA) // SOS
      return false;
    if (marker == 0xC4) // DHT
      return true;

    size_t seg_len = ((size_t)data[pos + 2] << 8) | data[pos + 3];
    if (seg_len < 2)
      return false; // malformed; let the decoder report it
    pos += 2 + seg_len;
  }
  return false;
}

// Writes one DHT marker segment (FF C4, 2-byte length, then all four
// standard tables back to back) to `out`, which must have room for at
// least 420 bytes (2 + 2 + 4 * (1 + 16 + table_val_len), summing to 418
// for the length field's own value -- independently matches both a real
// captured example's length field of 0x01A2 seen when researching this,
// and this project's own prior estimate of "~420 bytes"). Returns the
// number of bytes written.
static size_t build_standard_dht_segment(uint8_t *out) {
  struct {
    uint8_t tc_th; // high nibble: table class (0=DC,1=AC); low: table id
    const uint8_t *bits; // 17 elements; index 0 is the unused placeholder
    const uint8_t *val;
    size_t val_len;
  } tables[4] = {
      {0x00, std_dc_luminance_bits, std_dc_luminance_val, 12},
      {0x01, std_dc_chrominance_bits, std_dc_chrominance_val, 12},
      {0x10, std_ac_luminance_bits, std_ac_luminance_val, 162},
      {0x11, std_ac_chrominance_bits, std_ac_chrominance_val, 162},
  };

  size_t payload_len = 2; // the length field counts itself
  for (int t = 0; t < 4; ++t)
    payload_len += 1 + 16 + tables[t].val_len;

  size_t pos = 0;
  out[pos++] = 0xFF;
  out[pos++] = 0xC4;
  out[pos++] = (uint8_t)(payload_len >> 8);
  out[pos++] = (uint8_t)(payload_len & 0xFF);

  for (int t = 0; t < 4; ++t) {
    out[pos++] = tables[t].tc_th;
    memcpy(&out[pos], &tables[t].bits[1], 16); // skip the unused index 0
    pos += 16;
    memcpy(&out[pos], tables[t].val, tables[t].val_len);
    pos += tables[t].val_len;
  }

  return pos;
}

// If `data` has no DHT before its first SOS, copies it into in->dht_scratch
// (growing that buffer as needed) with a standard-tables DHT segment
// spliced in right after SOI -- always a valid insertion point, since DHT
// may appear anywhere before the SOS that references it. Points *out_data/
// *out_size at whichever buffer decode should actually use: the original,
// unmodified, if it already had a DHT (the common case costs nothing
// beyond the marker scan above), or the augmented scratch copy otherwise.
// Returns false only on allocation failure.
static bool ensure_dht(V4l2In *in, const uint8_t *data, size_t size,
                      const uint8_t **out_data, size_t *out_size) {
  if (size >= 2 && jpeg_has_dht(data, size)) {
    *out_data = data;
    *out_size = size;
    return true;
  }

  size_t needed = size + 420; // exact DHT segment size; see the comment above
  if (in->dht_scratch_cap < needed) {
    uint8_t *grown = realloc(in->dht_scratch, needed);
    if (!grown)
      return false;
    in->dht_scratch = grown;
    in->dht_scratch_cap = needed;
  }

  memcpy(in->dht_scratch, data, 2); // SOI
  size_t dht_len = build_standard_dht_segment(in->dht_scratch + 2);
  memcpy(in->dht_scratch + 2 + dht_len, data + 2, size - 2);

  *out_data = in->dht_scratch;
  *out_size = 2 + dht_len + (size - 2);
  return true;
}

static DecodeResult decode_mjpeg_frame(V4l2In *in, const uint8_t *jpeg_data,
                                       size_t jpeg_size, VideoFrame *frame) {
  if (!ensure_dht(in, jpeg_data, jpeg_size, &jpeg_data, &jpeg_size)) {
    printf("v4l2_in: out of memory augmenting a DHT-less JPEG frame\n");
    return DECODE_TRANSIENT_FAIL;
  }

  int jw, jh, jsubsamp, jcolorspace;
  if (tjDecompressHeader3(in->jpeg_decoder, jpeg_data, jpeg_size, &jw, &jh,
                          &jsubsamp, &jcolorspace) < 0) {
    printf("v4l2_in: tjDecompressHeader3 failed: %s\n",
           tjGetErrorStr2(in->jpeg_decoder));
    return DECODE_TRANSIENT_FAIL;
  }

  // Compared against the *capture* geometry, not the frame's: with
  // downscale > 1 the frame is deliberately smaller than what the camera
  // sends, so frame->width would be the wrong yardstick for "did the
  // camera change format on us".
  if ((uint32_t)jw != in->width || (uint32_t)jh != in->height) {
    printf("v4l2_in: frame dimensions changed mid-stream: got %dx%d, "
           "expected %ux%u\n",
           jw, jh, in->width, in->height);
    return DECODE_FORMAT_MISMATCH;
  }
  if (jsubsamp != TJSAMP_422) {
    printf("v4l2_in: camera's JPEG stream is not 4:2:2 subsampled "
           "(libjpeg-turbo reports subsamp=%d); this capture path requires "
           "4:2:2 to match VideoFrame's I422 layout -- see the note in "
           "v4l2_in.h\n",
           jsubsamp);
    return DECODE_FORMAT_MISMATCH;
  }

  // Decodes directly into the frame's own raw planes -- no RGB conversion,
  // no intermediate buffer. This is the entire reason for requiring 4:2:2:
  // libjpeg-turbo's plane layout for a 4:2:2 source matches VideoFrame's
  // I422 layout exactly, so there's nothing to convert.
  //
  // When downscale > 1, frame->width/height are already the divided values
  // and libjpeg-turbo scales during decompression to hit them -- it does
  // this by discarding high-frequency DCT coefficients rather than by
  // decoding fully and resampling, so a half-size decode is meaningfully
  // cheaper than a full one, not just cheaper downstream. config.c
  // restricts downscale to 1/2/4/8 precisely so the ratio is always one
  // libjpeg-turbo supports exactly: asked for an unsupported ratio it
  // quietly returns the largest supported size that *fits*, which would
  // leave a smaller image sitting in a larger frame with stale bytes
  // around it rather than failing.
  uint8_t *planes[3] = {frame->raw_planes[0], frame->raw_planes[1],
                        frame->raw_planes[2]};
  int strides[3] = {(int)frame->stride[0], (int)frame->stride[1],
                    (int)frame->stride[2]};

  if (tjDecompressToYUVPlanes(in->jpeg_decoder, jpeg_data, jpeg_size, planes,
                              (int)frame->width, strides, (int)frame->height,
                              0) < 0) {
    printf("v4l2_in: tjDecompressToYUVPlanes failed: %s\n",
           tjGetErrorStr2(in->jpeg_decoder));
    return DECODE_TRANSIENT_FAIL;
  }

  return DECODE_OK;
}

// YUYV is packed 4:2:2: every 2 horizontal pixels are 4 bytes, Y0 U0 Y1 V0
// -- two luma samples sharing one chroma pair, which is exactly I422's
// subsampling, just interleaved instead of planar. No decode needed, only
// a deinterleave: split those 4 bytes into VideoFrame's three separate
// planes.
// Box-averages each NxN block of a packed YUYV frame straight into the
// frame's planar I422 buffers, so downscaling costs one pass rather than a
// deinterleave followed by a separate resample.
//
// Kept entirely separate from the N == 1 path below rather than
// generalizing that loop: N == 1 is both the common case and the one whose
// NEON kernel has been verified bit-exact under qemu, and folding a
// runtime divisor into it would have meant re-verifying that work to buy
// nothing (a box average with N == 1 is just a copy).
//
// Chroma indexing is the one part that isn't a direct translation of the
// luma loop. Output chroma sample cx covers output pixels 2*cx and 2*cx+1,
// hence source pixels [2*cx*N, 2*cx*N + 2N), which is exactly the N
// four-byte YUYV groups [cx*N, cx*N + N) -- each group carrying one U (at
// byte 1) and one V (at byte 3). So U and V average N*N samples apiece,
// the same count as luma, just gathered a group at a time.
static void downscale_yuyv_frame(const uint8_t *yuyv, VideoFrame *frame,
                                 uint32_t cap_width, uint32_t n) {
  const size_t src_stride = (size_t)cap_width * 2;
  const uint32_t half = n * n / 2; // rounding term for the averages below

  uint8_t *y_plane = frame->raw_planes[0];
  uint8_t *u_plane = frame->raw_planes[1];
  uint8_t *v_plane = frame->raw_planes[2];
  const size_t y_stride = frame->stride[0];
  const size_t c_stride = frame->stride[1]; // == stride[2]
  const uint32_t out_w = frame->width;
  const uint32_t chroma_w = out_w / 2; // exact: config.c validated evenness

#pragma omp parallel for
  for (uint32_t oy = 0; oy < frame->height; ++oy) {
    const uint8_t *src_block = yuyv + (size_t)oy * n * src_stride;
    uint8_t *y_row = y_plane + (size_t)oy * y_stride;

    for (uint32_t ox = 0; ox < out_w; ++ox) {
      uint32_t sum = 0;
      for (uint32_t dy = 0; dy < n; ++dy) {
        const uint8_t *r = src_block + (size_t)dy * src_stride;
        for (uint32_t dx = 0; dx < n; ++dx)
          sum += r[(size_t)(ox * n + dx) * 2];
      }
      y_row[ox] = (uint8_t)((sum + half) / (n * n));
    }

    uint8_t *u_row = u_plane + (size_t)oy * c_stride;
    uint8_t *v_row = v_plane + (size_t)oy * c_stride;
    for (uint32_t cx = 0; cx < chroma_w; ++cx) {
      uint32_t u_sum = 0, v_sum = 0;
      for (uint32_t dy = 0; dy < n; ++dy) {
        const uint8_t *r = src_block + (size_t)dy * src_stride;
        for (uint32_t g = 0; g < n; ++g) {
          const uint8_t *grp = r + (size_t)(cx * n + g) * 4;
          u_sum += grp[1];
          v_sum += grp[3];
        }
      }
      u_row[cx] = (uint8_t)((u_sum + half) / (n * n));
      v_row[cx] = (uint8_t)((v_sum + half) / (n * n));
    }
  }
}

static DecodeResult unpack_yuyv_frame(const uint8_t *yuyv, size_t yuyv_size,
                                      VideoFrame *frame, uint32_t cap_width,
                                      uint32_t cap_height, uint32_t downscale) {
  // Sized from the capture geometry, not the frame's: with downscale > 1
  // the driver still hands over full-resolution bytes, and it is only the
  // output that shrinks.
  size_t expected = (size_t)cap_width * cap_height * 2;
  if (yuyv_size < expected) {
    // Unlike a JPEG payload, YUYV has no self-describing length -- a short
    // buffer here means the driver hasn't handed over a full frame yet
    // (or a USB glitch truncated one), not a format problem, so this is
    // worth retrying rather than failing outright.
    printf("v4l2_in: YUYV frame too short: got %zu bytes, expected %zu\n",
           yuyv_size, expected);
    return DECODE_TRANSIENT_FAIL;
  }

  if (downscale > 1) {
    downscale_yuyv_frame(yuyv, frame, cap_width, downscale);
    return DECODE_OK;
  }

  uint8_t *y_plane = frame->raw_planes[0];
  uint8_t *u_plane = frame->raw_planes[1];
  uint8_t *v_plane = frame->raw_planes[2];
  size_t y_stride = frame->stride[0];
  size_t c_stride = frame->stride[1]; // == stride[2]

  for (uint32_t row = 0; row < frame->height; ++row) {
    const uint8_t *src = yuyv + (size_t)row * frame->width * 2;
    uint8_t *y_row = y_plane + (size_t)row * y_stride;
    uint8_t *u_row = u_plane + (size_t)row * c_stride;
    uint8_t *v_row = v_plane + (size_t)row * c_stride;

    uint32_t x = 0, cx = 0;
#if defined(GS_NEON_AARCH64) || defined(GS_NEON_ARM32)
    // YUYV's 4-byte groups (Y0 U0 Y1 V0) are exactly a 4-channel
    // interleaved format, which vld4q_u8 exists to de-interleave: one
    // 64-byte load yields 16 lanes each of Y0, U, Y1, V. U and V land
    // already in planar order, one store each. Y needs Y0/Y1 re-interleaved
    // back into a single row (Y0[0],Y1[0],Y0[1],Y1[1],...) -- vst2q_u8
    // does exactly that interleave on the way out, so no manual zip is
    // needed. Each chunk covers 32 luma pixels (16 pixel pairs) from 64
    // source bytes.
    //
    // Verified bit-exact against the scalar loop below over pseudo-random
    // frame content at widths both divisible and not divisible by 32 (so
    // the scalar remainder tail is also exercised), cross-compiled for
    // aarch64 and run under qemu-user emulation; this file has not been
    // built or run on real hardware.
    for (; x + 31 < frame->width; x += 32, cx += 16) {
      uint8x16x4_t g = vld4q_u8(&src[x * 2]);
      vst1q_u8(&u_row[cx], g.val[1]);
      vst1q_u8(&v_row[cx], g.val[3]);
      uint8x16x2_t y_pair = {{g.val[0], g.val[2]}};
      vst2q_u8(&y_row[x], y_pair);
    }
#endif
    for (; x + 1 < frame->width; x += 2, ++cx) {
      y_row[x] = src[x * 2];
      u_row[cx] = src[x * 2 + 1];
      y_row[x + 1] = src[x * 2 + 2];
      v_row[cx] = src[x * 2 + 3];
    }
    // An odd width leaves one trailing luma sample with no paired chroma
    // update of its own -- vf_create's chroma_width = (width+1)/2 already
    // reserves a slot for it, just copy the luma and leave that slot as
    // whatever the previous pair wrote.
    if (x < frame->width) {
      y_row[x] = src[x * 2];
    }
  }

  return DECODE_OK;
}

bool v4l2_in_capture(V4l2In *in, VideoFrame *frame) {
  // A handful of retries absorbs an isolated corrupt USB transfer (real,
  // if uncommon, on actual hardware) without surfacing it as a pipeline
  // failure. A format mismatch is different -- retrying can't fix it, so
  // that path returns immediately below instead of burning through these.
  for (int attempt = 0; attempt < 5; attempt++) {
    struct pollfd pfd = {.fd = in->fd, .events = POLLIN, .revents = 0};
    int pr = poll(&pfd, 1, 2000);
    if (pr < 0) {
      if (errno == EINTR)
        continue;
      printf("v4l2_in: poll failed: %s\n", strerror(errno));
      return false;
    }
    if (pr == 0) {
      printf("v4l2_in: timed out waiting for a frame (attempt %d/5)\n",
             attempt + 1);
      continue;
    }

    struct v4l2_buffer buf = {0};
    buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    buf.memory = V4L2_MEMORY_MMAP;

    if (xioctl(in->fd, VIDIOC_DQBUF, &buf) < 0) {
      if (errno == EAGAIN)
        continue;
      printf("v4l2_in: VIDIOC_DQBUF failed: %s\n", strerror(errno));
      return false;
    }

    const uint8_t *data = (const uint8_t *)in->buffers[buf.index].start;
    DecodeResult result = (in->capture_format == V4L2_PIX_FMT_MJPEG)
                             ? decode_mjpeg_frame(in, data, buf.bytesused, frame)
                             : unpack_yuyv_frame(data, buf.bytesused, frame,
                                                 in->width, in->height,
                                                 in->downscale);

    // The buffer goes back to the driver regardless of decode outcome --
    // skip this and streaming stalls silently once every buffer has been
    // dequeued and none returned.
    if (xioctl(in->fd, VIDIOC_QBUF, &buf) < 0) {
      printf("v4l2_in: VIDIOC_QBUF failed: %s\n", strerror(errno));
      return false;
    }

    if (result == DECODE_OK)
      return true;
    if (result == DECODE_FORMAT_MISMATCH)
      return false; // not transient; see decode_mjpeg_frame's messages above

    printf("v4l2_in: dropped a frame (attempt %d/5)\n", attempt + 1);
  }

  printf("v4l2_in: too many consecutive capture failures; giving up\n");
  return false;
}

void v4l2_in_close(V4l2In *in) {
  if (!in)
    return;

  if (in->fd >= 0) {
    enum v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    xioctl(in->fd, VIDIOC_STREAMOFF, &type);
  }

  for (uint32_t i = 0; i < in->n_buffers; i++) {
    if (in->buffers[i].start && in->buffers[i].start != MAP_FAILED)
      munmap(in->buffers[i].start, in->buffers[i].length);
  }

  if (in->jpeg_decoder)
    tjDestroy(in->jpeg_decoder);

  free(in->dht_scratch);

  if (in->fd >= 0)
    close(in->fd);

  free(in);
}

// Attempts VIDIOC_S_FMT for one pixel format at exactly width x height,
// logging why on failure -- useful on its own merits (distinguishing "the
// driver flatly rejected this format" from "it granted a different
// resolution instead" matters when debugging an unfamiliar camera), and
// specifically because v4l2_in_open's final fallback-exhausted message
// doesn't repeat these details itself.
//
// Unlike v4l2_out.c's equivalent, a dimension mismatch here has to be
// treated the same as an outright failure: the frame pool is sized from
// width/height, and any capture format that doesn't land on them exactly
// dooms every subsequent frame (see v4l2_in_open's other caller-facing
// mismatch handling for the full reasoning).
static bool try_set_format(int fd, uint32_t fourcc, uint32_t width,
                          uint32_t height, struct v4l2_format *fmt_out) {
  struct v4l2_format fmt = {0};
  fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
  fmt.fmt.pix.width = width;
  fmt.fmt.pix.height = height;
  fmt.fmt.pix.pixelformat = fourcc;
  fmt.fmt.pix.field = V4L2_FIELD_NONE;

  if (xioctl(fd, VIDIOC_S_FMT, &fmt) < 0) {
    printf("v4l2_in: %.4s not accepted at %ux%u: %s\n", (const char *)&fourcc,
           width, height, strerror(errno));
    return false;
  }
  if (fmt.fmt.pix.pixelformat != fourcc) {
    printf("v4l2_in: %.4s was not granted (driver substituted a different "
           "pixel format)\n",
           (const char *)&fourcc);
    return false;
  }
  if (fmt.fmt.pix.width != width || fmt.fmt.pix.height != height) {
    printf("v4l2_in: %.4s at %ux%u got %ux%u instead\n", (const char *)&fourcc,
           width, height, fmt.fmt.pix.width, fmt.fmt.pix.height);
    return false;
  }

  *fmt_out = fmt;
  return true;
}

V4l2In *v4l2_in_open(const char *path, uint32_t width, uint32_t height,
                     uint32_t framerate_hint, uint32_t downscale) {
  int fd = open(path, O_RDWR | O_NONBLOCK);
  if (fd < 0) {
    switch (errno) {
    case ENOENT:
      printf("No such device: %s\n", path);
      break;
    case EACCES:
      printf("Permission denied opening %s\n"
             "Add your user to the 'video' group and log back in:\n"
             "  sudo usermod -aG video $USER\n",
             path);
      break;
    default:
      printf("Failed to open %s: %s\n", path, strerror(errno));
      break;
    }
    return NULL;
  }

  struct v4l2_capability cap = {0};
  if (xioctl(fd, VIDIOC_QUERYCAP, &cap) < 0) {
    printf("%s is not a V4L2 device (VIDIOC_QUERYCAP: %s)\n", path,
           strerror(errno));
    close(fd);
    return NULL;
  }

  // device_caps describes this specific node; capabilities is the union
  // across every node the driver owns. Same distinction as v4l2_out.c.
  uint32_t caps = (cap.capabilities & V4L2_CAP_DEVICE_CAPS) ? cap.device_caps
                                                            : cap.capabilities;
  if (!(caps & V4L2_CAP_VIDEO_CAPTURE)) {
    printf("%s does not support video capture (caps 0x%08x)\n", path, caps);
    close(fd);
    return NULL;
  }
  if (!(caps & V4L2_CAP_STREAMING)) {
    printf("%s does not support streaming I/O (caps 0x%08x); MMAP capture "
           "needs it\n",
           path, caps);
    close(fd);
    return NULL;
  }

  // MJPEG first: at USB2's ~24.6 MB/s isochronous ceiling, most webcams
  // only offer higher resolutions/framerates through it, reserving raw
  // YUYV for lower modes (see the framerate/bandwidth discussion this
  // capture path was designed around). YUYV needs no decode at all, so
  // it's still preferred over MJPEG at whatever resolution both are
  // actually available -- try_set_format's exact-match requirement means
  // this only ever falls through to YUYV when MJPEG genuinely isn't
  // offered at this resolution, not as a blanket preference.
  struct v4l2_format fmt;
  uint32_t capture_format;

  if (try_set_format(fd, V4L2_PIX_FMT_MJPEG, width, height, &fmt)) {
    capture_format = V4L2_PIX_FMT_MJPEG;
  } else if (try_set_format(fd, V4L2_PIX_FMT_YUYV, width, height, &fmt)) {
    capture_format = V4L2_PIX_FMT_YUYV;
    printf("%s: MJPEG not available at exactly %ux%u; falling back to "
           "YUYV\n",
           path, width, height);
  } else {
    // try_set_format() already logged why each attempt failed.
    printf("%s: neither MJPEG nor YUYV is available at exactly %ux%u\n",
           path, width, height);
    close(fd);
    return NULL;
  }

  // Best-effort: V4L2 does not require a driver to honor an exact
  // framerate, so a failure here is not treated as fatal.
  struct v4l2_streamparm parm = {0};
  parm.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
  parm.parm.capture.timeperframe.numerator = 1;
  parm.parm.capture.timeperframe.denominator = framerate_hint;
  xioctl(fd, VIDIOC_S_PARM, &parm);

  struct v4l2_requestbuffers req = {0};
  req.count = 4;
  req.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
  req.memory = V4L2_MEMORY_MMAP;

  if (xioctl(fd, VIDIOC_REQBUFS, &req) < 0) {
    printf("Failed to request capture buffers on %s: %s\n", path,
           strerror(errno));
    close(fd);
    return NULL;
  }
  if (req.count < 2) {
    printf("%s only granted %u capture buffer(s); need at least 2 for "
           "streaming\n",
           path, req.count);
    close(fd);
    return NULL;
  }
  if (req.count > v4l2_in_max_buffers) {
    printf("%s granted %u capture buffers, more than the %u this code "
           "supports\n",
           path, req.count, v4l2_in_max_buffers);
    close(fd);
    return NULL;
  }

  V4l2In *in = (V4l2In *)calloc(1, sizeof(V4l2In));
  if (!in) {
    close(fd);
    return NULL;
  }
  in->fd = fd;
  in->width = fmt.fmt.pix.width;
  in->height = fmt.fmt.pix.height;
  in->downscale = downscale ? downscale : 1;
  in->n_buffers = req.count;
  in->capture_format = capture_format;

  for (uint32_t i = 0; i < in->n_buffers; i++) {
    struct v4l2_buffer buf = {0};
    buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    buf.memory = V4L2_MEMORY_MMAP;
    buf.index = i;

    if (xioctl(fd, VIDIOC_QUERYBUF, &buf) < 0) {
      printf("VIDIOC_QUERYBUF failed for buffer %u: %s\n", i,
             strerror(errno));
      v4l2_in_close(in);
      return NULL;
    }

    void *start = mmap(NULL, buf.length, PROT_READ | PROT_WRITE, MAP_SHARED,
                       fd, buf.m.offset);
    if (start == MAP_FAILED) {
      printf("mmap failed for buffer %u: %s\n", i, strerror(errno));
      v4l2_in_close(in);
      return NULL;
    }
    in->buffers[i].start = start;
    in->buffers[i].length = buf.length;

    if (xioctl(fd, VIDIOC_QBUF, &buf) < 0) {
      printf("VIDIOC_QBUF failed for buffer %u: %s\n", i, strerror(errno));
      v4l2_in_close(in);
      return NULL;
    }
  }

  enum v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
  if (xioctl(fd, VIDIOC_STREAMON, &type) < 0) {
    printf("VIDIOC_STREAMON failed on %s: %s\n", path, strerror(errno));
    v4l2_in_close(in);
    return NULL;
  }

  // YUYV needs no decoder at all -- in->jpeg_decoder stays NULL from
  // calloc(), which v4l2_in_close() already checks for before tjDestroy().
  if (in->capture_format == V4L2_PIX_FMT_MJPEG) {
    in->jpeg_decoder = tjInitDecompress();
    if (!in->jpeg_decoder) {
      printf("tjInitDecompress failed: %s\n", tjGetErrorStr());
      v4l2_in_close(in);
      return NULL;
    }
  }

  // Probe: capture and decode one real frame before declaring open()
  // successful. This is what turns "camera's JPEG isn't actually 4:2:2"
  // into one clear failure right here, instead of a confusing per-frame
  // failure loop once producer_loop is already running.
  // Sized like every other frame this will fill -- at the *output*
  // geometry, not the capture geometry -- so the probe exercises the same
  // scaled-decode/box-average path the real frames take. Probing at full
  // size would leave the downscale path itself untested until the first
  // real frame, which is exactly what this probe exists to avoid.
  VideoFrame *probe =
      vf_create(in->width / in->downscale, in->height / in->downscale, 0);
  if (!probe) {
    printf("Failed to allocate probe frame\n");
    v4l2_in_close(in);
    return NULL;
  }
  bool probe_ok = v4l2_in_capture(in, probe);
  vf_free(probe);
  if (!probe_ok) {
    printf("Initial capture probe failed; see the error above\n");
    v4l2_in_close(in);
    return NULL;
  }

  return in;
}
