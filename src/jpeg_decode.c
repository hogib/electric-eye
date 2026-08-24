#include "jpeg_decode.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <turbojpeg.h>

struct JpegDecoder {
  tjhandle handle;
  // Source geometry: what the encoder sends, before any downscale. Frames
  // are validated against this, not against the (possibly smaller) output
  // frame -- see jpeg_decode.h.
  uint32_t width;
  uint32_t height;

  // Scratch space for a DHT-augmented copy of a frame, grown lazily via
  // realloc() the first time a frame actually needs it (an encoder that
  // already emits DHT, the common case, never touches this).
  uint8_t *dht_scratch;
  size_t dht_scratch_cap;

  // Half-height chroma planes, used only when the source is 4:2:0 (see
  // decode_420_to_i422 below). Allocated on the first 4:2:0 frame and
  // reused thereafter; a 4:2:2 source leaves these NULL forever.
  uint8_t *chroma_scratch;
  size_t chroma_scratch_cap;
};

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

// If `data` has no DHT before its first SOS, copies it into dec->dht_scratch
// (growing that buffer as needed) with a standard-tables DHT segment
// spliced in right after SOI -- always a valid insertion point, since DHT
// may appear anywhere before the SOS that references it. Points *out_data/
// *out_size at whichever buffer decode should actually use: the original,
// unmodified, if it already had a DHT (the common case costs nothing
// beyond the marker scan above), or the augmented scratch copy otherwise.
// Returns false only on allocation failure.
static bool ensure_dht(JpegDecoder *dec, const uint8_t *data, size_t size,
                      const uint8_t **out_data, size_t *out_size) {
  if (size >= 2 && jpeg_has_dht(data, size)) {
    *out_data = data;
    *out_size = size;
    return true;
  }

  size_t needed = size + 420; // exact DHT segment size; see the comment above
  if (dec->dht_scratch_cap < needed) {
    uint8_t *grown = realloc(dec->dht_scratch, needed);
    if (!grown)
      return false;
    dec->dht_scratch = grown;
    dec->dht_scratch_cap = needed;
  }

  memcpy(dec->dht_scratch, data, 2); // SOI
  size_t dht_len = build_standard_dht_segment(dec->dht_scratch + 2);
  memcpy(dec->dht_scratch + 2 + dht_len, data + 2, size - 2);

  *out_data = dec->dht_scratch;
  *out_size = 2 + dht_len + (size - 2);
  return true;
}

// Decodes a 4:2:0 source into VideoFrame's I422 planes.
//
// The two layouts differ in exactly one dimension: 4:2:0 halves chroma both
// horizontally *and* vertically, I422 only horizontally. So the chroma
// planes match in width and differ 2:1 in height, and converting means
// line-doubling each chroma row -- no horizontal work at all.
//
// libjpeg-turbo cannot write this directly: tjDecompressToYUVPlanes always
// emits the source's own subsampling, so a 4:2:0 frame decoded straight
// into an I422 frame's planes would write half as many chroma rows as the
// frame has and leave the bottom half stale. Hence the scratch planes: the
// luma plane still decodes directly into the frame (it is full-resolution
// in both layouts, so there is nothing to fix up and no copy to pay for),
// while chroma lands in scratch and is expanded on the way out.
//
// Nearest-neighbour line doubling rather than interpolating between
// adjacent chroma rows: the source genuinely has no information between
// them, and interpolating would blur chroma edges to buy only smoothness
// that the 4:2:0 encoder already discarded.
static JpegDecodeResult decode_420_to_i422(JpegDecoder *dec,
                                           const uint8_t *data, size_t size,
                                           VideoFrame *frame) {
  const size_t chroma_stride = frame->stride[1];
  const uint32_t chroma_h = frame->height / 2; // 4:2:0: half-height chroma
  const size_t plane_bytes = chroma_stride * chroma_h;

  if (dec->chroma_scratch_cap < plane_bytes * 2) {
    uint8_t *grown = realloc(dec->chroma_scratch, plane_bytes * 2);
    if (!grown) {
      printf("jpeg_decode: out of memory allocating 4:2:0 chroma scratch\n");
      return JPEG_DECODE_TRANSIENT_FAIL;
    }
    dec->chroma_scratch = grown;
    dec->chroma_scratch_cap = plane_bytes * 2;
  }

  uint8_t *planes[3] = {frame->raw_planes[0], dec->chroma_scratch,
                        dec->chroma_scratch + plane_bytes};
  int strides[3] = {(int)frame->stride[0], (int)chroma_stride,
                    (int)chroma_stride};

  if (tjDecompressToYUVPlanes(dec->handle, data, size, planes,
                              (int)frame->width, strides, (int)frame->height,
                              0) < 0) {
    printf("jpeg_decode: tjDecompressToYUVPlanes failed: %s\n",
           tjGetErrorStr2(dec->handle));
    return JPEG_DECODE_TRANSIENT_FAIL;
  }

  // Each source chroma row covers two output rows. Walked source-row-major
  // so each row is read once and written twice, rather than reading it
  // twice as a destination-major loop would.
  for (uint32_t p = 1; p <= 2; ++p) {
    const uint8_t *src = dec->chroma_scratch + (p - 1) * plane_bytes;
    uint8_t *dst = frame->raw_planes[p];
    for (uint32_t sy = 0; sy < chroma_h; ++sy) {
      const uint8_t *src_row = src + (size_t)sy * chroma_stride;
      memcpy(dst + (size_t)(sy * 2) * chroma_stride, src_row, chroma_stride);
      memcpy(dst + (size_t)(sy * 2 + 1) * chroma_stride, src_row,
             chroma_stride);
    }
  }

  return JPEG_DECODE_OK;
}

