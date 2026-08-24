// Includes the translation unit directly so the static parser and its
// validation are reachable. parse_config() is where every operator-facing
// config error is decided, and it is pure (buffer in, Config out), so it
// is both the highest-value and the easiest thing here to test.
#define _POSIX_C_SOURCE 200809L
#include "config.c"
#include "test_harness.h"

static bool parse(const char *json, Config *out) {
  *out = (Config){0};
  return parse_config(json, strlen(json), out);
}

// Every key omitted must yield exactly config_defaults -- the README
// documents these values, and a drift between the two is invisible until
// someone relies on an undocumented default.
static void test_defaults_match_documentation(void) {
  Config c;
  CHECK(parse("{}", &c));
  CHECK_EQ_INT(c.capture_width, 1280);
  CHECK_EQ_INT(c.capture_height, 720);
  CHECK_EQ_INT(c.downscale, 1);
  CHECK_EQ_INT(c.stream_frame_interval, 0);
  CHECK_EQ_INT(c.stream_quality, 60);
  CHECK_EQ_INT(c.capture_source, CAPTURE_AUTO);
  CHECK_EQ_INT(c.record_path[0], '\0');
  // An omitted chain keeps the built-in single-stage sobel default.
  CHECK_EQ_INT(c.stage_count, 1);
  CHECK_EQ_INT(c.stages[0].effect, EFFECT_SOBEL);
}

static void test_empty_chain_is_valid_passthrough(void) {
  Config c;
  CHECK(parse("{\"chain\":[]}", &c));
  CHECK_EQ_INT(c.stage_count, 0);
}

static void test_chain_order_is_preserved(void) {
  Config c;
  // Order is semantic -- blur-then-sobel differs from sobel-then-blur --
  // so a parser that reordered or deduplicated stages would silently
  // change the image.
  CHECK(parse("{\"chain\":[{\"effect\":\"blur\"},{\"effect\":\"sobel\"},"
              "{\"effect\":\"invert\"}]}",
              &c));
  CHECK_EQ_INT(c.stage_count, 3);
  CHECK_EQ_INT(c.stages[0].effect, EFFECT_BLUR);
  CHECK_EQ_INT(c.stages[1].effect, EFFECT_SOBEL);
  CHECK_EQ_INT(c.stages[2].effect, EFFECT_INVERT);
}

static void test_every_effect_name_parses(void) {
  const struct { const char *name; EffectType type; } names[] = {
      {"none", EFFECT_NONE},           {"grayscale", EFFECT_GRAYSCALE},
      {"invert", EFFECT_INVERT},       {"threshold", EFFECT_THRESHOLD},
      {"tint", EFFECT_TINT},           {"sobel", EFFECT_SOBEL},
      {"blur", EFFECT_BLUR},           {"contrast", EFFECT_CONTRAST},
      {"light", EFFECT_LIGHT},         {"log", EFFECT_LOG},
      {"canny", EFFECT_CANNY},
  };
  for (size_t i = 0; i < sizeof names / sizeof names[0]; i++) {
    char json[128];
    snprintf(json, sizeof json, "{\"chain\":[{\"effect\":\"%s\"}]}",
             names[i].name);
    Config c;
    CHECK(parse(json, &c));
    CHECK_EQ_INT(c.stages[0].effect, names[i].type);
  }
}

// light_level is the one field that does NOT default to 0: zero would mean
// "fully dark and desaturated", so an omitted value has to come back
// neutral. Every other parameter's zero-init already means "no change".
static void test_light_level_defaults_neutral(void) {
  Config c;
  CHECK(parse("{\"chain\":[{\"effect\":\"light\"}]}", &c));
  CHECK_EQ_INT(c.stages[0].light_level, 128);

  CHECK(parse("{\"chain\":[{\"effect\":\"light\",\"light_level\":0}]}", &c));
  CHECK_EQ_INT(c.stages[0].light_level, 0); // explicit 0 must survive

  // Other params keep their zero default.
  CHECK(parse("{\"chain\":[{\"effect\":\"blur\"}]}", &c));
  CHECK_EQ_INT(c.stages[0].blur_strength, 0);
}

