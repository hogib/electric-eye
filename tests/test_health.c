// Health reporting: the file eeye writes so topside can tell a working
// recording from a stopped one. The failure this guards against is silent
// by nature -- a pilot believing they are recording when they are not --
// so the file has to be both correct and always parseable.
#define _POSIX_C_SOURCE 200809L
#include "health.c"
#include "test_harness.h"
#include <stdlib.h>
#include <sys/stat.h>

static const char *tmp_base(void) {
  static char base[256];
  if (base[0] == '\0') {
    const char *dir = getenv("TMPDIR");
    snprintf(base, sizeof base, "%s/eeye_health_test",
             dir && dir[0] ? dir : "/tmp");
  }
  return base;
}

static char *read_file(const char *path, size_t *len) {
  FILE *f = fopen(path, "rb");
  if (!f)
    return NULL;
  fseek(f, 0, SEEK_END);
  long n = ftell(f);
  fseek(f, 0, SEEK_SET);
  char *buf = malloc((size_t)n + 1);
  if (fread(buf, 1, (size_t)n, f) != (size_t)n) {
    fclose(f);
    free(buf);
    return NULL;
  }
  fclose(f);
  buf[n] = '\0';
  if (len)
    *len = (size_t)n;
  return buf;
}

// Publishes a snapshot and returns the file's contents.
static char *publish_and_read(const HealthSnapshot *snap) {
  char health_path[512];
  snprintf(health_path, sizeof health_path, "%s.health", tmp_base());
  unlink(health_path);
  health_publish(tmp_base(), snap);
  return read_file(health_path, NULL);
}

static void test_reports_recording_off(void) {
  HealthSnapshot s = {.recording_state = RECORDING_OFF,
                      .camera_connected = true,
                      .frame_width = 640,
                      .frame_height = 480};
  char *out = publish_and_read(&s);
  CHECK(out != NULL);
  if (!out) return;
  CHECK(strstr(out, "\"state\":\"off\"") != NULL);
  CHECK(strstr(out, "\"connected\":true") != NULL);
  free(out);
}

// The state that matters most: recording was requested and is running.
static void test_reports_recording_active(void) {
  HealthSnapshot s = {.recording_state = RECORDING_ACTIVE,
                      .recording_bytes = 12345678,
                      .disk_free_bytes = 999888777,
                      .camera_connected = true,
                      .frame_width = 1280,
                      .frame_height = 720};
  snprintf(s.recording_path, sizeof s.recording_path, "/data/dive.raw");
  char *out = publish_and_read(&s);
  CHECK(out != NULL);
  if (!out) return;
  CHECK(strstr(out, "\"state\":\"active\"") != NULL);
  CHECK(strstr(out, "\"path\":\"/data/dive.raw\"") != NULL);
  CHECK(strstr(out, "\"bytes\":12345678") != NULL);
  CHECK(strstr(out, "\"disk_free_bytes\":999888777") != NULL);
  free(out);
}

// ...and the one this whole mechanism exists for: it stopped, and the
// reason has to survive the trip to topside.
static void test_reports_recording_failure_with_reason(void) {
  HealthSnapshot s = {.recording_state = RECORDING_FAILED,
                      .camera_connected = true};
  snprintf(s.recording_path, sizeof s.recording_path, "/data/dive.raw");
  snprintf(s.recording_error, sizeof s.recording_error,
           "write failed: No space left on device");
  char *out = publish_and_read(&s);
  CHECK(out != NULL);
  if (!out) return;
  CHECK(strstr(out, "\"state\":\"failed\"") != NULL);
  CHECK(strstr(out, "No space left on device") != NULL);
  free(out);
}

