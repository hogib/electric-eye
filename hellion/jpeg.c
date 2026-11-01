#include "jpeg.h"
#include "turbojpeg_compat.h"
#include <stdio.h>
#include <stdlib.h>

struct JpegEnc {
  tjhandle tj;
  unsigned char *buf;
  unsigned long cap;
  uint8_t *c420; // the 4:2:0 chroma planes, for jpeg_enc_i422_as_420
  size_t c420_cap;
};

JpegEnc *jpeg_enc_create(void) {
  JpegEnc *e = (JpegEnc *)calloc(1, sizeof *e);
  if (!e)
    return NULL;
  e->tj = tjInitCompress();
  if (!e->tj) {
    printf("hellion: tjInitCompress failed: %s\n", tjGetErrorStr());
    free(e);
    return NULL;
  }
  return e;
}

const uint8_t *jpeg_enc_i422(JpegEnc *e, const uint8_t *const planes[3],
                             uint32_t width, uint32_t height, int quality,
                             size_t *size) {
  // Reused across frames rather than letting turbojpeg malloc and free one
  // per call; the size only changes with the stream's.
  unsigned long needed = tjBufSize((int)width, (int)height, TJSAMP_422);
  if (e->cap < needed) {
    if (e->buf)
      tjFree(e->buf);
    e->buf = tjAlloc((int)needed);
    e->cap = e->buf ? needed : 0;
  }
  if (!e->buf)
    return NULL;

  int cw = (int)(width + 1) / 2;
  int strides[3] = {(int)width, cw, cw};
  unsigned long out = 0;
  if (tjCompressFromYUVPlanes(e->tj, TJ_SRC(planes), (int)width,
                              TJ_SRC(strides), (int)height, TJSAMP_422,
                              &e->buf, &out, quality,
                              TJFLAG_FASTDCT | TJFLAG_NOREALLOC) < 0) {
    printf("hellion: JPEG encode failed: %s\n", tjGetErrorStr2(e->tj));
    return NULL;
  }
  *size = (size_t)out;
  return e->buf;
}

const uint8_t *jpeg_enc_i422_as_420(JpegEnc *e,
                                    const uint8_t *const planes[3],
                                    uint32_t width, uint32_t height,
                                    int quality, size_t *size) {
  unsigned long needed = tjBufSize((int)width, (int)height, TJSAMP_420);
  if (e->cap < needed) {
    if (e->buf)
      tjFree(e->buf);
    e->buf = tjAlloc((int)needed);
    e->cap = e->buf ? needed : 0;
  }
  uint32_t cw = (width + 1) / 2, ch = (height + 1) / 2;
  size_t cplane = (size_t)cw * ch;
  if (e->c420_cap < 2 * cplane) {
    free(e->c420);
    e->c420 = (uint8_t *)malloc(2 * cplane);
    e->c420_cap = e->c420 ? 2 * cplane : 0;
  }
  if (!e->buf || !e->c420)
    return NULL;
  for (int p = 0; p < 2; p++) {
    const uint8_t *src = planes[1 + p]; // cw x height
    uint8_t *dst = e->c420 + (size_t)p * cplane;
    for (uint32_t r = 0; r < ch; r++) {
      const uint8_t *a = src + (size_t)(2 * r) * cw;
      const uint8_t *b = 2 * r + 1 < height ? a + cw : a;
      for (uint32_t x = 0; x < cw; x++)
        dst[(size_t)r * cw + x] = (uint8_t)((a[x] + b[x] + 1) >> 1);
    }
  }
  const uint8_t *p420[3] = {planes[0], e->c420, e->c420 + cplane};
  int strides[3] = {(int)width, (int)cw, (int)cw};
  unsigned long out = 0;
  if (tjCompressFromYUVPlanes(e->tj, TJ_SRC(p420), (int)width,
                              TJ_SRC(strides), (int)height, TJSAMP_420,
                              &e->buf, &out, quality,
                              TJFLAG_FASTDCT | TJFLAG_NOREALLOC) < 0) {
    printf("hellion: JPEG encode failed: %s\n", tjGetErrorStr2(e->tj));
    return NULL;
  }
  *size = (size_t)out;
  return e->buf;
}

void jpeg_enc_destroy(JpegEnc *e) {
  if (!e)
    return;
  free(e->c420);
  if (e->buf)
    tjFree(e->buf);
  tjDestroy(e->tj);
  free(e);
}
