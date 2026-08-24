#pragma once
/*
 * Minimal assertion harness. No framework: these tests link against the
 * project's own translation units and run under meson test, and a
 * dependency would be one more thing to install on a Pi before the tests
 * can be trusted.
 *
 * Each test file defines tests as plain functions, registers them in
 * main() with RUN_TEST, and returns TEST_EXIT_CODE. A failing check
 * reports file/line and keeps going, so one run shows every failure
 * rather than only the first.
 */
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

static int tests_run = 0;
static int checks_failed = 0;
static int current_test_failures = 0;
static const char *current_test_name = "";

#define CHECK(cond)                                                            \
  do {                                                                         \
    if (!(cond)) {                                                             \
      printf("    FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);               \
      checks_failed++;                                                         \
      current_test_failures++;                                                 \
    }                                                                          \
  } while (0)

#define CHECK_EQ_INT(actual, expected)                                         \
  do {                                                                         \
    long long a_ = (long long)(actual), e_ = (long long)(expected);            \
    if (a_ != e_) {                                                            \
      printf("    FAIL %s:%d: %s == %s (got %lld, want %lld)\n", __FILE__,     \
             __LINE__, #actual, #expected, a_, e_);                            \
      checks_failed++;                                                         \
      current_test_failures++;                                                 \
    }                                                                          \
  } while (0)

// Reports the first differing index rather than just "buffers differ" --
// with image planes that difference is the entire diagnostic.
#define CHECK_MEM_EQ(actual, expected, n)                                      \
  do {                                                                         \
    const uint8_t *a_ = (const uint8_t *)(actual);                             \
    const uint8_t *e_ = (const uint8_t *)(expected);                           \
    size_t n_ = (size_t)(n);                                                    \
    size_t i_ = 0;                                                             \
    for (; i_ < n_; i_++)                                                      \
      if (a_[i_] != e_[i_])                                                    \
        break;                                                                 \
    if (i_ != n_) {                                                            \
      printf("    FAIL %s:%d: %s != %s at byte %zu (got %u, want %u)\n",       \
             __FILE__, __LINE__, #actual, #expected, i_, a_[i_], e_[i_]);      \
      checks_failed++;                                                         \
      current_test_failures++;                                                 \
    }                                                                          \
  } while (0)

#define RUN_TEST(fn)                                                           \
  do {                                                                         \
    current_test_name = #fn;                                                   \
    current_test_failures = 0;                                                 \
    tests_run++;                                                               \
    fn();                                                                      \
    printf("  %-4s %s\n", current_test_failures ? "FAIL" : "ok", #fn);         \
  } while (0)

#define TEST_MAIN_END()                                                        \
  do {                                                                         \
    printf("%s: %d tests, %d failed checks\n",                                 \
           checks_failed ? "FAILED" : "PASSED", tests_run, checks_failed);     \
    return checks_failed ? 1 : 0;                                              \
  } while (0)
