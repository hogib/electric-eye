// Auto visibility, on synthetic underwater frames.
//
// A deterministic scene (coloured patches, gratings of several
// frequencies, a gradient, mild noise) is put through a simple underwater
// image formation model -- each colour channel attenuated by its own
// transmission, then veiled by scattered water-coloured light:
//
//   I_c = J_c * t_c + B_c * (1 - t_c),  red attenuated most
//
// -- and the corrected frame is checked for what the pilot needs: the
// cast gone, the brightness range back, edges stronger, colours that were
// there recovered, and nothing made worse: no flicker on a still scene, no
// noise blown up into texture, a clean scene left close to itself.
//
// AUTO_VIS_DUMP=<dir> writes the frames as raw yuv422p (full range) for a
// look with ffmpeg, e.g.:
//   ffmpeg -f rawvideo -pix_fmt yuvj422p -s 640x480 -i underwater-out.yuv o.png
#define _POSIX_C_SOURCE 200809L
#include "auto_vis.c"
#include "test_harness.h"
#include <stdio.h>
#include <time.h>

enum { W = 640, H = 480, CW = W / 2 };

typedef struct {
  uint8_t y[W * H], u[CW * H], v[CW * H];
} Frame;

static const size_t strides[3] = {W, CW, CW};

static uint8_t to_u8(double v) { return v < 0 ? 0 : v > 255 ? 255 : (uint8_t)lround(v); }

// The scene in linear-ish RGB, 0-255.
static void scene_rgb(int x, int y, double rgb[3]) {
  // Background: a soft diagonal gradient.
  double g = 60 + 120.0 * (x + y) / (W + H);
  rgb[0] = g * 0.9;
  rgb[1] = g;
  rgb[2] = g * 0.95;
  // Coloured patches: red, yellow, white, near-black, orange.
  static const struct { int x0, y0, x1, y1; double r, g, b; } patch[] = {
      {40, 40, 160, 140, 200, 40, 40},    {200, 40, 320, 140, 220, 200, 50},
      {360, 40, 480, 140, 235, 235, 235}, {520, 40, 620, 140, 15, 15, 15},
      {40, 330, 200, 440, 230, 120, 30},
  };
  for (size_t i = 0; i < sizeof patch / sizeof patch[0]; i++) {
    if (x >= patch[i].x0 && x < patch[i].x1 && y >= patch[i].y0 &&
        y < patch[i].y1) {
      rgb[0] = patch[i].r;
      rgb[1] = patch[i].g;
      rgb[2] = patch[i].b;
    }
  }
  // Gratings, coarse to fine: the detail haze takes first.
  if (y >= 180 && y < 300) {
    double period = x < 200 ? 24 : x < 400 ? 10 : 5;
    double s = sin(2 * 3.14159265358979 * x / period);
    for (int c = 0; c < 3; c++)
      rgb[c] = 128 + 70 * s;
  }
}

static uint32_t lcg_state;
static int noise(int amp) {
  lcg_state = lcg_state * 1664525u + 1013904223u;
  return (int)((lcg_state >> 24) % (2 * amp + 1)) - amp;
}

// RGB -> full-range BT.601 YCbCr, 4:2:2 (chroma averaged over pixel pairs).
static void rgb_to_frame(double (*rgb_at)(int, int, int), Frame *f) {
  for (int y = 0; y < H; y++) {
    for (int c = 0; c < CW; c++) {
      double su = 0, sv = 0;
      for (int k = 0; k < 2; k++) {
        int x = 2 * c + k;
        double r = rgb_at(x, y, 0), g = rgb_at(x, y, 1), b = rgb_at(x, y, 2);
        f->y[y * W + x] = to_u8(0.299 * r + 0.587 * g + 0.114 * b + noise(2));
        su += 128 - 0.168736 * r - 0.331264 * g + 0.5 * b;
        sv += 128 + 0.5 * r - 0.418688 * g - 0.081312 * b;
      }
      f->u[y * CW + c] = to_u8(su / 2);
      f->v[y * CW + c] = to_u8(sv / 2);
    }
  }
}

static double clean_at(int x, int y, int c) {
  double rgb[3];
  scene_rgb(x, y, rgb);
  return rgb[c];
}

// Water: transmission per channel, and the colour of the scattered light.
static double water_t[3] = {0.25, 0.70, 0.80};
static double water_b[3] = {15, 110, 125};
static double water_at(int x, int y, int c) {
  return clean_at(x, y, c) * water_t[c] + water_b[c] * (1 - water_t[c]);
}

