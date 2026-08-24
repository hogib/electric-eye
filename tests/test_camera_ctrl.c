// Camera controls: the portable mapping layer between one config and two
// backends that disagree on units, ranges, and even how a control is
// selected.
//
// The V4L2 side needs a real device to exercise its ioctls, so what is
// tested here is everything that decides *what gets sent*: the unset
// sentinel, the range scaling, the unit conversion, and the rpicam
// argument construction. Whether a given camera then honours those values
// is a hardware question, checked live rather than here.
#define _POSIX_C_SOURCE 200809L
#include "camera_ctrl.c"
#include "test_harness.h"

static void test_init_leaves_everything_unset(void) {
  CameraControls c;
  camera_ctrl_init(&c);
  CHECK(!camera_ctrl_any_set(&c));
  CHECK_EQ_INT(c.exposure, camera_ctrl_unset);
  CHECK_EQ_INT(c.brightness, camera_ctrl_unset);
  CHECK_EQ_INT(c.auto_exposure, camera_ctrl_unset);
}

// The distinction the sentinel exists for: "not mentioned" must not be
// confused with any value an operator could legitimately ask for. A plain
// 0 default would drive brightness to the middle of its range and
// contrast to its minimum on every config that omitted them.
static void test_zero_is_a_real_value_not_unset(void) {
  CameraControls c;
  camera_ctrl_init(&c);
  c.brightness = 0; // explicitly neutral, not "leave alone"
  CHECK(camera_ctrl_any_set(&c));
  CHECK(c.brightness != camera_ctrl_unset);

  camera_ctrl_init(&c);
  c.gain = 0; // explicitly minimum gain
  CHECK(camera_ctrl_any_set(&c));
}

static void test_any_set_detects_each_field(void) {
  const size_t n_fields = sizeof(CameraControls) / sizeof(int32_t);
  for (size_t i = 0; i < n_fields; i++) {
    CameraControls c;
    camera_ctrl_init(&c);
    CHECK(!camera_ctrl_any_set(&c));
    ((int32_t *)&c)[i] = 50;
    // Every field must count -- a new one added without updating
    // any_set() would silently never be applied.
    CHECK(camera_ctrl_any_set(&c));
  }
}

static void test_equal_detects_every_change(void) {
  CameraControls a, b;
  camera_ctrl_init(&a);
  camera_ctrl_init(&b);
  CHECK(camera_ctrl_equal(&a, &b));

  const size_t n_fields = sizeof(CameraControls) / sizeof(int32_t);
  for (size_t i = 0; i < n_fields; i++) {
    camera_ctrl_init(&b);
    ((int32_t *)&b)[i] = 7;
    // This gates whether a reloaded config re-issues ioctls; a field it
    // could not see would make that change silently never apply.
    CHECK(!camera_ctrl_equal(&a, &b));
  }
}

// Ranges are per-camera -- the dev webcam's brightness is -64..64, its
// contrast 0..100 -- so percent-style config values have to land on the
// right device numbers. These are the exact mappings verified live
// against that camera.
static void test_scaling_matches_real_device_ranges(void) {
  // brightness -100..100 onto -64..64
  CHECK_EQ_INT(scale_to_range(100, -100, 100, -64, 64), 64);
  CHECK_EQ_INT(scale_to_range(-100, -100, 100, -64, 64), -64);
  CHECK_EQ_INT(scale_to_range(0, -100, 100, -64, 64), 0);

  // contrast/saturation/sharpness 0..200 (100 neutral) onto 0..100
  CHECK_EQ_INT(scale_to_range(150, 0, 200, 0, 100), 75);
  CHECK_EQ_INT(scale_to_range(50, 0, 200, 0, 100), 25);
  CHECK_EQ_INT(scale_to_range(0, 0, 200, 0, 100), 0);
  CHECK_EQ_INT(scale_to_range(200, 0, 200, 0, 100), 100);

  // gain 0..100 onto the dev camera's 0..128
  CHECK_EQ_INT(scale_to_range(0, 0, 100, 0, 128), 0);
  CHECK_EQ_INT(scale_to_range(100, 0, 100, 0, 128), 128);
  CHECK_EQ_INT(scale_to_range(50, 0, 100, 0, 128), 64);
}