// A path may legally contain a quote or a backslash. One unescaped byte
// makes the whole file unparseable -- and it would fail exactly when
// someone chose an awkward filename, not during testing.
static void test_awkward_paths_stay_parseable(void) {
  const char *nasty[] = {
      "/data/dive \"one\".raw",
      "/data/back\\slash.raw",
      "/data/tab\there.raw",
  };
  for (size_t i = 0; i < sizeof nasty / sizeof nasty[0]; i++) {
    HealthSnapshot s = {.recording_state = RECORDING_ACTIVE};
    snprintf(s.recording_path, sizeof s.recording_path, "%s", nasty[i]);
    char *out = publish_and_read(&s);
    CHECK(out != NULL);
    if (!out) continue;
    // Balanced quotes outside escapes is a cheap proxy for "still JSON":
    // an unescaped quote in the path breaks the count.
    int quotes = 0;
    for (const char *p = out; *p; p++) {
      if (*p == '\\') { p++; continue; }
      if (*p == '"') quotes++;
    }
    CHECK_EQ_INT(quotes % 2, 0);
    // Raw control characters must not appear literally.
    CHECK(strchr(out, '\t') == NULL);
    free(out);
  }
}

// Publishing repeatedly must overwrite cleanly rather than append or
// leave the previous state behind -- this runs once a second for a whole
// dive.
static void test_republish_replaces_previous(void) {
  HealthSnapshot active = {.recording_state = RECORDING_ACTIVE,
                           .recording_bytes = 500};
  snprintf(active.recording_path, sizeof active.recording_path, "/a.raw");
  free(publish_and_read(&active));

  HealthSnapshot off = {.recording_state = RECORDING_OFF};
  char *out = publish_and_read(&off);
  CHECK(out != NULL);
  if (!out) return;
  CHECK(strstr(out, "\"state\":\"off\"") != NULL);
  CHECK(strstr(out, "\"state\":\"active\"") == NULL); // no stale remnant
  CHECK(strstr(out, "/a.raw") == NULL);
  free(out);
}

// No temp file may be left behind: the recording directory is the one
// place a stray file costs real disk.
static void test_leaves_no_temp_file(void) {
  HealthSnapshot s = {.recording_state = RECORDING_OFF};
  free(publish_and_read(&s));
  char tmp_path[512];
  snprintf(tmp_path, sizeof tmp_path, "%s.health.tmp", tmp_base());
  struct stat st;
  CHECK(stat(tmp_path, &st) != 0);
}

// Health reporting must never be able to take down the pipeline it
// reports on, so an unwritable location is a silent no-op.
static void test_unwritable_path_is_not_fatal(void) {
  HealthSnapshot s = {.recording_state = RECORDING_OFF};
  health_publish("/proc/nonexistent-dir/eeye_config.json", &s);
  // Reaching here without crashing is the assertion.
}

static void test_disk_free_reports_something(void) {
  // An existing directory: must give a plausible answer.
  uint64_t free_bytes = health_disk_free("/tmp");
  CHECK(free_bytes > 0);

  // A file that does not exist yet -- the normal case when checking
  // before starting a recording. Must fall back to the parent directory
  // rather than giving up.
  uint64_t via_parent = health_disk_free("/tmp/definitely-not-here.raw");
  CHECK(via_parent > 0);

  // Nonsense input must be 0, not garbage.
  CHECK_EQ_INT(health_disk_free(""), 0);
  CHECK_EQ_INT(health_disk_free(NULL), 0);
  CHECK_EQ_INT(health_disk_free("/no/such/dir/at/all/file.raw"), 0);
}

int main(void) {
  printf("test_health:\n");
  RUN_TEST(test_reports_recording_off);
  RUN_TEST(test_reports_recording_active);
  RUN_TEST(test_reports_recording_failure_with_reason);
  RUN_TEST(test_awkward_paths_stay_parseable);
  RUN_TEST(test_republish_replaces_previous);
  RUN_TEST(test_leaves_no_temp_file);
  RUN_TEST(test_unwritable_path_is_not_fatal);
  RUN_TEST(test_disk_free_reports_something);
  TEST_MAIN_END();
}
