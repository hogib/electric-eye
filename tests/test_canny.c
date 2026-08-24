// Canny edge detection. Shares the shape-level contracts with log_edges
// (binary output, thin contours, neutral chroma, safe borders), plus the
// two properties unique to this operator: non-maximum suppression thins
// ridges to one pixel, and hysteresis keeps a weak edge only where it
// connects to a strong one.
#include "conv.h"
#include "test_harness.h"
#include "video_frame.h"
#include <stdlib.h>

typedef struct {
  VideoFrame *in;
  VideoFrame *out;
} Fixture;

static Fixture make_fixture(uint32_t w, uint32_t h) {
  Fixture f = {vf_create(w, h, 0), vf_create(w, h, 0)};
  return f;
}
static void free_fixture(Fixture *f) { vf_free(f->in); vf_free(f->out); }

static void fill_luma(VideoFrame *f, uint8_t v) {
  for (uint32_t y = 0; y < f->height; y++)
    memset(f->raw_planes[0] + (size_t)y * f->stride[0], v, f->width);
}
static void set_px(VideoFrame *f, uint32_t x, uint32_t y, uint8_t v) {
  f->raw_planes[0][(size_t)y * f->stride[0] + x] = v;
}
static uint8_t get_out(const VideoFrame *f, uint32_t x, uint32_t y) {
  return f->planes[0][(size_t)y * f->stride[0] + x];
}
static void run(Fixture *f, uint8_t strength, uint8_t low, uint8_t high) {
  const uint8_t *src[3] = {f->in->raw_planes[0], f->in->raw_planes[1],
                           f->in->raw_planes[2]};
  uint8_t *dst[3] = {f->out->planes[0], f->out->planes[1], f->out->planes[2]};
  canny_edges(src, dst, f->in->width, f->in->height, f->out->stride, strength,
              low, high);
}
static int count_edges(const VideoFrame *f) {
  int n = 0;
  for (uint32_t y = 0; y < f->height; y++)
    for (uint32_t x = 0; x < f->width; x++)
      if (get_out(f, x, y)) n++;
  return n;
}

// A vertical step edge: dark left, bright right, transition at x == split.
static void make_step(Fixture *f, uint32_t split, uint8_t dark, uint8_t light) {
  for (uint32_t y = 0; y < f->in->height; y++)
    for (uint32_t x = 0; x < f->in->width; x++)
      set_px(f->in, x, y, x < split ? dark : light);
}

static void test_flat_field_has_no_edges(void) {
  Fixture f = make_fixture(64, 64);
  for (int v = 0; v <= 255; v += 85) {
    fill_luma(f.in, (uint8_t)v);
    run(&f, 1, 1, 1); // lowest thresholds that can still seed a fill
    CHECK_EQ_INT(count_edges(f.out), 0);
  }
  free_fixture(&f);
}

static void test_output_is_binary(void) {
  Fixture f = make_fixture(64, 64);
  make_step(&f, 32, 40, 220);
  run(&f, 1, 40, 90);
  for (uint32_t y = 0; y < 64; y++)
    for (uint32_t x = 0; x < 64; x++) {
      uint8_t v = get_out(f.out, x, y);
      CHECK(v == 0 || v == 255);
    }
  free_fixture(&f);
}

// Non-maximum suppression's entire job: a step edge produces a ridge
// several pixels wide in raw gradient magnitude, and this must thin it to
// exactly one pixel per row. Without NMS this test fails with 2-3.
static void test_nms_thins_edge_to_one_pixel(void) {
  Fixture f = make_fixture(64, 64);
  make_step(&f, 32, 40, 220);
  run(&f, 1, 40, 90);
  for (uint32_t y = 8; y < 56; y++) { // interior rows only
    int on_row = 0;
    for (uint32_t x = 0; x < 64; x++)
      if (get_out(f.out, x, y)) on_row++;
    CHECK_EQ_INT(on_row, 1);
  }
  free_fixture(&f);
}

static void test_edge_is_located_correctly(void) {
  Fixture f = make_fixture(64, 64);
  make_step(&f, 32, 40, 220);
  run(&f, 1, 40, 90);
  int off = 0;
  for (uint32_t y = 8; y < 56; y++)
    for (uint32_t x = 0; x < 64; x++)
      if (get_out(f.out, x, y) && (x < 30 || x > 33)) off++;
  CHECK_EQ_INT(off, 0);
  free_fixture(&f);
}