JpegDecodeResult jpeg_decode_frame(JpegDecoder *dec, const uint8_t *data,
                                   size_t size, VideoFrame *frame) {
  if (!ensure_dht(dec, data, size, &data, &size)) {
    printf("jpeg_decode: out of memory augmenting a DHT-less frame\n");
    return JPEG_DECODE_TRANSIENT_FAIL;
  }

  int jw, jh, jsubsamp, jcolorspace;
  if (tjDecompressHeader3(dec->handle, data, size, &jw, &jh, &jsubsamp,
                          &jcolorspace) < 0) {
    printf("jpeg_decode: tjDecompressHeader3 failed: %s\n",
           tjGetErrorStr2(dec->handle));
    return JPEG_DECODE_TRANSIENT_FAIL;
  }

  // Compared against the *source* geometry, not the frame's: with
  // downscale > 1 the frame is deliberately smaller than what the encoder
  // sends, so frame->width would be the wrong yardstick for "did the
  // source change format on us".
  if ((uint32_t)jw != dec->width || (uint32_t)jh != dec->height) {
    printf("jpeg_decode: frame dimensions changed mid-stream: got %dx%d, "
           "expected %ux%u\n",
           jw, jh, dec->width, dec->height);
    return JPEG_DECODE_FORMAT_MISMATCH;
  }

  if (jsubsamp == TJSAMP_420)
    return decode_420_to_i422(dec, data, size, frame);

  if (jsubsamp != TJSAMP_422) {
    printf("jpeg_decode: source is neither 4:2:2 nor 4:2:0 subsampled "
           "(libjpeg-turbo reports subsamp=%d); this path decodes into "
           "VideoFrame's I422 layout and supports only those two -- see "
           "jpeg_decode.h\n",
           jsubsamp);
    return JPEG_DECODE_FORMAT_MISMATCH;
  }

  // 4:2:2: libjpeg-turbo's plane layout for this source matches
  // VideoFrame's I422 exactly, so it decodes straight into the frame's own
  // raw planes with nothing to convert and no intermediate buffer.
  //
  // When the frame is smaller than the source, libjpeg-turbo scales during
  // decompression to hit it. config.c restricts downscale to 1/2/4/8
  // precisely so the ratio is always one libjpeg-turbo supports exactly:
  // asked for an unsupported ratio it quietly returns the largest
  // supported size that *fits*, which would leave a smaller image sitting
  // in a larger frame with stale bytes around it rather than failing.
  uint8_t *planes[3] = {frame->raw_planes[0], frame->raw_planes[1],
                        frame->raw_planes[2]};
  int strides[3] = {(int)frame->stride[0], (int)frame->stride[1],
                    (int)frame->stride[2]};

  if (tjDecompressToYUVPlanes(dec->handle, data, size, planes,
                              (int)frame->width, strides, (int)frame->height,
                              0) < 0) {
    printf("jpeg_decode: tjDecompressToYUVPlanes failed: %s\n",
           tjGetErrorStr2(dec->handle));
    return JPEG_DECODE_TRANSIENT_FAIL;
  }

  return JPEG_DECODE_OK;
}

JpegDecoder *jpeg_decoder_create(uint32_t width, uint32_t height) {
  JpegDecoder *dec = (JpegDecoder *)calloc(1, sizeof(JpegDecoder));
  if (!dec)
    return NULL;

  dec->handle = tjInitDecompress();
  if (!dec->handle) {
    printf("jpeg_decode: tjInitDecompress failed: %s\n", tjGetErrorStr());
    free(dec);
    return NULL;
  }
  dec->width = width;
  dec->height = height;
  return dec;
}

void jpeg_decoder_destroy(JpegDecoder *dec) {
  if (!dec)
    return;
  if (dec->handle)
    tjDestroy(dec->handle);
  free(dec->dht_scratch);
  free(dec->chroma_scratch);
  free(dec);
}