static void test_effect_parameters_round_trip(void) {
  Config c;
  CHECK(parse("{\"chain\":[{\"effect\":\"tint\",\"tint_u\":90,\"tint_v\":150,"
              "\"tint_strength\":180}]}",
              &c));
  CHECK_EQ_INT(c.stages[0].tint_u, 90);
  CHECK_EQ_INT(c.stages[0].tint_v, 150);
  CHECK_EQ_INT(c.stages[0].tint_strength, 180);

  CHECK(parse("{\"chain\":[{\"effect\":\"threshold\",\"threshold_value\":200}]}",
              &c));
  CHECK_EQ_INT(c.stages[0].threshold_value, 200);

  CHECK(parse("{\"chain\":[{\"effect\":\"sobel\",\"sobel_threshold\":60}]}", &c));
  CHECK_EQ_INT(c.stages[0].sobel_threshold, 60);

  CHECK(parse("{\"chain\":[{\"effect\":\"log\",\"log_strength\":3,"
              "\"log_threshold\":25}]}",
              &c));
  CHECK_EQ_INT(c.stages[0].log_strength, 3);
  CHECK_EQ_INT(c.stages[0].log_threshold, 25);
}

// stream_raw is the first bool-valued key. A bool parser that accepted
// numbers, or silently defaulted a malformed value to false, would turn a
// typo into "the raw toggle just doesn't work".
static void test_stream_raw_bool_parsing(void) {
  Config c;
  CHECK(parse("{}", &c));
  CHECK(c.stream_raw == false); // default
  CHECK(parse("{\"stream_raw\":true}", &c));
  CHECK(c.stream_raw == true);
  CHECK(parse("{\"stream_raw\":false}", &c));
  CHECK(c.stream_raw == false);
  // Anything that isn't a JSON bool must be rejected, not coerced.
  CHECK(!parse("{\"stream_raw\":1}", &c));
  CHECK(!parse("{\"stream_raw\":0}", &c));
  CHECK(!parse("{\"stream_raw\":\"true\"}", &c));
  CHECK(!parse("{\"stream_raw\":TRUE}", &c));
  CHECK(!parse("{\"stream_raw\":truthy}", &c));
  // It is hot-reloadable, so it must coexist with the rest of a config.
  CHECK(parse("{\"chain\":[{\"effect\":\"sobel\"}],\"stream_raw\":true,"
              "\"stream_frame_interval\":3}",
              &c));
  CHECK(c.stream_raw == true);
  CHECK_EQ_INT(c.stages[0].effect, EFFECT_SOBEL);
}

// canny's thresholds are seeded to usable defaults rather than 0, which
// would make it emit an empty map -- the same special-casing light_level
// gets, and equally easy to lose.
static void test_canny_threshold_defaults(void) {
  Config c;
  CHECK(parse("{\"chain\":[{\"effect\":\"canny\"}]}", &c));
  CHECK_EQ_INT(c.stages[0].canny_low, 40);
  CHECK_EQ_INT(c.stages[0].canny_high, 90);
  CHECK_EQ_INT(c.stages[0].canny_strength, 0); // 0 means 1 pass, see conv.h
  // Explicit values must survive, including an explicit 0.
  CHECK(parse("{\"chain\":[{\"effect\":\"canny\",\"canny_low\":0,"
              "\"canny_high\":255,\"canny_strength\":3}]}",
              &c));
  CHECK_EQ_INT(c.stages[0].canny_low, 0);
  CHECK_EQ_INT(c.stages[0].canny_high, 255);
  CHECK_EQ_INT(c.stages[0].canny_strength, 3);
  // Cross-effect parameters stay rejected.
  CHECK(!parse("{\"chain\":[{\"effect\":\"canny\",\"log_strength\":2}]}",
               &c));
  CHECK(!parse("{\"chain\":[{\"effect\":\"log\",\"canny_low\":20}]}", &c));
}