// Hysteresis, the property that distinguishes Canny from a thresholded
// Sobel: a weak edge segment touching a strong one is kept.
static void test_weak_edge_connected_to_strong_survives(void) {
  Fixture f = make_fixture(64, 64);
  fill_luma(f.in, 100);
  // A single vertical line: strong contrast on the top half, weak on the
  // bottom half, forming one continuous edge.
  for (uint32_t y = 0; y < 64; y++) {
    uint8_t amp = (y < 32) ? 200 : 130; // both sides of the same line
    for (uint32_t x = 32; x < 64; x++)
      set_px(f.in, x, y, amp);
  }
  // Thresholds chosen so the weak half alone would not seed an edge.
  run(&f, 1, 20, 60);

  int strong_half = 0, weak_half = 0;
  for (uint32_t y = 4; y < 30; y++)
    for (uint32_t x = 0; x < 64; x++)
      if (get_out(f.out, x, y)) strong_half++;
  for (uint32_t y = 34; y < 60; y++)
    for (uint32_t x = 0; x < 64; x++)
      if (get_out(f.out, x, y)) weak_half++;
  CHECK(strong_half > 0);
  CHECK(weak_half > 0); // carried by connectivity, not by its own strength
  free_fixture(&f);
}

// ...and the converse: a weak edge with nothing strong anywhere in the
// frame must be dropped entirely. If this passes while the test above
// also passes, hysteresis is genuinely doing connectivity rather than
// just thresholding at `low`.
static void test_isolated_weak_edge_is_dropped(void) {
  Fixture f = make_fixture(64, 64);
  fill_luma(f.in, 100);
  for (uint32_t y = 0; y < 64; y++)
    for (uint32_t x = 32; x < 64; x++)
      set_px(f.in, x, y, 112); // very low contrast everywhere
  run(&f, 1, 20, 200);         // high is unreachable -> nothing seeds
  CHECK_EQ_INT(count_edges(f.out), 0);
  free_fixture(&f);
}

// Raising `high` can only remove seeds, so the edge count must not grow.
static void test_high_threshold_is_monotonic(void) {
  Fixture f = make_fixture(64, 64);
  for (uint32_t y = 0; y < 64; y++)
    for (uint32_t x = 0; x < 64; x++)
      set_px(f.in, x, y, (uint8_t)((x * 13 + y * 29) % 200 + 20));
  int prev = -1;
  for (int h = 20; h <= 240; h += 40) {
    run(&f, 1, 10, (uint8_t)h);
    int n = count_edges(f.out);
    if (prev >= 0) CHECK(n <= prev);
    prev = n;
  }
  free_fixture(&f);
}

// Thresholds may arrive in either order; reversing them must not silently
// produce an empty map.
static void test_reversed_thresholds_are_swapped(void) {
  Fixture f = make_fixture(64, 64);
  make_step(&f, 32, 40, 220);
  run(&f, 1, 40, 90);
  int normal = count_edges(f.out);
  run(&f, 1, 90, 40); // same pair, reversed
  int reversed = count_edges(f.out);
  CHECK(normal > 0);
  CHECK_EQ_INT(reversed, normal);
  free_fixture(&f);
}

// strength 0 must behave as 1 -- the smoothing is mandatory because it is
// what bounds hysteresis's cost, so 0 must not mean "skip it".
static void test_strength_zero_equals_one(void) {
  Fixture f = make_fixture(64, 64);
  for (uint32_t y = 0; y < 64; y++)
    for (uint32_t x = 0; x < 64; x++)
      set_px(f.in, x, y, (uint8_t)((x * 11 + y * 17) % 256));
  run(&f, 0, 40, 90);
  uint8_t *first = malloc(f.out->plane_sizes[0]);
  memcpy(first, f.out->planes[0], f.out->plane_sizes[0]);
  run(&f, 1, 40, 90);
  CHECK_MEM_EQ(f.out->planes[0], first, f.out->plane_sizes[0]);
  free(first);
  free_fixture(&f);
}

// More smoothing must not increase fine detail.
static void test_strength_smooths_away_fine_detail(void) {
  Fixture f = make_fixture(64, 64);
  for (uint32_t y = 0; y < 64; y++)
    for (uint32_t x = 0; x < 64; x++)
      set_px(f.in, x, y, ((x + y) & 1) ? 220 : 40);
  run(&f, 1, 40, 90);
  int fine = count_edges(f.out);
  run(&f, 6, 40, 90);
  int coarse = count_edges(f.out);
  CHECK(coarse <= fine);
  free_fixture(&f);
}

static void test_chroma_is_neutralized(void) {
  Fixture f = make_fixture(64, 64);
  fill_luma(f.in, 128);
  memset(f.in->raw_planes[1], 200, f.in->plane_sizes[1]);
  memset(f.in->raw_planes[2], 40, f.in->plane_sizes[2]);
  memset(f.out->planes[1], 7, f.out->plane_sizes[1]);
  memset(f.out->planes[2], 7, f.out->plane_sizes[2]);
  run(&f, 1, 40, 90);
  for (size_t i = 0; i < f.out->plane_sizes[1]; i++)
    CHECK_EQ_INT(f.out->planes[1][i], 128);
  for (size_t i = 0; i < f.out->plane_sizes[2]; i++)
    CHECK_EQ_INT(f.out->planes[2][i], 128);
  free_fixture(&f);
}

