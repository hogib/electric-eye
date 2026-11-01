#include "img_stats.h"
#include <math.h>
#include <string.h>

// Chroma is only trusted between these luma levels (see ImgStats.cast_u).
enum { chroma_min_luma = 24, chroma_max_luma = 235 };

void img_stats_compute(const uint8_t *const planes[3], uint32_t width,
                       uint32_t height, const size_t stride[3],
                       ImgStats *out) {
  memset(out, 0, sizeof *out);
  int64_t sum_u = 0, sum_v = 0, sum_y = 0;
  uint32_t n_chroma = 0;
  // I422: chroma is half width, full height, so luma column x pairs with
  // chroma column x/2. Even x only, so every sample has its own chroma.
  for (uint32_t y = 0; y < height; y += 2) {
    const uint8_t *yr = planes[0] + (size_t)y * stride[0];
    const uint8_t *ur = planes[1] + (size_t)y * stride[1];
    const uint8_t *vr = planes[2] + (size_t)y * stride[2];
    for (uint32_t x = 0; x < width; x += 2) {
      uint8_t l = yr[x];
      out->hist[l]++;
      if (l >= chroma_min_luma && l <= chroma_max_luma) {
        sum_u += (int)ur[x / 2] - 128;
        sum_v += (int)vr[x / 2] - 128;
        sum_y += l;
        n_chroma++;
      }
    }
  }
  out->samples = ((height + 1) / 2) * ((width + 1) / 2);
  out->chroma_samples = n_chroma;
  if (n_chroma) {
    out->cast_u = (float)sum_u / (float)n_chroma;
    out->cast_v = (float)sum_v / (float)n_chroma;
    out->mean_luma = (float)sum_y / (float)n_chroma;
  }
}

uint8_t img_stats_percentile(const ImgStats *s, double fraction) {
  if (s->samples == 0)
    return 0;
  double want = fraction * (double)s->samples;
  uint64_t cum = 0;
  for (int v = 0; v < 256; v++) {
    cum += s->hist[v];
    if ((double)cum >= want)
      return (uint8_t)v;
  }
  return 255;
}

static int8_t clamp_i8(float v) {
  long r = lroundf(v);
  return (int8_t)(r < -127 ? -127 : r > 127 ? 127 : r);
}

void img_stats_summarize(const ImgStats *s, FrameStats *out) {
  memset(out, 0, sizeof *out);
  if (s->samples == 0)
    return;
  out->valid = true;
  uint64_t dark = 0, clip = 0;
  for (int b = 0; b < 32; b++) {
    uint64_t sum = 0;
    for (int v = b * 8; v < b * 8 + 8; v++)
      sum += s->hist[v];
    out->hist32[b] = (uint16_t)((sum * 1000 + s->samples / 2) / s->samples);
  }
  for (int v = 0; v < img_dark_level; v++)
    dark += s->hist[v];
  for (int v = img_clip_level; v < 256; v++)
    clip += s->hist[v];
  out->dark_pm = (uint16_t)((dark * 1000 + s->samples / 2) / s->samples);
  out->clip_pm = (uint16_t)((clip * 1000 + s->samples / 2) / s->samples);
  out->p1 = img_stats_percentile(s, 0.01);
  out->p50 = img_stats_percentile(s, 0.50);
  out->p99 = img_stats_percentile(s, 0.99);
  out->cast_u = clamp_i8(s->cast_u);
  out->cast_v = clamp_i8(s->cast_v);
}