// log takes its own two parameters and no others -- the per-effect key
// check has to know about a newly added effect, or it silently accepts
// anything on it.
static void test_log_parameter_validation(void) {
  Config c;
  CHECK(parse("{\"chain\":[{\"effect\":\"log\"}]}", &c)); // both optional
  CHECK_EQ_INT(c.stages[0].log_strength, 0);
  CHECK_EQ_INT(c.stages[0].log_threshold, 0);
  // Another effect's parameter on a log stage is an error...
  CHECK(!parse("{\"chain\":[{\"effect\":\"log\",\"tint_u\":90}]}", &c));
  CHECK(!parse("{\"chain\":[{\"effect\":\"log\",\"blur_strength\":4}]}",
               &c));
  // ...and log's parameters on another effect are equally an error.
  CHECK(!parse("{\"chain\":[{\"effect\":\"blur\",\"log_strength\":3}]}",
               &c));
  CHECK(!parse("{\"chain\":[{\"effect\":\"sobel\",\"log_threshold\":3}]}",
               &c));
}

// The README documents both presets verbatim; they must parse as printed.
// An earlier README showed them with trailing "// sepia" comments, which
// this parser rejects -- so anyone copy-pasting got an error.
static void test_readme_tint_presets_parse(void) {
  Config c;
  CHECK(parse("{\"chain\":[{\"effect\": \"tint\", \"tint_u\": 90, "
              "\"tint_v\": 150, \"tint_strength\": 180}]}",
              &c));
  CHECK(parse("{\"chain\":[{\"effect\": \"tint\", \"tint_u\": 190, "
              "\"tint_v\": 100, \"tint_strength\": 140}]}",
              &c));
}

// A typo must be loud. Silently ignoring an unknown key is how someone
// spends an hour wondering why a setting does nothing.
static void test_unknown_keys_are_hard_errors(void) {
  Config c;
  CHECK(!parse("{\"bogus_key\":1}", &c));
  CHECK(!parse("{\"chain\":[],\"downscal\":2}", &c)); // plausible typo
  // A parameter on an effect that doesn't take it is equally an error.
  CHECK(!parse("{\"chain\":[{\"effect\":\"blur\",\"tint_u\":90}]}", &c));
  CHECK(!parse("{\"chain\":[{\"effect\":\"nosucheffect\"}]}", &c));
}

static void test_malformed_json_is_rejected(void) {
  Config c;
  CHECK(!parse("", &c));
  CHECK(!parse("[]", &c));                       // not an object
  CHECK(!parse("{\"chain\":[]", &c));            // unterminated
  CHECK(!parse("{\"chain\":[]} trailing", &c));  // trailing data
  CHECK(!parse("{\"chain\":[]},", &c));
  CHECK(!parse("{chain:[]}", &c));               // unquoted key
  CHECK(!parse("{\"chain\" []}", &c));           // missing colon
  CHECK(!parse("{\"chain\":[{\"effect\":\"blur\"}", &c));
}

// Values are parsed as bytes; anything outside 0-255 must be rejected
// rather than silently truncated into a valid-looking setting.
static void test_out_of_range_values_rejected(void) {
  Config c;
  CHECK(!parse("{\"chain\":[{\"effect\":\"blur\",\"blur_strength\":256}]}", &c));
  CHECK(!parse("{\"chain\":[{\"effect\":\"blur\",\"blur_strength\":-1}]}", &c));
  CHECK(!parse("{\"stream_quality\":999}", &c));
  // 255 is the boundary and must still be accepted.
  CHECK(parse("{\"chain\":[{\"effect\":\"blur\",\"blur_strength\":255}]}", &c));
}