static void make(double (*rgb_at)(int, int, int), Frame *f) {
  lcg_state = 12345;
  rgb_to_frame(rgb_at, f);
}

static void run(AutoVis *av, const Frame *in, Frame *out, uint64_t ns) {
  const uint8_t *src[3] = {in->y, in->u, in->v};
  uint8_t *dst[3] = {out->y, out->u, out->v};
  ImgStats st;
  img_stats_compute(src, W, H, strides, &st);
  CHECK(auto_vis_apply(av, &st, src, dst, W, H, strides, ns));
}

static void stats_of(const Frame *f, ImgStats *st) {
  const uint8_t *p[3] = {f->y, f->u, f->v};
  img_stats_compute(p, W, H, strides, st);
}

static double cast_of(const Frame *f) {
  ImgStats st;
  stats_of(f, &st);
  return sqrt(st.cast_u * st.cast_u + st.cast_v * st.cast_v);
}

static int range_of(const Frame *f) {
  ImgStats st;
  stats_of(f, &st);
  return img_stats_percentile(&st, 0.99) - img_stats_percentile(&st, 0.01);
}

// Mean absolute luma difference to the right and below neighbours: how much
// edge there is to see.
static double edge_energy(const Frame *f) {
  double sum = 0;
  for (int y = 0; y < H - 1; y++)
    for (int x = 0; x < W - 1; x++)
      sum += abs(f->y[y * W + x + 1] - f->y[y * W + x]) +
             abs(f->y[(y + 1) * W + x] - f->y[y * W + x]);
  return sum / ((double)(W - 1) * (H - 1));
}

// Mean chroma offset over a rectangle (in luma coordinates).
static void patch_chroma(const Frame *f, int x0, int y0, int x1, int y1,
                         double *u, double *v) {
  double su = 0, sv = 0;
  int n = 0;
  for (int y = y0; y < y1; y++)
    for (int c = x0 / 2; c < x1 / 2; c++, n++) {
      su += f->u[y * CW + c] - 128;
      sv += f->v[y * CW + c] - 128;
    }
  *u = su / n;
  *v = sv / n;
}

static void dump(const char *name, const Frame *f) {
  const char *dir = getenv("AUTO_VIS_DUMP");
  if (!dir)
    return;
  char path[512];
  snprintf(path, sizeof path, "%s/%s.yuv", dir, name);
  FILE *o = fopen(path, "wb");
  if (!o)
    return;
  fwrite(f->y, 1, sizeof f->y, o);
  fwrite(f->u, 1, sizeof f->u, o);
  fwrite(f->v, 1, sizeof f->v, o);
  fclose(o);
}

static const uint64_t frame_ns = 33333333;

// Runs `in` through a fresh AutoVis for `frames` frames; returns the last.
static void settle(const Frame *in, Frame *out, int frames) {
  AutoVis *av = auto_vis_create();
  for (int i = 0; i < frames; i++)
    run(av, in, out, (uint64_t)(i + 1) * frame_ns);
  auto_vis_destroy(av);
}

static Frame clean, water, out, out2;

static void test_underwater_frame_is_corrected(void) {
  make(water_at, &water);
  settle(&water, &out, 30);
  dump("underwater-in", &water);
  dump("underwater-out", &out);

  double cast_in = cast_of(&water), cast_out = cast_of(&out);
  int range_in = range_of(&water), range_out = range_of(&out);
  double edges_in = edge_energy(&water), edges_out = edge_energy(&out);
  printf("    cast %.1f -> %.1f, luma range %d -> %d, edge energy %.2f -> %.2f\n",
         cast_in, cast_out, range_in, range_out, edges_in, edges_out);

  CHECK(cast_in > 15);           // the model really does cast the frame
  CHECK(cast_out < cast_in / 3); // most of the cast is gone
  CHECK(range_out >= 220);       // brightness range back to nearly full
  CHECK(edges_out > 2 * edges_in);

  // The red patch: the water turned it grey-green (V at or below neutral);
  // corrected, it reads red again.
  double u_in, v_in, u_out, v_out;
  patch_chroma(&water, 40, 40, 160, 140, &u_in, &v_in);
  patch_chroma(&out, 40, 40, 160, 140, &u_out, &v_out);
  printf("    red patch V %.1f -> %.1f\n", v_in, v_out);
  CHECK(v_out > v_in + 15);
  CHECK(v_out > 15);
}

