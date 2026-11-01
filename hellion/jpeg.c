#include "jpeg.h"
#include "turbojpeg_compat.h"
#include <stdio.h>
#include <stdlib.h>

struct JpegEnc {
  tjhandle tj;
  unsigned char *buf;
  unsigned long cap;
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

void jpeg_enc_destroy(JpegEnc *e) {
  if (!e)
    return;
  if (e->buf)
    tjFree(e->buf);
  tjDestroy(e->tj);
  free(e);
}
