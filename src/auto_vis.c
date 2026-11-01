#include "auto_vis.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>

// --- Tuning ----------------------------------------------------------------
//
// Starting points, chosen on synthetic underwater frames (see
// tests/test_auto_vis.c) and a webcam; to be re-tuned on real footage from
// the vehicle. Each is the one place its behaviour is decided.

// Smoothing time constant, seconds of capture time.
static const double smooth_tau_s = 0.4;
// A gap longer than this restarts from the current frame (no fade-in).
static const double restart_gap_s = 1.0;

// The measured cast (chroma units, as a vector in U/V) is removed down to
// this much, never below: a scene's own colours average out to a small
// cast too -- the synthetic test scene's red, yellow and orange patches give
// ~10, and removing that turned its grey background blue -- while water
// gives several times that (~40 in the test's model). Also what keeps open
// water, which really is mostly blue, from being pushed to grey and past it.
static const double cast_deadband = 8.0;
static const double max_cast_shift = 48.0; // chroma units
// Chroma gain after the cast is removed.
static const double saturation = 1.2;

// Brightness stretch: the percentiles mapped to black and white.
static const double stretch_lo_pct = 0.005, stretch_hi_pct = 0.995;
// The stretched range is never narrower than this many input levels, which
// caps the stretch at 255/min_range (~2.7x): beyond that it is mostly
// amplifying sensor noise. With the clip limit below, a flat, dark, noisy
// frame (open water at depth) comes out with ~3.9x its noise; 64 and 2.5
// gave 5.5x for a barely sharper picture (tests/test_auto_vis.c prints it).
static const double min_range = 96.0;
// Where the median is placed, and how far gamma may move to get it there.
static const double target_median = 0.45;
static const double gamma_min = 0.55, gamma_max = 1.6;

// CLAHE: tile size in pixels (approximate; the frame is divided evenly),
// clip limit in multiples of a flat histogram's bin height, and how much of
// the local result is blended over the globally corrected picture.
static const uint32_t tile_px_x = 160, tile_px_y = 120;
static const uint32_t max_tiles_x = 8, max_tiles_y = 6;
static const double clip_limit = 2.0;
static const double local_strength = 0.7;

// --- State -------------------------------------------------------------------

struct AutoVis {
  bool primed;
  uint64_t last_ns;
  // Smoothed global parameters.
  double lo, hi, gamma, du, dv;

  // CLAHE working state, sized for the current frame.
  uint32_t w, h, tx, ty;
  uint32_t *tile_hist; // tx*ty*256
  float *tile_lut_f;   // tx*ty*256, smoothed
  uint8_t *tile_lut;   // tx*ty*256
  bool tiles_primed;
  // Bilinear interpolation between tile centres: for each column, the left
  // tile and the right tile's weight (0-256); likewise per row.
  uint16_t *col_t, *col_w, *row_t, *row_w;
};

AutoVis *auto_vis_create(void) { return calloc(1, sizeof(AutoVis)); }

static void free_tiles(AutoVis *av) {
  free(av->tile_hist);
  free(av->tile_lut_f);
  free(av->tile_lut);
  free(av->col_t);
  free(av->col_w);
  free(av->row_t);
  free(av->row_w);
  av->tile_hist = NULL;
  av->tile_lut_f = NULL;
  av->tile_lut = NULL;
  av->col_t = av->col_w = av->row_t = av->row_w = NULL;
  av->w = av->h = 0;
}

void auto_vis_destroy(AutoVis *av) {
  if (!av)
    return;
  free_tiles(av);
  free(av);
}

static uint32_t clamp_u32(uint32_t v, uint32_t lo, uint32_t hi) {
  return v < lo ? lo : v > hi ? hi : v;
}

