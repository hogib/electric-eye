// Laplacian of Gaussian zero-crossing edges. The properties worth pinning
// are the ones that define the operator rather than any particular pixel:
// contours are thin, the threshold rejects noise, and a flat field
// produces nothing at all.
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

static void free_fixture(Fixture *f) {
  vf_free(f->in);
  vf_free(f->out);
}

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

static void run_log(Fixture *f, uint8_t strength, uint8_t threshold) {
  const uint8_t *src[3] = {f->in->raw_planes[0], f->in->raw_planes[1],
                           f->in->raw_planes[2]};
  uint8_t *dst[3] = {f->out->planes[0], f->out->planes[1], f->out->planes[2]};
  log_edges(src, dst, f->in->width, f->in->height, f->out->stride, strength,
            threshold);
}

static int count_edges(const VideoFrame *f) {
  int n = 0;
  for (uint32_t y = 0; y < f->height; y++)
    for (uint32_t x = 0; x < f->width; x++)
      if (get_out(f, x, y))
        n++;
  return n;
}

// A uniform field has no edges anywhere. This is the single most
// important property: a false positive here means every flat region of
// every frame lights up.
static void test_flat_field_has_no_edges(void) {
  Fixture f = make_fixture(64, 64);
  for (int v = 0; v <= 255; v += 85) {
    fill_luma(f.in, (uint8_t)v);
    run_log(&f, 1, 0); // threshold 0: even the most permissive setting
    CHECK_EQ_INT(count_edges(f.out), 0);
  }
  free_fixture(&f);
}

// Output must be strictly binary -- the contract downstream is an edge
// map, not a response magnitude.
static void test_output_is_binary(void) {
  Fixture f = make_fixture(64, 64);
  fill_luma(f.in, 60);
  for (uint32_t y = 0; y < 64; y++)
    for (uint32_t x = 32; x < 64; x++)
      set_px(f.in, x, y, 200);
  run_log(&f, 1, 10);
  for (uint32_t y = 0; y < 64; y++)
    for (uint32_t x = 0; x < 64; x++) {
      uint8_t v = get_out(f.out, x, y);
      CHECK(v == 0 || v == 255);
    }
  free_fixture(&f);
}

// A single step edge must produce exactly one pixel per row -- the thin
// contour that is the whole reason to use zero-crossings over a gradient
// magnitude. A 2px-wide response would mean both sides of the crossing are
// being marked.
static void test_step_edge_is_one_pixel_wide(void) {
  Fixture f = make_fixture(64, 64);
  fill_luma(f.in, 60);
  for (uint32_t y = 0; y < 64; y++)
    for (uint32_t x = 32; x < 64; x++)
      set_px(f.in, x, y, 200);
  run_log(&f, 1, 10);

  // Interior rows only: the top and bottom rows border-replicate, which is
  // a deliberately different case (covered below).
  for (uint32_t y = 2; y < 62; y++) {
    int on_this_row = 0;
    for (uint32_t x = 0; x < 64; x++)
      if (get_out(f.out, x, y))
        on_this_row++;
    CHECK_EQ_INT(on_this_row, 1);
  }
  free_fixture(&f);
}

// ...and that pixel must sit at the actual edge, not drift away from it.
static void test_edge_is_located_correctly(void) {
  Fixture f = make_fixture(64, 64);
  fill_luma(f.in, 60);
  for (uint32_t y = 0; y < 64; y++)
    for (uint32_t x = 32; x < 64; x++)
      set_px(f.in, x, y, 200);
  run_log(&f, 1, 10);

  int off_edge = 0;
  for (uint32_t y = 2; y < 62; y++)
    for (uint32_t x = 0; x < 64; x++)
      if (get_out(f.out, x, y) && (x < 30 || x > 33))
        off_edge++;
  CHECK_EQ_INT(off_edge, 0);
  free_fixture(&f);
}

// The threshold is a slope test: raising it must monotonically reduce the
// number of edges, never increase it.
static void test_threshold_is_monotonic(void) {
  Fixture f = make_fixture(64, 64);
  // Textured content, so there is a spread of response strengths to cut.
  for (uint32_t y = 0; y < 64; y++)
    for (uint32_t x = 0; x < 64; x++)
      set_px(f.in, x, y, (uint8_t)((x * 13 + y * 29) % 200 + 20));

  int prev = -1;
  for (int t = 0; t <= 120; t += 20) {
    run_log(&f, 1, (uint8_t)t);
    int n = count_edges(f.out);
    if (prev >= 0)
      CHECK(n <= prev);
    prev = n;
  }
  free_fixture(&f);
}

// A low-amplitude ripple on an otherwise flat field is noise, not
// structure: a modest threshold must reject all of it while a real step
// edge in the same frame survives.
static void test_threshold_rejects_noise_keeps_edge(void) {
  Fixture f = make_fixture(64, 64);
  for (uint32_t y = 0; y < 64; y++)
    for (uint32_t x = 0; x < 64; x++) {
      int base = x < 32 ? 60 : 200;
      set_px(f.in, x, y, (uint8_t)(base + (int)((x * 7 + y * 13) % 5) - 2));
    }

  run_log(&f, 1, 30);
  // Exactly one edge pixel per row, all of them at the real edge.
  CHECK_EQ_INT(count_edges(f.out), 64);
  int off_edge = 0;
  for (uint32_t y = 0; y < 64; y++)
    for (uint32_t x = 0; x < 64; x++)
      if (get_out(f.out, x, y) && (x < 31 || x > 33))
        off_edge++;
  CHECK_EQ_INT(off_edge, 0);
  free_fixture(&f);
}