static void test_downscale_restricted_to_powers(void) {
  Config c;
  const int valid[] = {1, 2, 4, 8};
  for (size_t i = 0; i < 4; i++) {
    char json[128];
    // 1280x720 divides evenly by all four.
    snprintf(json, sizeof json,
             "{\"capture_width\":1280,\"capture_height\":720,\"downscale\":%d}",
             valid[i]);
    CHECK(parse(json, &c));
    CHECK_EQ_INT(c.downscale, valid[i]);
  }
  const int invalid[] = {0, 3, 5, 6, 7, 16};
  for (size_t i = 0; i < 6; i++) {
    char json[128];
    snprintf(json, sizeof json,
             "{\"capture_width\":1280,\"capture_height\":720,\"downscale\":%d}",
             invalid[i]);
    CHECK(!parse(json, &c));
  }
}

// Both dimensions must be a multiple of 2*downscale. Width, so the I422
// chroma planes stay exactly half-width; height, because a 4:2:0 source
// (what rpicam-vid emits) has half-height chroma that gets line-doubled
// into full-height planes, which only lands exactly on an even height.
//
// The height half of that rule changed when 4:2:0 support landed -- it
// used to be a multiple of downscale alone -- so the cases below
// deliberately include heights divisible by downscale but NOT by
// 2*downscale, which is exactly what the old rule let through.
static void test_geometry_divisibility_rule(void) {
  Config c;
  const struct { int w, h, ds; bool ok; } cases[] = {
      {1280, 720, 1, true},   {1280, 720, 2, true},
      {1280, 720, 4, true},   {640, 480, 2, true},
      {641, 480, 1, false},   // odd width
      {640, 481, 1, false},   // odd height
      {1280, 724, 4, false},  // 724 % 4 == 0 but 724 % 8 != 0
      {640, 482, 2, false},   // 482 % 2 == 0 but 482 % 4 != 0
      {1284, 720, 4, false},  // 1284 % 4 == 0 but 1284 % 8 != 0
      {1280, 728, 4, true},   // both multiples of 8
      {0, 480, 1, false},     // zero dimensions
      {640, 0, 1, false},
  };
  for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
    char json[160];
    snprintf(json, sizeof json,
             "{\"capture_width\":%d,\"capture_height\":%d,\"downscale\":%d}",
             cases[i].w, cases[i].h, cases[i].ds);
    bool got = parse(json, &c);
    if (got != cases[i].ok) {
      printf("    (geometry %dx%d ds=%d: got %s, want %s)\n", cases[i].w,
             cases[i].h, cases[i].ds, got ? "accept" : "reject",
             cases[i].ok ? "accept" : "reject");
    }
    CHECK(got == cases[i].ok);
  }
}

static void test_capture_source_values(void) {
  Config c;
  CHECK(parse("{\"capture_source\":\"auto\"}", &c));
  CHECK_EQ_INT(c.capture_source, CAPTURE_AUTO);
  CHECK(parse("{\"capture_source\":\"v4l2\"}", &c));
  CHECK_EQ_INT(c.capture_source, CAPTURE_V4L2);
  CHECK(parse("{\"capture_source\":\"rpicam\"}", &c));
  CHECK_EQ_INT(c.capture_source, CAPTURE_RPICAM);
  CHECK(!parse("{\"capture_source\":\"bogus\"}", &c));
  CHECK(!parse("{\"capture_source\":2}", &c)); // not a string
}

static void test_record_path_round_trips(void) {
  Config c;
  CHECK(parse("{\"record_path\":\"/tmp/out.raw\"}", &c));
  CHECK(strcmp(c.record_path, "/tmp/out.raw") == 0);
  CHECK(parse("{\"record_path\":\"\"}", &c));
  CHECK_EQ_INT(c.record_path[0], '\0');
}