// Interpolation tables along one axis of `n` pixels split into `tiles`.
static void axis_tables(uint32_t n, uint32_t tiles, uint16_t *t, uint16_t *w) {
  double tile = (double)n / tiles;
  for (uint32_t i = 0; i < n; i++) {
    // Position relative to tile centres: centre k is at (k + 0.5) * tile.
    double pos = ((double)i + 0.5) / tile - 0.5;
    if (pos <= 0) {
      t[i] = 0;
      w[i] = 0;
    } else if (pos >= tiles - 1) {
      t[i] = (uint16_t)(tiles - 1);
      w[i] = 0;
    } else {
      uint32_t k = (uint32_t)pos;
      t[i] = (uint16_t)k;
      w[i] = (uint16_t)lround((pos - k) * 256.0);
    }
  }
}

static bool ensure_tiles(AutoVis *av, uint32_t width, uint32_t height) {
  if (av->w == width && av->h == height && av->tile_hist)
    return true;
  free_tiles(av);
  uint32_t tx = clamp_u32((width + tile_px_x / 2) / tile_px_x, 1, max_tiles_x);
  uint32_t ty = clamp_u32((height + tile_px_y / 2) / tile_px_y, 1, max_tiles_y);
  size_t n = (size_t)tx * ty * 256;
  av->tile_hist = malloc(n * sizeof *av->tile_hist);
  av->tile_lut_f = malloc(n * sizeof *av->tile_lut_f);
  av->tile_lut = malloc(n);
  av->col_t = malloc(width * sizeof *av->col_t);
  av->col_w = malloc(width * sizeof *av->col_w);
  av->row_t = malloc(height * sizeof *av->row_t);
  av->row_w = malloc(height * sizeof *av->row_w);
  if (!av->tile_hist || !av->tile_lut_f || !av->tile_lut || !av->col_t ||
      !av->col_w || !av->row_t || !av->row_w) {
    free_tiles(av);
    return false;
  }
  av->w = width;
  av->h = height;
  av->tx = tx;
  av->ty = ty;
  av->tiles_primed = false;
  axis_tables(width, tx, av->col_t, av->col_w);
  axis_tables(height, ty, av->row_t, av->row_w);
  return true;
}

static double clampd(double v, double lo, double hi) {
  return v < lo ? lo : v > hi ? hi : v;
}

static uint8_t clamp_u8(int v) { return (uint8_t)(v < 0 ? 0 : v > 255 ? 255 : v); }

// --- The three corrections ---------------------------------------------------

typedef struct {
  double lo, hi, gamma, du, dv;
} Targets;

static Targets targets_for(const ImgStats *s) {
  Targets t;
  double lo = img_stats_percentile(s, stretch_lo_pct);
  double hi = img_stats_percentile(s, stretch_hi_pct);
  if (hi - lo < min_range) {
    double mid = (lo + hi) / 2;
    lo = mid - min_range / 2;
    hi = mid + min_range / 2;
    if (lo < 0) {
      hi -= lo;
      lo = 0;
    }
    if (hi > 255) {
      lo -= hi - 255;
      hi = 255;
    }
  }
  t.lo = lo;
  t.hi = hi;

  double med = (img_stats_percentile(s, 0.5) - lo) / (hi - lo);
  med = clampd(med, 0.02, 0.98);
  t.gamma = clampd(log(target_median) / log(med), gamma_min, gamma_max);

  // Too few trustworthy pixels to say what colour the scene is: leave it.
  bool enough = s->chroma_samples >= s->samples / 50 && s->chroma_samples > 0;
  double mag = hypot(s->cast_u, s->cast_v);
  double removed = enough && mag > cast_deadband ? (mag - cast_deadband) / mag : 0;
  t.du = clampd(-removed * s->cast_u, -max_cast_shift, max_cast_shift);
  t.dv = clampd(-removed * s->cast_v, -max_cast_shift, max_cast_shift);
  return t;
}

