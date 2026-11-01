// The v4l2loopback output is optional: a missing or broken device must be
// retried on a schedule that keeps the log readable, and never stop the
// pipeline. These tests drive the retry policy against paths that cannot
// be a loopback device, so they need no kernel module.
//
// Set EEYE_TEST_LOOPBACK=/dev/videoN (a v4l2loopback device nothing else
// is writing to) to also write real frames to it.
#include "virtual_cam.c"
#include "test_harness.h"

static void test_empty_path_means_off(void) {
  CHECK(virtual_cam_create("", 640, 480) == NULL);
  CHECK(virtual_cam_create(NULL, 640, 480) == NULL);

  VideoFrame *f = vf_create(640, 480, 0);
  CHECK(!virtual_cam_write(NULL, f)); // NULL is a no-op, not a crash
  CHECK(!virtual_cam_connected(NULL));
  virtual_cam_destroy(NULL);
  vf_free(f);
}

// A device that does not exist is only stat()ed -- open_attempts stays 0,
// so nothing logs per attempt -- and is checked again no sooner than the
// retry interval.
static void test_missing_device_is_checked_quietly(void) {
  VirtualCam *vc = virtual_cam_create("/dev/eeye-test-no-such-video", 64, 32);
  CHECK(vc != NULL);
  VideoFrame *f = vf_create(64, 32, 0);

  CHECK(!virtual_cam_write(vc, f));
  CHECK(vc->explained_missing);
  CHECK_EQ_INT(vc->open_attempts, 0);
  int64_t due = vc->next_attempt_ms;
  CHECK(due >= now_ms() + retry_missing_ms - 100);

  // Immediately again: not due, so nothing changes.
  CHECK(!virtual_cam_write(vc, f));
  CHECK_EQ_INT(vc->next_attempt_ms, due);
  CHECK(!virtual_cam_connected(vc));

  vf_free(f);
  virtual_cam_destroy(vc);
}

// A node that exists but will not open (here: not a V4L2 device at all)
// backs off 2s, 4s, 8s ... up to 30s, since each attempt logs a reason.
static void test_unopenable_device_backs_off(void) {
  VirtualCam *vc = virtual_cam_create("/dev/null", 64, 32);
  VideoFrame *f = vf_create(64, 32, 0);

  int64_t expect_wait = retry_min_ms;
  for (int i = 0; i < 6; i++) {
    vc->next_attempt_ms = 0; // due now
    int64_t before = now_ms();
    CHECK(!virtual_cam_write(vc, f));
    CHECK_EQ_INT(vc->open_attempts, (unsigned)(i + 1));
    int64_t waited = vc->next_attempt_ms - before;
    CHECK(waited >= expect_wait - 50 && waited <= expect_wait + 50);
    expect_wait = expect_wait * 2 > retry_max_ms ? retry_max_ms
                                                 : expect_wait * 2;
  }
  CHECK_EQ_INT(vc->backoff_ms, retry_max_ms);

  // Not due: no further attempt.
  CHECK(!virtual_cam_write(vc, f));
  CHECK_EQ_INT(vc->open_attempts, 6);

  vf_free(f);
  virtual_cam_destroy(vc);
}

static void test_real_loopback_if_configured(void) {
  const char *dev = getenv("EEYE_TEST_LOOPBACK");
  if (!dev || !dev[0]) {
    printf("    (skipped: EEYE_TEST_LOOPBACK not set)\n");
    return;
  }
  VirtualCam *vc = virtual_cam_create(dev, 320, 240);
  VideoFrame *f = vf_create(320, 240, 0);
  memset(f->pixel_data, 128, f->plane_sizes[0] * 2);
  CHECK(virtual_cam_write(vc, f));
  CHECK(virtual_cam_connected(vc));
  CHECK(virtual_cam_write(vc, f));
  vf_free(f);
  virtual_cam_destroy(vc);
}

int main(void) {
  RUN_TEST(test_empty_path_means_off);
  RUN_TEST(test_missing_device_is_checked_quietly);
  RUN_TEST(test_unopenable_device_backs_off);
  RUN_TEST(test_real_loopback_if_configured);
  TEST_MAIN_END();
}