// A scene that needs nothing should come out close to itself. Its colourful
// patches average out to a small cast of their own (~10 here), and a plain
// gray-world correction "removes" that by tinting everything else the
// opposite way -- it turned this scene's grey background visibly blue -- so
// the check is on the background: it must stay the colour it was.
static void test_clean_frame_left_mostly_alone(void) {
  make(clean_at, &clean);
  settle(&clean, &out, 30);
  dump("clean-in", &clean);
  dump("clean-out", &out);
  double cast_in = cast_of(&clean), cast_out = cast_of(&out);
  double bu_in, bv_in, bu, bv;
  patch_chroma(&clean, 240, 330, 620, 470, &bu_in, &bv_in);
  patch_chroma(&out, 240, 330, 620, 470, &bu, &bv);
  printf("    cast %.1f -> %.1f, grey background U/V %.1f/%.1f -> %.1f/%.1f, "
         "luma range %d -> %d\n",
         cast_in, cast_out, bu_in, bv_in, bu, bv, range_of(&clean),
         range_of(&out));
  // The background is slightly green by construction (V ~ -7), and the
  // saturation boost alone moves that by ~1.4; a cast "correction" of the
  // scene's own colours moved it by twice the allowance here.
  CHECK(fabs(bu - bu_in) < 4 && fabs(bv - bv_in) < 4);
  CHECK(fabs(cast_out - cast_in) < 3);
  // Colours keep their identity: the red patch stays red, the yellow one
  // yellow (strongly negative U).
  double u, v;
  patch_chroma(&out, 40, 40, 160, 140, &u, &v);
  CHECK(v > 40);
  patch_chroma(&out, 200, 40, 320, 140, &u, &v);
  CHECK(u < -40);
}

// A still scene must give a still picture: once settled, consecutive
// frames of the same input differ by at most a level, almost nowhere.
static void test_still_scene_does_not_flicker(void) {
  make(water_at, &water);
  AutoVis *av = auto_vis_create();
  for (int i = 0; i < 90; i++)
    run(av, &water, &out, (uint64_t)(i + 1) * frame_ns);
  run(av, &water, &out2, 91 * frame_ns);
  size_t differ = 0;
  int worst = 0;
  for (size_t i = 0; i < sizeof out.y; i++) {
    int d = abs(out.y[i] - out2.y[i]);
    if (d) differ++;
    if (d > worst) worst = d;
  }
  CHECK(worst <= 1);
  CHECK(differ < sizeof out.y / 1000);
  auto_vis_destroy(av);
}

static double mean_luma(const Frame *f) {
  double s = 0;
  for (size_t i = 0; i < sizeof f->y; i++)
    s += f->y[i];
  return s / sizeof f->y;
}

// A sudden change (a light switched on) is followed smoothly, not in one
// jump, and settles within about a second and a half.
static void test_changes_are_followed_smoothly(void) {
  static Frame bright, settled;
  make(water_at, &water);
  bright = water;
  for (size_t i = 0; i < sizeof bright.y; i++)
    bright.y[i] = to_u8(bright.y[i] * 1.6 + 20);
  settle(&bright, &settled, 30); // where it should end up

  AutoVis *av = auto_vis_create();
  uint64_t t = 0;
  for (int i = 0; i < 60; i++)
    run(av, &water, &out, t += frame_ns);
  run(av, &bright, &out, t += frame_ns);
  double first = fabs(mean_luma(&out) - mean_luma(&settled));
  for (int i = 0; i < 45; i++)
    run(av, &bright, &out, t += frame_ns);
  double later = fabs(mean_luma(&out) - mean_luma(&settled));
  printf("    mean luma off its settled value by %.1f, then %.1f after 1.5s\n",
         first, later);
  CHECK(first > 3); // it did not snap
  CHECK(later < 1.5);
  auto_vis_destroy(av);
}

// Switching on, or a gap in the frames, starts from the current frame: the
// first frame is already the settled answer, with no fade-in.
static void test_restart_has_no_fade_in(void) {
  make(water_at, &water);
  settle(&water, &out, 1);
  settle(&water, &out2, 60);
  CHECK_MEM_EQ(out.y, out2.y, sizeof out.y);

  AutoVis *av = auto_vis_create();
  make(clean_at, &clean);
  for (int i = 0; i < 30; i++)
    run(av, &clean, &out, (uint64_t)(i + 1) * frame_ns);
  run(av, &water, &out, 31 * frame_ns + 2000000000ull); // a 2s gap
  CHECK_MEM_EQ(out.y, out2.y, sizeof out.y);
  auto_vis_destroy(av);
}