// Each tile's histogram of the source luma (read only). The histogram of
// the globally corrected picture is this one pushed through the global LUT
// (see build_tile_luts), so nothing has to be written to get it.
static void luma_global(AutoVis *av, const uint8_t *src, size_t stride) {
  const uint32_t tx = av->tx, ty = av->ty, w = av->w, h = av->h;
  uint32_t *const hists = av->tile_hist;
#pragma omp for schedule(static)
  for (uint32_t i = 0; i < tx * ty; i++) {
    uint32_t band = i / tx, t = i % tx;
    uint32_t y0 = (uint32_t)((uint64_t)h * band / ty);
    uint32_t y1 = (uint32_t)((uint64_t)h * (band + 1) / ty);
    uint32_t x0 = (uint32_t)((uint64_t)w * t / tx);
    uint32_t x1 = (uint32_t)((uint64_t)w * (t + 1) / tx);
    // Four interleaved counters, so runs of one level (flat water) do not
    // serialise on one counter's load-increment-store.
    uint32_t hh[4][256];
    memset(hh, 0, sizeof hh);
    for (uint32_t y = y0; y < y1; y++) {
      const uint8_t *s = src + (size_t)y * stride;
      uint32_t x = x0;
      for (; x + 4 <= x1; x += 4) {
        hh[0][s[x]]++;
        hh[1][s[x + 1]]++;
        hh[2][s[x + 2]]++;
        hh[3][s[x + 3]]++;
      }
      for (; x < x1; x++)
        hh[0][s[x]]++;
    }
    uint32_t *hist = hists + (size_t)i * 256;
    for (int v = 0; v < 256; v++)
      hist[v] = hh[0][v] + hh[1][v] + hh[2][v] + hh[3][v];
  }
}

// Each tile's clipped-histogram mapping, smoothed over time, blended with
// the identity by local_strength.
static void build_tile_luts(AutoVis *av, double alpha, const uint8_t lut[256]) {
  const size_t tiles = (size_t)av->tx * av->ty;
  const bool primed = av->tiles_primed;
#pragma omp for schedule(static)
  for (size_t t = 0; t < tiles; t++) {
    // The corrected picture's histogram: the source's through lut.
    uint32_t hist[256];
    {
      const uint32_t *sh = av->tile_hist + t * 256;
      memset(hist, 0, sizeof hist);
      for (int v = 0; v < 256; v++)
        hist[lut[v]] += sh[v];
    }
    uint64_t n = 0;
    for (int v = 0; v < 256; v++)
      n += hist[v];
    float *lf = av->tile_lut_f + t * 256;
    uint8_t *lu = av->tile_lut + t * 256;
    if (n == 0) {
      for (int v = 0; v < 256; v++)
        lu[v] = lut[v];
      continue;
    }
    // Clip each bin at clip_limit times a flat histogram's height and
    // spread what was cut off evenly over all bins: that bounds the slope
    // of the mapping, which is how much this tile's noise is amplified.
    double limit = clip_limit * (double)n / 256.0;
    double excess = 0;
    for (int v = 0; v < 256; v++)
      if (hist[v] > limit)
        excess += hist[v] - limit;
    double share = excess / 256.0;
    double cum = 0;
    for (int v = 0; v < 256; v++) {
      double h = (hist[v] > limit ? limit : hist[v]) + share;
      // Map each level to the middle of its own bin's share of the CDF, so
      // a flat histogram maps to the identity rather than shifting up.
      double target = (cum + h / 2) * 255.0 / (double)n;
      cum += h;
      double mixed = local_strength * target + (1 - local_strength) * v;
      lf[v] = primed ? (float)(lf[v] + alpha * (mixed - lf[v])) : (float)mixed;
      // lf >= 0 and a float: + 0.5 is exact in double, so this is lround.
      lu[v] = clamp_u8((int)((double)lf[v] + 0.5));
    }
    // Fold the global LUT in: local(global(v)).
    uint8_t tmp[256];
    for (int v = 0; v < 256; v++)
      tmp[v] = lu[lut[v]];
    memcpy(lu, tmp, sizeof tmp);
  }
}

