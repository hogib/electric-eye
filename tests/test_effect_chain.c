// Consecutive point ops are fused into one LUT per channel instead of one
// full-frame pass per stage. That optimization is invisible when it works
// and silently corrupts every frame when it doesn't, so it is checked here
// against an independently written reference that applies each stage
// separately, the naive way.
//
// The reference below is deliberately NOT a refactor of the production
// code -- it is transcribed from each effect's documented definition, so
// the two agreeing means the fusion algebra is right, not merely
// self-consistent.
#define _POSIX_C_SOURCE 200809L
#include "effect_chain.c"
#include "test_harness.h"
#include <stdlib.h>

// --- Independent reference ------------------------------------------

static void ref_stage(uint8_t *y, uint8_t *u, uint8_t *v, size_t n_y,
                      size_t n_c, const EffectStage *s) {
  switch (s->effect) {
  case EFFECT_GRAYSCALE:
    memset(u, 128, n_c);
    memset(v, 128, n_c);
    break;
  case EFFECT_INVERT:
    for (size_t i = 0; i < n_y; i++)
      y[i] = (uint8_t)(255 - y[i]);
    break;
  case EFFECT_THRESHOLD:
    for (size_t i = 0; i < n_y; i++)
      y[i] = y[i] < s->threshold_value ? 0 : 255;
    break;
  case EFFECT_TINT:
    if (s->tint_strength != 0) {
      for (size_t i = 0; i < n_c; i++) {
        int32_t du = (int32_t)s->tint_u - (int32_t)u[i];
        u[i] = (uint8_t)(u[i] + (du * s->tint_strength) / 255);
        int32_t dv = (int32_t)s->tint_v - (int32_t)v[i];
        v[i] = (uint8_t)(v[i] + (dv * s->tint_strength) / 255);
      }
    }
    break;
  case EFFECT_LIGHT:
    if (s->light_level != 128) {
      int32_t off = (int32_t)s->light_level - 128;
      float scale = (float)s->light_level / 128.0f;
      for (size_t i = 0; i < n_y; i++) {
        int32_t ny = (int32_t)y[i] + off;
        y[i] = (uint8_t)(ny < 0 ? 0 : ny > 255 ? 255 : ny);
      }
      for (size_t i = 0; i < n_c; i++) {
        float nu = 128.0f + ((float)u[i] - 128.0f) * scale;
        u[i] = (uint8_t)((nu < 0 ? 0 : nu > 255 ? 255 : nu) + 0.5f);
        float nv = 128.0f + ((float)v[i] - 128.0f) * scale;
        v[i] = (uint8_t)((nv < 0 ? 0 : nv > 255 ? 255 : nv) + 0.5f);
      }
    }
    break;
  default:
    break;
  }
}

// --- Fixtures --------------------------------------------------------

static VideoFrame *make_frame(uint32_t w, uint32_t h, unsigned seed) {
  VideoFrame *f = vf_create(w, h, 0);
  // Deterministic pseudo-random content covering the full 0-255 range, so
  // clamping at both ends is exercised.
  unsigned s = seed;
  for (size_t i = 0; i < f->plane_sizes[0]; i++) {
    s = s * 1103515245u + 12345u;
    f->raw_planes[0][i] = (uint8_t)(s >> 16);
  }
  for (size_t i = 0; i < f->plane_sizes[1]; i++) {
    s = s * 1103515245u + 12345u;
    f->raw_planes[1][i] = (uint8_t)(s >> 16);
    s = s * 1103515245u + 12345u;
    f->raw_planes[2][i] = (uint8_t)(s >> 16);
  }
  return f;
}

// Runs a chain through the production path and through the reference, and
// requires the two to agree byte for byte.
static void compare_against_reference(const EffectStage *stages,
                                      size_t count, unsigned seed) {
  VideoFrame *frame = make_frame(64, 32, seed);

  Config cfg = {0};
  cfg.stage_count = count;
  for (size_t i = 0; i < count; i++)
    cfg.stages[i] = stages[i];

  // Reference: start from the same raw frame, apply each stage in turn.
  size_t n_y = frame->plane_sizes[0], n_c = frame->plane_sizes[1];
  uint8_t *ry = malloc(n_y), *ru = malloc(n_c), *rv = malloc(n_c);
  memcpy(ry, frame->raw_planes[0], n_y);
  memcpy(ru, frame->raw_planes[1], n_c);
  memcpy(rv, frame->raw_planes[2], n_c);
  for (size_t i = 0; i < count; i++)
    ref_stage(ry, ru, rv, n_y, n_c, &cfg.stages[i]);

  apply_effect_chain(frame, &cfg);

  CHECK_MEM_EQ(frame->planes[0], ry, n_y);
  CHECK_MEM_EQ(frame->planes[1], ru, n_c);
  CHECK_MEM_EQ(frame->planes[2], rv, n_c);

  free(ry); free(ru); free(rv);
  vf_free(frame);
}