static void test_borders_do_not_manufacture_edges(void) {
  Fixture f = make_fixture(64, 64);
  fill_luma(f.in, 120);
  run(&f, 1, 1, 1);
  for (uint32_t x = 0; x < 64; x++) {
    CHECK_EQ_INT(get_out(f.out, x, 0), 0);
    CHECK_EQ_INT(get_out(f.out, x, 63), 0);
  }
  for (uint32_t y = 0; y < 64; y++) {
    CHECK_EQ_INT(get_out(f.out, 0, y), 0);
    CHECK_EQ_INT(get_out(f.out, 63, y), 0);
  }
  free_fixture(&f);
}

// A horizontal edge exercises the vertical direction sector, so a
// transposed direction table (an easy mistake) shows up here.
static void test_horizontal_edge_is_detected(void) {
  Fixture f = make_fixture(64, 64);
  for (uint32_t y = 0; y < 64; y++)
    for (uint32_t x = 0; x < 64; x++)
      set_px(f.in, x, y, y < 32 ? 40 : 220);
  run(&f, 1, 40, 90);
  for (uint32_t x = 8; x < 56; x++) {
    int on_col = 0;
    for (uint32_t y = 0; y < 64; y++)
      if (get_out(f.out, x, y)) on_col++;
    CHECK_EQ_INT(on_col, 1); // thinned to one pixel down each column too
  }
  free_fixture(&f);
}

static void test_various_dimensions(void) {
  const uint32_t dims[][2] = {{16, 16}, {64, 32}, {32, 64}, {80, 48}, {4, 4}};
  for (size_t i = 0; i < sizeof dims / sizeof dims[0]; i++) {
    Fixture f = make_fixture(dims[i][0], dims[i][1]);
    for (uint32_t y = 0; y < dims[i][1]; y++)
      for (uint32_t x = 0; x < dims[i][0]; x++)
        set_px(f.in, x, y, (uint8_t)((x * 31 + y * 7) % 256));
    run(&f, 2, 40, 90);
    for (uint32_t y = 0; y < dims[i][1]; y++)
      for (uint32_t x = 0; x < dims[i][0]; x++) {
        uint8_t v = get_out(f.out, x, y);
        CHECK(v == 0 || v == 255);
      }
    free_fixture(&f);
  }
}

// Degenerate sizes with no interior pixel must be a no-op, not a crash.
static void test_tiny_frames_are_safe(void) {
  for (uint32_t d = 1; d <= 2; d++) {
    Fixture f = make_fixture(d, d);
    fill_luma(f.in, 128);
    run(&f, 1, 40, 90);
    free_fixture(&f);
  }
}

static void test_null_planes_are_safe(void) {
  Fixture f = make_fixture(16, 16);
  const uint8_t *src[3] = {NULL, NULL, NULL};
  uint8_t *dst[3] = {f.out->planes[0], f.out->planes[1], f.out->planes[2]};
  canny_edges(src, dst, 16, 16, f.out->stride, 1, 40, 90);
  free_fixture(&f);
}

// Repeated calls reuse file-static scratch, so a stale-state bug would
// show up on the second run rather than the first.
static void test_repeated_calls_are_stable(void) {
  Fixture f = make_fixture(64, 64);
  make_step(&f, 32, 40, 220);
  run(&f, 2, 40, 90);
  uint8_t *first = malloc(f.out->plane_sizes[0]);
  memcpy(first, f.out->planes[0], f.out->plane_sizes[0]);
  for (int i = 0; i < 5; i++) {
    run(&f, 2, 40, 90);
    CHECK_MEM_EQ(f.out->planes[0], first, f.out->plane_sizes[0]);
  }
  free(first);
  free_fixture(&f);
}

int main(void) {
  printf("test_canny:\n");
  RUN_TEST(test_flat_field_has_no_edges);
  RUN_TEST(test_output_is_binary);
  RUN_TEST(test_nms_thins_edge_to_one_pixel);
  RUN_TEST(test_edge_is_located_correctly);
  RUN_TEST(test_weak_edge_connected_to_strong_survives);
  RUN_TEST(test_isolated_weak_edge_is_dropped);
  RUN_TEST(test_high_threshold_is_monotonic);
  RUN_TEST(test_reversed_thresholds_are_swapped);
  RUN_TEST(test_strength_zero_equals_one);
  RUN_TEST(test_strength_smooths_away_fine_detail);
  RUN_TEST(test_chroma_is_neutralized);
  RUN_TEST(test_borders_do_not_manufacture_edges);
  RUN_TEST(test_horizontal_edge_is_detected);
  RUN_TEST(test_various_dimensions);
  RUN_TEST(test_tiny_frames_are_safe);
  RUN_TEST(test_null_planes_are_safe);
  RUN_TEST(test_repeated_calls_are_stable);
  TEST_MAIN_END();
}