// Chroma: remove the cast in proportion to brightness, then boost
// saturation. Brightness is the source luma under each chroma sample (the
// mean of the two pixels it covers in I422).
typedef struct {
  // off[l]: U offset in the low 16 bits, V in the high, each already
  // including -128 + span, so sat[s + off] is the output.
  uint32_t off[256];
  uint8_t sat[2 * 256 + 1];
} ChromaTab;

static void chroma_tables(const AutoVis *av, double ref_luma, ChromaTab *ct) {
  enum { span = 256 };
  for (int l = 0; l < 256; l++) {
    double k = ref_luma > 0 ? clampd(l / ref_luma, 0, 1.5) : 1;
    int su = (int)lround(av->du * k), sv = (int)lround(av->dv * k);
    su = su < -128 ? -128 : su > 128 ? 128 : su;
    sv = sv < -128 ? -128 : sv > 128 ? 128 : sv;
    ct->off[l] = (uint32_t)(su - 128 + span) | (uint32_t)(sv - 128 + span) << 16;
  }
  for (int o = -span; o <= span; o++)
    ct->sat[o + span] = clamp_u8((int)lround(128 + saturation * o));
}

static void chroma_row(const ChromaTab *ct, const uint8_t *ly, const uint8_t *su,
                       const uint8_t *sv, uint8_t *du, uint8_t *dv, uint32_t w) {
  const uint32_t pairs = w / 2;
  const uint8_t *const sat = ct->sat;
  for (uint32_t c = 0; c < pairs; c++) {
    uint32_t o = ct->off[(ly[2 * c] + ly[2 * c + 1] + 1) >> 1];
    du[c] = sat[su[c] + (o & 0xffff)];
    dv[c] = sat[sv[c] + (o >> 16)];
  }
  if (w & 1) {
    uint32_t o = ct->off[ly[w - 1]];
    du[pairs] = sat[su[pairs] + (o & 0xffff)];
    dv[pairs] = sat[sv[pairs] + (o >> 16)];
  }
}

// Luma, in place: each pixel through the four nearest tiles' mappings,
// blended by distance to their centres.
//
// Bit-exact with the per-pixel form
//   ((a*(256-wx) + b*wx)*(256-wy) + (c*(256-wx) + d*wx)*wy + 32768) >> 16
// which equals, regrouped,
//   ((a*(256-wy) + c*wy)*(256-wx) + (b*(256-wy) + d*wy)*wx + 32768) >> 16.
// Columns are walked in segments of constant left tile, so the four LUT
// pointers are per segment, not per pixel; LOCAL_PREBLEND blends the
// row's top/bottom LUTs once per row into 16-bit tables (2 gathers/pixel
// instead of 4).
static void luma_local(AutoVis *av, const uint8_t *const src[3],
                       uint8_t *const dst[3], const size_t stride3[3],
                       const ChromaTab *ct) {
  const uint8_t *src_plane = src[0];
  uint8_t *y_plane = dst[0];
  const size_t stride = stride3[0];
  const uint32_t tx = av->tx, ty = av->ty, w = av->w, h = av->h;
  const uint16_t *const col_t = av->col_t, *const col_w = av->col_w;
  const uint16_t *const row_t = av->row_t, *const row_w = av->row_w;
  const uint8_t *const luts = av->tile_lut;
  // seg[k]: first column whose left tile is k (col_t is non-decreasing).
  uint32_t seg[max_tiles_x + 1];
  {
    uint32_t x = 0;
    for (uint32_t k = 0; k < tx; k++) {
      seg[k] = x;
      while (x < w && col_t[x] == k)
        x++;
    }
    seg[tx] = w;
  }
#pragma omp for schedule(static)
  for (uint32_t y = 0; y < h; y++) {
    const uint8_t *in = src_plane + (size_t)y * stride;
    uint8_t *row = y_plane + (size_t)y * stride;
    uint32_t t0 = row_t[y];
    uint32_t t1 = t0 + 1 < ty ? t0 + 1 : t0;
    uint32_t wy = row_w[y];
    const uint8_t *top = luts + (size_t)t0 * tx * 256;
    const uint8_t *bot = luts + (size_t)t1 * tx * 256;
    uint16_t vb[max_tiles_x * 256];
    // wy == 0 (and t0 == t1, where wy is 0 too) is the same formula.
    {
      const uint16_t iwy = (uint16_t)(256 - wy), wyy = (uint16_t)wy;
      for (uint32_t k = 0; k < tx; k++) {
        const uint8_t *tp = top + k * 256, *bp = bot + k * 256;
        uint16_t *o = vb + k * 256;
        for (int i = 0; i < 256; i++)
          o[i] = (uint16_t)(tp[i] * iwy + bp[i] * wyy);
      }
    }
    for (uint32_t k = 0; k < tx; k++) {
      const uint16_t *p0 = vb + k * 256;
      uint32_t x = seg[k], xe = seg[k + 1];
      if (k + 1 < tx) {
        const uint16_t *p1 = p0 + 256;
        for (; x < xe; x++) {
          uint32_t v = in[x], wx = col_w[x];
          row[x] = (uint8_t)((p0[v] * (256 - wx) + p1[v] * wx + 32768) >> 16);
        }
      } else {
        for (; x < xe; x++)
          row[x] = (uint8_t)((p0[in[x]] + 128) >> 8);
      }
    }
    chroma_row(ct, in, src[1] + (size_t)y * stride3[1], src[2] + (size_t)y * stride3[2],
               dst[1] + (size_t)y * stride3[1], dst[2] + (size_t)y * stride3[2], w);
  }
}