#define STAGES(...) ((const EffectStage[]){__VA_ARGS__})

// A single point op must match its own definition -- the base case before
// any fusion is involved.
static void test_single_point_ops(void) {
  compare_against_reference(STAGES({.effect = EFFECT_INVERT}), 1, 1);
  compare_against_reference(STAGES({.effect = EFFECT_GRAYSCALE}), 1, 2);
  compare_against_reference(
      STAGES({.effect = EFFECT_THRESHOLD, .threshold_value = 128}), 1, 3);
  compare_against_reference(STAGES({.effect = EFFECT_TINT, .tint_u = 90,
                                    .tint_v = 150, .tint_strength = 180}),
                            1, 4);
  compare_against_reference(
      STAGES({.effect = EFFECT_LIGHT, .light_level = 200}), 1, 5);
}

// Two fused stages must equal the two applied separately. Order matters:
// invert-then-threshold differs from threshold-then-invert, and a fusion
// that composed in the wrong direction would pass a symmetric test.
static void test_two_stage_fusion_respects_order(void) {
  compare_against_reference(
      STAGES({.effect = EFFECT_INVERT},
             {.effect = EFFECT_THRESHOLD, .threshold_value = 100}),
      2, 10);
  compare_against_reference(
      STAGES({.effect = EFFECT_THRESHOLD, .threshold_value = 100},
             {.effect = EFFECT_INVERT}),
      2, 11);
}

// Grayscale overwrites chroma unconditionally, so a tint before it must be
// discarded while a tint after it must survive. This is the case where a
// naive "compose everything" fusion goes wrong.
static void test_grayscale_discards_earlier_chroma_work(void) {
  compare_against_reference(
      STAGES({.effect = EFFECT_TINT, .tint_u = 90, .tint_v = 150,
              .tint_strength = 200},
             {.effect = EFFECT_GRAYSCALE}),
      2, 20);
  compare_against_reference(
      STAGES({.effect = EFFECT_GRAYSCALE},
             {.effect = EFFECT_TINT, .tint_u = 90, .tint_v = 150,
              .tint_strength = 200}),
      2, 21);
}

// Long runs of every point op, in both directions, so composition errors
// that only show up after several stages are caught.
static void test_long_fused_runs(void) {
  compare_against_reference(
      STAGES({.effect = EFFECT_LIGHT, .light_level = 160},
             {.effect = EFFECT_TINT, .tint_u = 100, .tint_v = 140,
              .tint_strength = 120},
             {.effect = EFFECT_INVERT},
             {.effect = EFFECT_THRESHOLD, .threshold_value = 90},
             {.effect = EFFECT_LIGHT, .light_level = 90}),
      5, 30);
  compare_against_reference(
      STAGES({.effect = EFFECT_THRESHOLD, .threshold_value = 60},
             {.effect = EFFECT_LIGHT, .light_level = 200},
             {.effect = EFFECT_GRAYSCALE},
             {.effect = EFFECT_INVERT},
             {.effect = EFFECT_TINT, .tint_u = 200, .tint_v = 60,
              .tint_strength = 255}),
      5, 31);
}

// No-op parameter values must genuinely change nothing: tint_strength 0
// and light_level 128 are documented as identity, and are special-cased in
// the fusion code (which is exactly where an identity can be got wrong).
static void test_documented_noop_parameters(void) {
  compare_against_reference(
      STAGES({.effect = EFFECT_TINT, .tint_u = 200, .tint_v = 50,
              .tint_strength = 0}),
      1, 40);
  compare_against_reference(
      STAGES({.effect = EFFECT_LIGHT, .light_level = 128}), 1, 41);
  compare_against_reference(STAGES({.effect = EFFECT_NONE}), 1, 42);
}