// Out-of-range input must clamp rather than run off the end of the
// device's range -- a config written for one camera should degrade
// sensibly on another, not produce a nonsense value.
static void test_scaling_clamps_out_of_range_input(void) {
  CHECK_EQ_INT(scale_to_range(500, 0, 200, 0, 100), 100);
  CHECK_EQ_INT(scale_to_range(-500, -100, 100, -64, 64), -64);
}

// A degenerate device range must not divide by zero.
static void test_scaling_survives_degenerate_ranges(void) {
  CHECK_EQ_INT(scale_to_range(50, 0, 0, 0, 100), 0);
  CHECK_EQ_INT(scale_to_range(50, 0, 100, 7, 7), 7);
}

// --- rpicam argument construction ------------------------------------

typedef struct {
  char *argv[32];
  char storage[512];
  int argc;
} RpicamArgs;

static void build(RpicamArgs *r, const CameraControls *c) {
  r->argc = camera_ctrl_rpicam_args(c, r->argv, 32, r->storage,
                                    sizeof r->storage);
}

// Finds a flag and returns its value, or NULL.
static const char *flag_value(const RpicamArgs *r, const char *flag) {
  for (int i = 0; i + 1 < r->argc; i++)
    if (strcmp(r->argv[i], flag) == 0)
      return r->argv[i + 1];
  return NULL;
}

static bool has_flag(const RpicamArgs *r, const char *flag) {
  for (int i = 0; i < r->argc; i++)
    if (strcmp(r->argv[i], flag) == 0)
      return true;
  return false;
}

static void test_unset_controls_produce_no_arguments(void) {
  CameraControls c;
  camera_ctrl_init(&c);
  RpicamArgs r;
  build(&r, &c);
  CHECK_EQ_INT(r.argc, 0); // nothing configured, nothing passed
}

// rpicam has no separate auto-exposure toggle: passing --shutter IS
// manual mode. So auto_exposure:false alone has nothing to send, and the
// exposure value is what actually gates the flag.
static void test_shutter_only_when_manual_with_a_value(void) {
  CameraControls c;
  camera_ctrl_init(&c);
  c.auto_exposure = 0;
  RpicamArgs r;
  build(&r, &c);
  CHECK(!has_flag(&r, "--shutter")); // manual, but no value to use

  camera_ctrl_init(&c);
  c.auto_exposure = 1;
  c.exposure = 20000;
  build(&r, &c);
  CHECK(!has_flag(&r, "--shutter")); // auto: --shutter would override it

  camera_ctrl_init(&c);
  c.auto_exposure = 0;
  c.exposure = 20000;
  build(&r, &c);
  const char *v = flag_value(&r, "--shutter");
  CHECK(v != NULL);
  // Microseconds pass through unconverted -- this is rpicam's own unit,
  // which is why the config uses it.
  if (v)
    CHECK(strcmp(v, "20000") == 0);
}

static void test_image_adjustments_convert_to_rpicam_floats(void) {
  CameraControls c;
  camera_ctrl_init(&c);
  c.brightness = 50;   // -100..100 -> -1.0..1.0
  c.contrast = 150;    // 0..200 with 100 neutral -> 1.5
  c.saturation = 0;    // -> 0.0, greyscale
  c.sharpness = 200;   // -> 2.0
  RpicamArgs r;
  build(&r, &c);
  const char *b = flag_value(&r, "--brightness");
  const char *ct = flag_value(&r, "--contrast");
  const char *s = flag_value(&r, "--saturation");
  const char *sh = flag_value(&r, "--sharpness");
  CHECK(b && strcmp(b, "0.50") == 0);
  CHECK(ct && strcmp(ct, "1.50") == 0);
  CHECK(s && strcmp(s, "0.00") == 0);
  CHECK(sh && strcmp(sh, "2.00") == 0);
}

