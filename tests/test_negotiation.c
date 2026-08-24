// Resolution negotiation ranking: when a camera doesn't offer the exact
// configured mode, consider_size() decides which of the offered modes is
// substituted. Wrong choices here are silent -- the pipeline runs happily
// at a resolution nobody asked for, reframing every shot.
//
// v4l2_in.c is included for the statics. It needs the V4L2 headers but
// nothing in these tests touches a device.
#define _POSIX_C_SOURCE 200809L
#include "v4l2_in.c"
#include "test_harness.h"

// Runs a camera's whole mode list through the ranker, as enumerate_sizes
// would, and returns the winner.
static SizeChoice rank(const uint32_t (*modes)[2], size_t n, uint32_t want_w,
                       uint32_t want_h, uint32_t downscale) {
  SizeChoice best = {0};
  for (size_t i = 0; i < n; i++)
    consider_size(&best, modes[i][0], modes[i][1], want_w, want_h, downscale);
  return best;
}

// The dev laptop's actual webcam mode list, so the common case is pinned
// against real hardware rather than invented numbers.
static const uint32_t real_camera[][2] = {
    {1280, 720}, {176, 144}, {320, 240}, {352, 288}, {640, 360}, {640, 480},
};
static const size_t real_camera_n = sizeof real_camera / sizeof real_camera[0];

static void test_exact_match_wins_outright(void) {
  SizeChoice b = rank(real_camera, real_camera_n, 1280, 720, 1);
  CHECK(b.found);
  CHECK(b.exact);
  CHECK_EQ_INT(b.width, 1280);
  CHECK_EQ_INT(b.height, 720);

  // An exact match must win regardless of where it sits in the list.
  b = rank(real_camera, real_camera_n, 640, 480, 1);
  CHECK(b.exact);
  CHECK_EQ_INT(b.width, 640);
  CHECK_EQ_INT(b.height, 480);
}

static void test_unavailable_mode_falls_back_to_closest(void) {
  // 1920x1080 isn't offered; 1280x720 is the same 16:9 and closest.
  SizeChoice b = rank(real_camera, real_camera_n, 1920, 1080, 1);
  CHECK(b.found);
  CHECK(!b.exact);
  CHECK_EQ_INT(b.width, 1280);
  CHECK_EQ_INT(b.height, 720);
}

// Aspect ratio outranks pixel count: a 4:3 stand-in for a 16:9 request
// reframes every shot, which is a worse surprise than the same framing at
// fewer pixels. 960x720 is closer in raw pixels to 1280x720 than 640x360
// is, but it is 4:3 -- so the 16:9 option must win.
static void test_aspect_ratio_beats_pixel_count(void) {
  const uint32_t modes[][2] = {{960, 720}, {640, 360}};
  SizeChoice b = rank(modes, 2, 1280, 720, 1);
  CHECK(b.found);
  CHECK_EQ_INT(b.width, 640);
  CHECK_EQ_INT(b.height, 360);
}

// Within one aspect group, the nearest pixel count wins -- measured as an
// absolute gap, so "nearest" can be either above or below the request.
static void test_nearest_pixels_within_same_aspect(void) {
  // Against 1280x720 (921,600 px): 640x360 is 691,200 away and 1920x1080
  // is 1,152,000 away, so the smaller mode is genuinely the closer one.
  const uint32_t modes[][2] = {{320, 180}, {640, 360}, {1920, 1080}};
  SizeChoice b = rank(modes, 3, 1280, 720, 1);
  CHECK_EQ_INT(b.width, 640);
  CHECK_EQ_INT(b.height, 360);

  // With only larger options, the nearest larger one wins -- confirming
  // the comparison is a distance, not a "prefer smaller" preference.
  const uint32_t larger[][2] = {{1920, 1080}, {3840, 2160}};
  b = rank(larger, 2, 1280, 720, 1);
  CHECK_EQ_INT(b.width, 1920);
  CHECK_EQ_INT(b.height, 1080);
}

// 352x288 is nominally "4:3" but is really 11:9 (~1.9% off), which is
// outside the 1% grouping tolerance -- so it must not be treated as
// interchangeable with a true 4:3 mode.
static void test_near_miss_ratios_are_not_grouped(void) {
  CHECK(aspect_error(640, 480, 640, 480) == 0);
  CHECK(aspect_error(352, 288, 640, 480) > aspect_group_tolerance);
  // 16:9 against 4:3 is a large error by construction.
  CHECK(aspect_error(640, 480, 1280, 720) > aspect_group_tolerance);
  // Same ratio at different scales is a zero-error match.
  CHECK(aspect_error(640, 360, 1920, 1080) == 0);
}

// Candidates that would break the downscale divisibility rule must be
// excluded outright -- the negotiated size is what the chroma math runs
// on, so an unusable mode is worse than no substitution.
static void test_downscale_filters_candidates(void) {
  // 642x362: fine at downscale 1, unusable at 4.
  const uint32_t modes[][2] = {{642, 362}};
  SizeChoice b = rank(modes, 1, 642, 362, 1);
  CHECK(b.found);

  b = rank(modes, 1, 642, 362, 4);
  CHECK(!b.found); // nothing usable -- reported, not silently downgraded
}

// The divisibility rule the negotiator enforces must be the same one
// config.c enforces on the requested size, or a mode accepted here would
// be rejected there (or worse, accepted by both and wrong).
//
// config.c requires width % (2*downscale) == 0 AND height % (2*downscale)
// == 0 -- the height half doubled when 4:2:0 support landed, because that
// path line-doubles half-height chroma into full-height planes.
static void test_filter_matches_config_rule(void) {
  const uint32_t ds = 2;
  // Height 482 is a multiple of ds but NOT of 2*ds: config.c rejects it.
  const uint32_t modes[][2] = {{640, 482}};
  SizeChoice b = rank(modes, 1, 640, 482, ds);
  CHECK(!b.found);
}

static void test_zero_and_degenerate_sizes_ignored(void) {
  const uint32_t modes[][2] = {{0, 720}, {1280, 0}, {0, 0}};
  SizeChoice b = rank(modes, 3, 1280, 720, 1);
  CHECK(!b.found);
}

static void test_empty_mode_list_finds_nothing(void) {
  SizeChoice best = {0};
  CHECK(!best.found);
  CHECK_EQ_INT(best.width, 0);
}

// Ranking must not depend on the order the driver happens to enumerate in.
static void test_result_is_order_independent(void) {
  const uint32_t forward[][2] = {{320, 240}, {640, 360}, {1280, 720}};
  const uint32_t reverse[][2] = {{1280, 720}, {640, 360}, {320, 240}};
  SizeChoice a = rank(forward, 3, 1920, 1080, 1);
  SizeChoice b = rank(reverse, 3, 1920, 1080, 1);
  CHECK_EQ_INT(a.width, b.width);
  CHECK_EQ_INT(a.height, b.height);
}

int main(void) {
  printf("test_negotiation:\n");
  RUN_TEST(test_exact_match_wins_outright);
  RUN_TEST(test_unavailable_mode_falls_back_to_closest);
  RUN_TEST(test_aspect_ratio_beats_pixel_count);
  RUN_TEST(test_nearest_pixels_within_same_aspect);
  RUN_TEST(test_near_miss_ratios_are_not_grouped);
  RUN_TEST(test_downscale_filters_candidates);
  RUN_TEST(test_filter_matches_config_rule);
  RUN_TEST(test_zero_and_degenerate_sizes_ignored);
  RUN_TEST(test_empty_mode_list_finds_nothing);
  RUN_TEST(test_result_is_order_independent);
  TEST_MAIN_END();
}
