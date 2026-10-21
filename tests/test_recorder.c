// The recording tap moved off consumer_loop's thread so that a slow disk
// costs recorded frames instead of the live view. What has to hold:
// frames reach the file intact and in order, every frame handed over comes
// back to the pool, and a stalled disk never makes recorder_submit block.
//
// The stalled disk is a FIFO: fopen() on one blocks until a reader shows
// up, which is exactly a write path that has stopped draining.
#define _POSIX_C_SOURCE 200809L
#include "recorder.c"
#include "test_harness.h"
#include <fcntl.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

enum { W = 32, H = 8 };

static char dir[64];

static void make_dir(void) {
  snprintf(dir, sizeof dir, "/tmp/eeye_rec_XXXXXX");
  if (!mkdtemp(dir))
    perror("mkdtemp");
}

static void path_in_dir(char *out, size_t cap, const char *name) {
  snprintf(out, cap, "%s/%s", dir, name);
}

static void sleep_ms(long ms) {
  struct timespec ts = {ms / 1000, (ms % 1000) * 1000000L};
  nanosleep(&ts, NULL);
}

static double now_ms(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (double)ts.tv_sec * 1e3 + (double)ts.tv_nsec / 1e6;
}

// A pool of frames whose raw planes are filled with `tag`, the way the
// producer would, all sitting in a free ring.
typedef struct {
  FrameRingBuffer free_ring;
  VideoFrame *frames[8];
} Pool;

static void pool_init(Pool *p) {
  ring_init(&p->free_ring);
  for (int i = 0; i < 8; i++) {
    p->frames[i] = vf_create(W, H, 0);
    ring_push_wait(&p->free_ring, p->frames[i], NULL);
  }
}

static VideoFrame *take(Pool *p, uint8_t tag) {
  VideoFrame *f = NULL;
  static atomic_bool running = true; // never cleared: the pool always has one
  CHECK(ring_pop_wait(&p->free_ring, (void **)&f, &running));
  memset(f->raw_data, tag, f->plane_sizes[0] + f->plane_sizes[1] +
                               f->plane_sizes[2]);
  f->source_jpeg_len = 0;
  return f;
}

static size_t pool_free_count(Pool *p) {
  RingStats s;
  ring_get_stats(&p->free_ring, &s);
  return s.occupancy;
}

static void pool_free(Pool *p) {
  for (int i = 0; i < 8; i++)
    vf_free(p->frames[i]);
  ring_destroy(&p->free_ring);
}

static size_t frame_bytes(void) { return (size_t)W * H * 2; }

static long file_size(const char *path) {
  struct stat st;
  return stat(path, &st) == 0 ? (long)st.st_size : -1;
}

// Hands a frame over, or returns it to the pool the way consumer_loop does
// when the recorder refuses it.
static bool submit_or_return(Recorder *r, Pool *p, VideoFrame *f) {
  if (recorder_submit(r, f))
    return true;
  ring_push_wait(&p->free_ring, f, NULL);
  return false;
}

static void test_raw_frames_written_in_order(void) {
  Pool p;
  pool_init(&p);
  char path[128];
  path_in_dir(path, sizeof path, "a.raw");
  Recorder *r = recorder_create(&p.free_ring, W, H, 30);
  recorder_set_target(r, path, RECORD_RAW);
  for (uint8_t tag = 1; tag <= 5; tag++) {
    CHECK(submit_or_return(r, &p, take(&p, tag)));
    sleep_ms(2); // stay within the queue bound; drops are tested below
  }
  recorder_set_target(r, "", RECORD_RAW);
  recorder_destroy(r);

  CHECK_EQ_INT(file_size(path), 5 * (long)frame_bytes());
  FILE *f = fopen(path, "rb");
  uint8_t *buf = malloc(frame_bytes());
  for (uint8_t tag = 1; tag <= 5 && f; tag++) {
    CHECK_EQ_INT(fread(buf, 1, frame_bytes(), f), frame_bytes());
    for (size_t i = 0; i < frame_bytes(); i++)
      if (buf[i] != tag) {
        CHECK_EQ_INT(buf[i], tag);
        break;
      }
  }
  if (f)
    fclose(f);
  free(buf);
  CHECK_EQ_INT(pool_free_count(&p), 8); // every frame came back
  pool_free(&p);
}

static void test_mjpeg_writes_the_source_bytes(void) {
  Pool p;
  pool_init(&p);
  char path[128];
  path_in_dir(path, sizeof path, "b.mjpeg");
  Recorder *r = recorder_create(&p.free_ring, W, H, 30);
  recorder_set_target(r, path, RECORD_MJPEG);
  const uint8_t jpeg[] = {0xFF, 0xD8, 1, 2, 3, 0xFF, 0xD9};
  for (int i = 0; i < 3; i++) {
    VideoFrame *f = take(&p, 9);
    vf_set_source_jpeg(f, jpeg, sizeof jpeg);
    CHECK(submit_or_return(r, &p, f));
    sleep_ms(2);
  }
  recorder_destroy(r);
  CHECK_EQ_INT(file_size(path), 3 * (long)sizeof jpeg);
  CHECK_EQ_INT(pool_free_count(&p), 8);
  pool_free(&p);
}