bool auto_vis_apply(AutoVis *av, const ImgStats *stats,
                    const uint8_t *const src[3], uint8_t *const dst[3],
                    uint32_t width, uint32_t height, const size_t stride[3],
                    uint64_t capture_ns) {
  if (!ensure_tiles(av, width, height)) {
    for (int p = 0; p < 3; p++)
      memcpy(dst[p], src[p], stride[p] * height);
    return false;
  }

  // How far to move toward this frame's targets: by elapsed capture time,
  // so the smoothing is the same at 15fps as at 60. Unknown timestamps
  // count as one frame at 30fps.
  double dt = 1.0 / 30;
  if (capture_ns && av->last_ns && capture_ns > av->last_ns)
    dt = (double)(capture_ns - av->last_ns) / 1e9;
  bool restart = !av->primed || dt > restart_gap_s;
  if (restart) {
    av->tiles_primed = false;
    dt = 0;
  }
  av->last_ns = capture_ns;
  double alpha = restart ? 1.0 : 1.0 - exp(-dt / smooth_tau_s);

  Targets t = targets_for(stats);
  if (restart) {
    av->lo = t.lo;
    av->hi = t.hi;
    av->gamma = t.gamma;
    av->du = t.du;
    av->dv = t.dv;
  } else {
    av->lo += alpha * (t.lo - av->lo);
    av->hi += alpha * (t.hi - av->hi);
    av->gamma += alpha * (t.gamma - av->gamma);
    av->du += alpha * (t.du - av->du);
    av->dv += alpha * (t.dv - av->dv);
  }
  av->primed = true;

  uint8_t lut[256];
  double range = av->hi - av->lo > 1 ? av->hi - av->lo : 1;
  for (int v = 0; v < 256; v++) {
    double n = clampd((v - av->lo) / range, 0, 1);
    lut[v] = clamp_u8((int)lround(255.0 * pow(n, av->gamma)));
  }

  ChromaTab ct;
  chroma_tables(av, stats->mean_luma, &ct);
  // One team for the three passes (each is an orphaned omp for): under
  // OMP_WAIT_POLICY=passive every region start wakes the whole team.
#pragma omp parallel
  {
    luma_global(av, src[0], stride[0]);
    build_tile_luts(av, alpha, lut);
    luma_local(av, src, dst, stride, &ct);
  }
  av->tiles_primed = true;
  return true;
}