// A flat, dark, noisy frame -- open water at depth -- must not have its
// noise stretched into texture.
static void test_noise_is_not_blown_up(void) {
  lcg_state = 99;
  for (size_t i = 0; i < sizeof water.y; i++)
    water.y[i] = (uint8_t)(30 + noise(3));
  memset(water.u, 140, sizeof water.u);
  memset(water.v, 110, sizeof water.v);
  settle(&water, &out, 30);
  double m_in = mean_luma(&water), m_out = mean_luma(&out), v_in = 0, v_out = 0;
  for (size_t i = 0; i < sizeof water.y; i++) {
    v_in += (water.y[i] - m_in) * (water.y[i] - m_in);
    v_out += (out.y[i] - m_out) * (out.y[i] - m_out);
  }
  double sd_in = sqrt(v_in / sizeof water.y), sd_out = sqrt(v_out / sizeof water.y);
  printf("    noise sd %.2f -> %.2f\n", sd_in, sd_out);
  CHECK(sd_out < 4.5 * sd_in);
}

static void test_uniform_frame(void) {
  memset(water.y, 128, sizeof water.y);
  memset(water.u, 128, sizeof water.u);
  memset(water.v, 128, sizeof water.v);
  settle(&water, &out, 3);
  for (size_t i = 1; i < sizeof out.y; i++)
    if (out.y[i] != out.y[0]) {
      CHECK(!"uniform luma stays uniform");
      break;
    }
  CHECK(out.u[0] == 128 && out.v[0] == 128);
}

// Odd and tiny sizes: one tile, the edge column of I422 chroma.
static void test_small_frames(void) {
  static uint8_t y[34 * 6], u[17 * 6], v[17 * 6], oy[34 * 6], ou[17 * 6], ov[17 * 6];
  for (size_t i = 0; i < sizeof y; i++)
    y[i] = (uint8_t)(i * 7);
  memset(u, 100, sizeof u);
  memset(v, 150, sizeof v);
  const size_t st[3] = {34, 17, 17};
  const uint8_t *src[3] = {y, u, v};
  uint8_t *dst[3] = {oy, ou, ov};
  ImgStats s;
  img_stats_compute(src, 34, 6, st, &s);
  AutoVis *av = auto_vis_create();
  CHECK(auto_vis_apply(av, &s, src, dst, 34, 6, st, 0));
  CHECK(av->tx == 1 && av->ty == 1);
  auto_vis_destroy(av);
}

// What it costs per frame, at the sizes that matter. Informational: the
// budget is the frame period minus everything else, and the Pi is the
// machine that decides it.
static void test_cost(void) {
  static const uint32_t sizes[][2] = {{640, 480}, {1280, 720}};
  for (size_t k = 0; k < 2; k++) {
    uint32_t w = sizes[k][0], h = sizes[k][1];
    size_t st[3] = {w, w / 2, w / 2};
    uint8_t *buf = malloc((size_t)w * h * 4);
    CHECK(buf != NULL);
    if (!buf)
      return;
    uint8_t *sy = buf, *su = sy + (size_t)w * h, *sv = su + (size_t)w / 2 * h;
    uint8_t *dy = sv + (size_t)w / 2 * h, *du = dy + (size_t)w * h,
            *dv = du + (size_t)w / 2 * h;
    lcg_state = 7;
    for (size_t i = 0; i < (size_t)w * h * 2; i++)
      buf[i] = (uint8_t)(100 + noise(60));
    const uint8_t *src[3] = {sy, su, sv};
    uint8_t *dst[3] = {dy, du, dv};
    AutoVis *av = auto_vis_create();
    ImgStats s;
    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    enum { n = 30 };
    for (int i = 0; i < n; i++) {
      img_stats_compute(src, w, h, st, &s);
      auto_vis_apply(av, &s, src, dst, w, h, st, (uint64_t)(i + 1) * frame_ns);
    }
    clock_gettime(CLOCK_MONOTONIC, &t1);
    double ms = ((t1.tv_sec - t0.tv_sec) * 1e3 + (t1.tv_nsec - t0.tv_nsec) / 1e6) / n;
    printf("    %ux%u: %.2f ms per frame (stats + correction)\n", w, h, ms);
    auto_vis_destroy(av);
    free(buf);
  }
}

int main(void) {
  RUN_TEST(test_underwater_frame_is_corrected);
  RUN_TEST(test_clean_frame_left_mostly_alone);
  RUN_TEST(test_still_scene_does_not_flicker);
  RUN_TEST(test_changes_are_followed_smoothly);
  RUN_TEST(test_restart_has_no_fade_in);
  RUN_TEST(test_noise_is_not_blown_up);
  RUN_TEST(test_uniform_frame);
  RUN_TEST(test_small_frames);
  RUN_TEST(test_cost);
  TEST_MAIN_END();
}