// rpicam selects white balance by named mode rather than by Kelvin, so
// the config's temperature has to pick a sensible neighbour.
static void test_white_balance_maps_kelvin_to_modes(void) {
  const struct { int32_t kelvin; const char *mode; } cases[] = {
      {2800, "incandescent"}, {3500, "tungsten"}, {4500, "fluorescent"},
      {5200, "indoor"},       {6000, "daylight"}, {7000, "cloudy"},
  };
  for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
    CameraControls c;
    camera_ctrl_init(&c);
    c.auto_white_balance = 0;
    c.white_balance = cases[i].kelvin;
    RpicamArgs r;
    build(&r, &c);
    const char *mode = flag_value(&r, "--awb");
    CHECK(mode != NULL);
    if (mode)
      CHECK(strcmp(mode, cases[i].mode) == 0);
  }
}

static void test_auto_white_balance_wins_over_temperature(void) {
  CameraControls c;
  camera_ctrl_init(&c);
  c.auto_white_balance = 1;
  c.white_balance = 3000; // contradictory; auto is the explicit request
  RpicamArgs r;
  build(&r, &c);
  const char *mode = flag_value(&r, "--awb");
  CHECK(mode && strcmp(mode, "auto") == 0);
}

static void test_gain_maps_to_a_multiplier(void) {
  CameraControls c;
  camera_ctrl_init(&c);
  c.gain = 0;
  RpicamArgs r;
  build(&r, &c);
  const char *g = flag_value(&r, "--gain");
  CHECK(g && strcmp(g, "1.00") == 0); // 0% = unity, not zero gain

  camera_ctrl_init(&c);
  c.gain = 100;
  build(&r, &c);
  g = flag_value(&r, "--gain");
  CHECK(g && strcmp(g, "16.00") == 0);
}

// Every argv entry must be a flag/value pair, or rpicam-vid would be
// handed a malformed command line.
static void test_arguments_are_well_formed_pairs(void) {
  CameraControls c;
  camera_ctrl_init(&c);
  c.auto_exposure = 0;
  c.exposure = 15000;
  c.gain = 25;
  c.auto_white_balance = 0;
  c.white_balance = 5000;
  c.brightness = -20;
  c.contrast = 120;
  c.saturation = 80;
  c.sharpness = 100;
  RpicamArgs r;
  build(&r, &c);
  CHECK(r.argc > 0);
  CHECK_EQ_INT(r.argc % 2, 0);
  for (int i = 0; i < r.argc; i += 2) {
    CHECK(r.argv[i][0] == '-' && r.argv[i][1] == '-');
    CHECK(r.argv[i + 1] != NULL);
    CHECK(r.argv[i + 1][0] != '\0');
  }
}

// Truncating the argument list would hand the camera a different
// configuration than was asked for, so a too-small buffer must fail
// loudly rather than silently drop flags.
static void test_insufficient_buffers_fail_rather_than_truncate(void) {
  CameraControls c;
  camera_ctrl_init(&c);
  c.brightness = 10;
  c.contrast = 110;
  c.saturation = 90;

  char *argv[2];
  char storage[512];
  CHECK_EQ_INT(camera_ctrl_rpicam_args(&c, argv, 2, storage, sizeof storage),
               -1);

  char *argv_big[32];
  char tiny[4];
  CHECK_EQ_INT(camera_ctrl_rpicam_args(&c, argv_big, 32, tiny, sizeof tiny),
               -1);
}

int main(void) {
  printf("test_camera_ctrl:\n");
  RUN_TEST(test_init_leaves_everything_unset);
  RUN_TEST(test_zero_is_a_real_value_not_unset);
  RUN_TEST(test_any_set_detects_each_field);
  RUN_TEST(test_equal_detects_every_change);
  RUN_TEST(test_scaling_matches_real_device_ranges);
  RUN_TEST(test_scaling_clamps_out_of_range_input);
  RUN_TEST(test_scaling_survives_degenerate_ranges);
  RUN_TEST(test_unset_controls_produce_no_arguments);
  RUN_TEST(test_shutter_only_when_manual_with_a_value);
  RUN_TEST(test_image_adjustments_convert_to_rpicam_floats);
  RUN_TEST(test_white_balance_maps_kelvin_to_modes);
  RUN_TEST(test_auto_white_balance_wins_over_temperature);
  RUN_TEST(test_gain_maps_to_a_multiplier);
  RUN_TEST(test_arguments_are_well_formed_pairs);
  RUN_TEST(test_insufficient_buffers_fail_rather_than_truncate);
  TEST_MAIN_END();
}