// Extremes of every parameter, where clamping at 0 and 255 happens.
static void test_parameter_extremes(void) {
  compare_against_reference(
      STAGES({.effect = EFFECT_LIGHT, .light_level = 0}), 1, 50);
  compare_against_reference(
      STAGES({.effect = EFFECT_LIGHT, .light_level = 255}), 1, 51);
  compare_against_reference(
      STAGES({.effect = EFFECT_TINT, .tint_u = 0, .tint_v = 255,
              .tint_strength = 255}),
      1, 52);
  compare_against_reference(
      STAGES({.effect = EFFECT_THRESHOLD, .threshold_value = 0}), 1, 53);
  compare_against_reference(
      STAGES({.effect = EFFECT_THRESHOLD, .threshold_value = 255}), 1, 54);
}

// An empty chain is a valid pass-through: the output must be the untouched
// camera frame, not whatever happened to be in the work buffer.
static void test_empty_chain_passes_frame_through(void) {
  VideoFrame *frame = make_frame(64, 32, 99);
  uint8_t *orig = malloc(frame->plane_sizes[0]);
  memcpy(orig, frame->raw_planes[0], frame->plane_sizes[0]);
  // Poison work so a missing copy is visible rather than coincidentally right.
  memset(frame->planes[0], 0x5A, frame->plane_sizes[0]);

  Config cfg = {0};
  cfg.stage_count = 0;
  apply_effect_chain(frame, &cfg);

  CHECK_MEM_EQ(frame->planes[0], orig, frame->plane_sizes[0]);
  free(orig);
  vf_free(frame);
}

// A neighborhood op between two point-op runs breaks fusion into separate
// runs and forces the raw/work/spare ping-pong. The point ops on each side
// must still compose correctly across that boundary.
static void test_point_ops_around_neighborhood_op(void) {
  VideoFrame *frame = make_frame(64, 32, 77);
  Config cfg = {0};
  cfg.stage_count = 3;
  cfg.stages[0] = (EffectStage){.effect = EFFECT_INVERT};
  cfg.stages[1] = (EffectStage){.effect = EFFECT_SOBEL, .sobel_threshold = 40};
  cfg.stages[2] = (EffectStage){.effect = EFFECT_THRESHOLD,
                                .threshold_value = 100};

  apply_effect_chain(frame, &cfg);

  // Sobel then threshold yields a binary image; the assertion is that the
  // chain ran to completion and left a valid result in planes[] rather
  // than a half-written or stale buffer.
  for (size_t i = 0; i < frame->plane_sizes[0]; i++) {
    uint8_t v = frame->planes[0][i];
    CHECK(v == 0 || v == 255);
  }
  // Chroma is neutralized by sobel and untouched by threshold.
  for (size_t i = 0; i < frame->plane_sizes[1]; i++)
    CHECK_EQ_INT(frame->planes[1][i], 128);
  vf_free(frame);
}

// The chain must always leave its result in frame->planes -- the rest of
// the pipeline reads nothing else. Two neighborhood ops leave the result
// in `spare` mid-chain, so the final copy-back is what makes this hold.
static void test_result_always_lands_in_work_buffer(void) {
  VideoFrame *frame = make_frame(64, 32, 88);
  Config cfg = {0};
  cfg.stage_count = 2;
  cfg.stages[0] = (EffectStage){.effect = EFFECT_BLUR, .blur_strength = 1};
  cfg.stages[1] = (EffectStage){.effect = EFFECT_SOBEL};

  memset(frame->planes[0], 0xC3, frame->plane_sizes[0]); // poison
  apply_effect_chain(frame, &cfg);

  size_t poisoned = 0;
  for (size_t i = 0; i < frame->plane_sizes[0]; i++)
    if (frame->planes[0][i] == 0xC3)
      poisoned++;
  // A fully-poisoned plane would mean nothing was copied back.
  CHECK(poisoned < frame->plane_sizes[0]);
  vf_free(frame);
}

int main(void) {
  printf("test_effect_chain:\n");
  RUN_TEST(test_single_point_ops);
  RUN_TEST(test_two_stage_fusion_respects_order);
  RUN_TEST(test_grayscale_discards_earlier_chroma_work);
  RUN_TEST(test_long_fused_runs);
  RUN_TEST(test_documented_noop_parameters);
  RUN_TEST(test_parameter_extremes);
  RUN_TEST(test_empty_chain_passes_frame_through);
  RUN_TEST(test_point_ops_around_neighborhood_op);
  RUN_TEST(test_result_always_lands_in_work_buffer);
  TEST_MAIN_END();
}
