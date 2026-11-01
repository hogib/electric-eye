#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/*
 * What the camera's picture looks like, measured once per frame on the
 * untouched (raw) planes: the brightness histogram and the colour cast.
 *
 * Two consumers. auto_vis.c steers its corrections from the full ImgStats;
 * the operator gets the small FrameStats summary through the health file,
 * because "too dark", "highlights clipped" and "everything is green" are
 * things a pilot cannot judge reliably from a small preview over a tether,
 * and they say whether the fix belongs to the camera (exposure) or to the
 * picture (auto visibility).
 *
 * Sampled, not exhaustive: every second pixel of every second row, which at
 * 720p is ~230,000 samples -- far more than a histogram needs, at a quarter
 * of the reads.
 */

// Luma below this counts as crushed black, at or above clip_level as
// blown-out highlights (full-range Y, 0-255).
enum { img_dark_level = 16, img_clip_level = 250 };

typedef struct {
  uint32_t hist[256]; // luma, sampled
  uint32_t samples;
  // Mean chroma offset from neutral (U-128, V-128) over well-exposed
  // samples only: near-black pixels have noise for colour and clipped ones
  // have lost theirs, and both would bias the cast estimate.
  float cast_u;
  float cast_v;
  // Mean luma of those same samples (what the cast was measured at).
  float mean_luma;
  uint32_t chroma_samples;
} ImgStats;

// The operator-facing summary. Small and fixed-size so it can ride along
// in every VideoFrame from the effects thread to the consumer, which
// publishes it.
typedef struct {
  bool valid;
  uint16_t hist32[32]; // luma histogram, 32 bins, permille of samples
  uint8_t p1, p50, p99; // luma percentiles
  uint16_t dark_pm;     // permille of samples below img_dark_level
  uint16_t clip_pm;     // permille at or above img_clip_level
  int8_t cast_u, cast_v; // mean chroma offset from neutral, rounded
  bool auto_on;          // auto visibility processed this frame
} FrameStats;

void img_stats_compute(const uint8_t *const planes[3], uint32_t width,
                       uint32_t height, const size_t stride[3],
                       ImgStats *out);

// Smallest luma value v with at least `fraction` of samples at or below it.
uint8_t img_stats_percentile(const ImgStats *s, double fraction);

void img_stats_summarize(const ImgStats *s, FrameStats *out);