// Config is copied by value into a fixed-size ring, so an overlong path or
// an overlong chain must be refused rather than truncated or overflowed.
static void test_oversized_inputs_rejected(void) {
  Config c;
  char json[1024];
  char long_path[max_record_path_len + 64];
  memset(long_path, 'a', sizeof long_path - 1);
  long_path[sizeof long_path - 1] = '\0';
  snprintf(json, sizeof json, "{\"record_path\":\"%s\"}", long_path);
  CHECK(!parse(json, &c));

  // One stage past the cap.
  char chain[2048] = "{\"chain\":[";
  for (size_t i = 0; i < max_chain_stages + 1; i++)
    strcat(chain, i ? ",{\"effect\":\"none\"}" : "{\"effect\":\"none\"}");
  strcat(chain, "]}");
  CHECK(!parse(chain, &c));
}

// Exactly max_chain_stages must still be accepted -- an off-by-one here
// would reject a legal config.
static void test_max_chain_length_accepted(void) {
  Config c;
  char chain[2048] = "{\"chain\":[";
  for (size_t i = 0; i < max_chain_stages; i++)
    strcat(chain, i ? ",{\"effect\":\"none\"}" : "{\"effect\":\"none\"}");
  strcat(chain, "]}");
  CHECK(parse(chain, &c));
  CHECK_EQ_INT(c.stage_count, max_chain_stages);
}

static void test_output_geometry_division(void) {
  Config c = config_defaults;
  uint32_t w, h;
  c.capture_width = 1280; c.capture_height = 720; c.downscale = 2;
  config_output_geometry(&c, &w, &h);
  CHECK_EQ_INT(w, 640);
  CHECK_EQ_INT(h, 360);

  c.downscale = 8;
  config_output_geometry(&c, &w, &h);
  CHECK_EQ_INT(w, 160);
  CHECK_EQ_INT(h, 90);

  // A zero downscale must not divide by zero -- treated as 1.
  c.downscale = 0;
  config_output_geometry(&c, &w, &h);
  CHECK_EQ_INT(w, 1280);
  CHECK_EQ_INT(h, 720);
}

// Whitespace and key order must not matter; JSON has no ordering rules and
// a hand-edited config in the field will not be formatted consistently.
static void test_whitespace_and_key_order(void) {
  Config c;
  CHECK(parse("  {\n  \"downscale\" : 2 ,\n \"capture_width\" : 640,\n"
              " \"capture_height\": 480\n}  \n",
              &c));
  CHECK_EQ_INT(c.downscale, 2);
  CHECK_EQ_INT(c.capture_width, 640);
}

int main(void) {
  printf("test_config:\n");
  RUN_TEST(test_defaults_match_documentation);
  RUN_TEST(test_empty_chain_is_valid_passthrough);
  RUN_TEST(test_chain_order_is_preserved);
  RUN_TEST(test_every_effect_name_parses);
  RUN_TEST(test_light_level_defaults_neutral);
  RUN_TEST(test_effect_parameters_round_trip);
  RUN_TEST(test_log_parameter_validation);
  RUN_TEST(test_canny_threshold_defaults);
  RUN_TEST(test_stream_raw_bool_parsing);
  RUN_TEST(test_readme_tint_presets_parse);
  RUN_TEST(test_unknown_keys_are_hard_errors);
  RUN_TEST(test_malformed_json_is_rejected);
  RUN_TEST(test_out_of_range_values_rejected);
  RUN_TEST(test_downscale_restricted_to_powers);
  RUN_TEST(test_geometry_divisibility_rule);
  RUN_TEST(test_capture_source_values);
  RUN_TEST(test_record_path_round_trips);
  RUN_TEST(test_oversized_inputs_rejected);
  RUN_TEST(test_max_chain_length_accepted);
  RUN_TEST(test_output_geometry_division);
  RUN_TEST(test_whitespace_and_key_order);
  TEST_MAIN_END();
}