// Higher strength means more Gaussian smoothing, so fine detail must
// progressively disappear. Checked on a high-frequency checkerboard, which
// is exactly what smoothing should destroy.
static void test_strength_smooths_away_fine_detail(void) {
  Fixture f = make_fixture(64, 64);
  for (uint32_t y = 0; y < 64; y++)
    for (uint32_t x = 0; x < 64; x++)
      set_px(f.in, x, y, ((x + y) & 1) ? 220 : 40);

  run_log(&f, 1, 20);
  int fine = count_edges(f.out);
  run_log(&f, 6, 20);
  int coarse = count_edges(f.out);
  CHECK(coarse < fine);
  free_fixture(&f);
}

// 0 and 1 both mean a single pass, matching gaussian_blur's strength --
// documented in conv.h, and easy to break.
static void test_strength_zero_equals_one(void) {
  Fixture f = make_fixture(64, 64);
  for (uint32_t y = 0; y < 64; y++)
    for (uint32_t x = 0; x < 64; x++)
      set_px(f.in, x, y, (uint8_t)((x * 11 + y * 17) % 256));

  run_log(&f, 0, 15);
  uint8_t *first = malloc(f.out->plane_sizes[0]);
  memcpy(first, f.out->planes[0], f.out->plane_sizes[0]);
  run_log(&f, 1, 15);
  CHECK_MEM_EQ(f.out->planes[0], first, f.out->plane_sizes[0]);
  free(first);
  free_fixture(&f);
}

// The result is a luma-only edge map, so chroma must be neutralized --
// carrying the source's colour through would tint white contours with
// hues that no longer correspond to anything in the output. Same contract
// as sobel_edges.
static void test_chroma_is_neutralized(void) {
  Fixture f = make_fixture(64, 64);
  fill_luma(f.in, 128);
  memset(f.in->raw_planes[1], 200, f.in->plane_sizes[1]);
  memset(f.in->raw_planes[2], 40, f.in->plane_sizes[2]);
  memset(f.out->planes[1], 7, f.out->plane_sizes[1]); // poison
  memset(f.out->planes[2], 7, f.out->plane_sizes[2]);

  run_log(&f, 1, 10);
  for (size_t i = 0; i < f.out->plane_sizes[1]; i++)
    CHECK_EQ_INT(f.out->planes[1][i], 128);
  for (size_t i = 0; i < f.out->plane_sizes[2]; i++)
    CHECK_EQ_INT(f.out->planes[2][i], 128);
  free_fixture(&f);
}

// Borders replicate rather than zero-pad. Zero-padding would manufacture a
// hard response along the whole frame boundary that isn't in the scene --
// on a flat field the border must stay as empty as the interior.
static void test_borders_do_not_manufacture_edges(void) {
  Fixture f = make_fixture(64, 64);
  fill_luma(f.in, 120);
  run_log(&f, 1, 0);
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

// Non-square and odd-ish dimensions must not crash or read out of bounds.
static void test_various_dimensions(void) {
  const uint32_t dims[][2] = {{16, 16}, {64, 32}, {32, 64}, {80, 48}, {2, 2}};
  for (size_t i = 0; i < sizeof dims / sizeof dims[0]; i++) {
    Fixture f = make_fixture(dims[i][0], dims[i][1]);
    for (uint32_t y = 0; y < dims[i][1]; y++)
      for (uint32_t x = 0; x < dims[i][0]; x++)
        set_px(f.in, x, y, (uint8_t)((x * 31 + y * 7) % 256));
    run_log(&f, 2, 10);
    // Reaching here without a crash or sanitizer trip is the assertion;
    // also confirm it wrote something binary rather than leaving garbage.
    for (uint32_t y = 0; y < dims[i][1]; y++)
      for (uint32_t x = 0; x < dims[i][0]; x++) {
        uint8_t v = get_out(f.out, x, y);
        CHECK(v == 0 || v == 255);
      }
    free_fixture(&f);
  }
}

// A NULL plane must be a no-op, not a crash -- matches sobel_edges and
// gaussian_blur, which both guard this.
static void test_null_planes_are_safe(void) {
  Fixture f = make_fixture(16, 16);
  const uint8_t *src[3] = {NULL, NULL, NULL};
  uint8_t *dst[3] = {f.out->planes[0], f.out->planes[1], f.out->planes[2]};
  log_edges(src, dst, 16, 16, f.out->stride, 1, 0);
  free_fixture(&f);
}

int main(void) {
  printf("test_log_edges:\n");
  RUN_TEST(test_flat_field_has_no_edges);
  RUN_TEST(test_output_is_binary);
  RUN_TEST(test_step_edge_is_one_pixel_wide);
  RUN_TEST(test_edge_is_located_correctly);
  RUN_TEST(test_threshold_is_monotonic);
  RUN_TEST(test_threshold_rejects_noise_keeps_edge);
  RUN_TEST(test_strength_smooths_away_fine_detail);
  RUN_TEST(test_strength_zero_equals_one);
  RUN_TEST(test_chroma_is_neutralized);
  RUN_TEST(test_borders_do_not_manufacture_edges);
  RUN_TEST(test_various_dimensions);
  RUN_TEST(test_null_planes_are_safe);
  TEST_MAIN_END();
}