// A YUYV or yuv420 source has no JPEG to keep: "mjpeg" falls back to raw
// for the whole file rather than writing nothing.
static void test_mjpeg_without_source_falls_back_to_raw(void) {
  Pool p;
  pool_init(&p);
  char path[128];
  path_in_dir(path, sizeof path, "c.raw");
  Recorder *r = recorder_create(&p.free_ring, W, H, 30);
  recorder_set_target(r, path, RECORD_MJPEG);
  for (int i = 0; i < 2; i++) {
    CHECK(submit_or_return(r, &p, take(&p, 3)));
    sleep_ms(2);
  }
  recorder_destroy(r);
  CHECK_EQ_INT(file_size(path), 2 * (long)frame_bytes());
  pool_free(&p);
}

// Frames submitted before a target change belong to the old file, frames
// after it to the new one -- even though the writer handles them later.
static void test_target_change_is_ordered_with_frames(void) {
  Pool p;
  pool_init(&p);
  char a[128], b[128];
  path_in_dir(a, sizeof a, "d1.raw");
  path_in_dir(b, sizeof b, "d2.raw");
  Recorder *r = recorder_create(&p.free_ring, W, H, 30);
  recorder_set_target(r, a, RECORD_RAW);
  CHECK(submit_or_return(r, &p, take(&p, 1)));
  CHECK(submit_or_return(r, &p, take(&p, 1)));
  recorder_set_target(r, b, RECORD_RAW);
  CHECK(submit_or_return(r, &p, take(&p, 2)));
  recorder_destroy(r);
  CHECK_EQ_INT(file_size(a), 2 * (long)frame_bytes());
  CHECK_EQ_INT(file_size(b), 1 * (long)frame_bytes());
  CHECK_EQ_INT(pool_free_count(&p), 8);
  pool_free(&p);
}

static void *open_fifo_reader(void *arg) {
  const char *path = arg;
  int fd = open(path, O_RDONLY);
  char buf[4096];
  while (fd >= 0 && read(fd, buf, sizeof buf) > 0) {
  }
  if (fd >= 0)
    close(fd);
  return NULL;
}

// The point of the whole module: with the disk stalled, submit returns at
// once, holds at most recorder_max_queued frames, and counts the rest as
// dropped. Once the disk drains, everything comes back to the pool.
static void test_stalled_disk_never_blocks_submit(void) {
  Pool p;
  pool_init(&p);
  char path[128];
  path_in_dir(path, sizeof path, "stall.fifo");
  CHECK(mkfifo(path, 0600) == 0);
  Recorder *r = recorder_create(&p.free_ring, W, H, 30);
  recorder_set_target(r, path, RECORD_RAW); // writer now blocks in fopen()

  int accepted = 0, refused = 0;
  double worst = 0;
  for (int i = 0; i < 6; i++) {
    VideoFrame *f = take(&p, 4);
    double t0 = now_ms();
    bool took = submit_or_return(r, &p, f);
    double dt = now_ms() - t0;
    if (dt > worst)
      worst = dt;
    took ? accepted++ : refused++;
  }
  CHECK_EQ_INT(accepted, recorder_max_queued);
  CHECK_EQ_INT(refused, 6 - (int)recorder_max_queued);
  CHECK(worst < 50.0); // never waits on the disk
  RecorderStatus st;
  recorder_status(r, &st);
  CHECK_EQ_INT(st.dropped_frames, 6 - (int)recorder_max_queued);

  pthread_t reader;
  pthread_create(&reader, NULL, open_fifo_reader, path);
  recorder_set_target(r, "", RECORD_RAW);
  recorder_destroy(r);
  pthread_join(reader, NULL);
  CHECK_EQ_INT(pool_free_count(&p), 8);
  pool_free(&p);
}

// An unopenable path is reported, and frames offered afterwards are simply
// not recorded -- not counted as drops, which would read as a slow disk.
static void test_open_failure_is_reported_not_retried(void) {
  Pool p;
  pool_init(&p);
  Recorder *r = recorder_create(&p.free_ring, W, H, 30);
  recorder_set_target(r, "/proc/nonexistent-dir/x.raw", RECORD_RAW);
  RecorderStatus st = {0};
  for (int i = 0; i < 200 && st.state != RECORDING_FAILED; i++) {
    sleep_ms(1);
    recorder_status(r, &st);
  }
  CHECK_EQ_INT(st.state, RECORDING_FAILED);
  CHECK(strstr(st.error, "could not open") != NULL);
  CHECK(!submit_or_return(r, &p, take(&p, 1)));
  recorder_status(r, &st);
  CHECK_EQ_INT(st.dropped_frames, 0);
  recorder_destroy(r);
  CHECK_EQ_INT(pool_free_count(&p), 8);
  pool_free(&p);
}

int main(void) {
  printf("test_recorder:\n");
  make_dir();
  RUN_TEST(test_raw_frames_written_in_order);
  RUN_TEST(test_mjpeg_writes_the_source_bytes);
  RUN_TEST(test_mjpeg_without_source_falls_back_to_raw);
  RUN_TEST(test_target_change_is_ordered_with_frames);
  RUN_TEST(test_stalled_disk_never_blocks_submit);
  RUN_TEST(test_open_failure_is_reported_not_retried);
  char cmd[96];
  snprintf(cmd, sizeof cmd, "rm -rf %s", dir);
  if (system(cmd) != 0)
    printf("  (could not remove %s)\n", dir);
  TEST_MAIN_END();
}
