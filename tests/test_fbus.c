// The frame bus is shared memory between processes, so the tests that
// matter run the writer and the readers in separate processes: a torn
// frame, a missed wake-up or a stale stream only shows up across a real
// process boundary. Every stream lives in a private temporary directory
// (EEYE_BUS_DIR), so parallel runs never meet.
// See test_lapped_copy_is_discarded.
static void after_copy_hook(const void *slot);
#define FBUS_AFTER_COPY_HOOK(slot) after_copy_hook(slot)

#include "fbus.c"
#include "test_harness.h"

// When set, the next copy has its slot "overwritten" by a simulated writer
// right after the reader copied it: the seqlock must notice.
static FbusWriter *lap_writer;

static void after_copy_hook(const void *slot) {
  if (!lap_writer)
    return;
  FbusWriter *w = lap_writer;
  lap_writer = NULL; // once
  (void)slot;
  // What a writer lapping the ring does to this slot: start writing a
  // newer frame into it. Publishing slot_count frames lands the last one
  // in the slot being read.
  uint8_t y[16 * 8], u[8 * 8], v[8 * 8];
  memset(y, 0xEE, sizeof y);
  memset(u, 0xEE, sizeof u);
  memset(v, 0xEE, sizeof v);
  const uint8_t *const planes[3] = {y, u, v};
  for (unsigned i = 0; i < fbus_slot_count; i++)
    fbus_writer_publish(w, planes, 100 + (int64_t)i, 0);
}
#include <signal.h>
#include <sys/wait.h>

static char bus_dir[64];

static void sleep_ms(int ms) {
  struct timespec ts = {ms / 1000, (long)(ms % 1000) * 1000000L};
  nanosleep(&ts, NULL);
}

static uint64_t ms_since(uint64_t t0) { return (mono_ns() - t0) / 1000000u; }

// One I422 frame's planes in a single buffer.
typedef struct {
  uint32_t w, h;
  uint8_t *buf;
  uint8_t *planes[3];
  size_t bytes[3];
} Planes;

static void planes_init(Planes *p, uint32_t w, uint32_t h) {
  p->w = w;
  p->h = h;
  uint32_t cw = (w + 1) / 2;
  p->bytes[0] = (size_t)w * h;
  p->bytes[1] = p->bytes[2] = (size_t)cw * h;
  p->buf = malloc(p->bytes[0] + 2 * p->bytes[1]);
  p->planes[0] = p->buf;
  p->planes[1] = p->buf + p->bytes[0];
  p->planes[2] = p->buf + p->bytes[0] + p->bytes[1];
}

static void planes_fill(Planes *p, uint8_t y, uint8_t u, uint8_t v) {
  memset(p->planes[0], y, p->bytes[0]);
  memset(p->planes[1], u, p->bytes[1]);
  memset(p->planes[2], v, p->bytes[2]);
}

static const uint8_t *const *cplanes(const Planes *p) {
  return (const uint8_t *const *)p->planes;
}

// Every byte of every plane equals `v`: a frame written whole.
static bool planes_uniform(const Planes *p, uint8_t v) {
  for (int i = 0; i < 3; i++)
    for (size_t j = 0; j < p->bytes[i]; j++)
      if (p->planes[i][j] != v)
        return false;
  return true;
}

static void test_publish_then_read(void) {
  FbusWriter *w = fbus_writer_open("t-basic", 64, 32);
  CHECK(w != NULL);
  FbusOpenResult why;
  FbusReader *r = fbus_reader_open("t-basic", &why);
  CHECK(r != NULL);
  uint32_t gw, gh;
  fbus_reader_geometry(r, &gw, &gh);
  CHECK_EQ_INT(gw, 64);
  CHECK_EQ_INT(gh, 32);

  Planes in, out;
  planes_init(&in, 64, 32);
  planes_init(&out, 64, 32);
  planes_fill(&in, 1, 2, 3);
  for (size_t i = 0; i < in.bytes[0]; i++)
    in.planes[0][i] = (uint8_t)i; // something with structure
  fbus_writer_publish(w, cplanes(&in), 7, 123456789u);

  FbusFrameInfo info;
  CHECK_EQ_INT(fbus_reader_next(r, out.planes, 1000, &info), FBUS_FRAME);
  CHECK_MEM_EQ(out.buf, in.buf, in.bytes[0] + 2 * in.bytes[1]);
  CHECK_EQ_INT(info.frame_no, 7);
  CHECK_EQ_INT(info.capture_mono_ns, 123456789u);
  CHECK(info.capture_real_ns > 0);
  CHECK(info.publish_mono_ns > 0);
  CHECK_EQ_INT(info.skipped, 0);

  // Odd widths round chroma up, as VideoFrame does.
  FbusWriter *odd = fbus_writer_open("t-odd", 33, 2);
  CHECK(odd != NULL);
  fbus_writer_close(odd);

  fbus_reader_close(r);
  fbus_writer_close(w);
  free(in.buf);
  free(out.buf);
}

// A frame published as chroma-neutral reads back with all-128 chroma
// whatever the caller's chroma planes held (they are not copied: a slot is
// filled once and then left), and a slot that held a full-colour frame in
// between gets its 128s back. Several laps of the slots, mixed.
static void test_neutral_chroma_publish(void) {
  FbusWriter *w = fbus_writer_open("t-neutral", 64, 32);
  CHECK(w != NULL);
  FbusReader *r = fbus_reader_open("t-neutral", NULL);
  CHECK(r != NULL);
  if (!w || !r)
    return;
  Planes in, out;
  planes_init(&in, 64, 32);
  planes_init(&out, 64, 32);
  static const bool neutral[] = {1, 1, 0, 1, 0, 0, 1, 1, 1, 0, 1, 1, 1, 1, 0, 1};
  for (int i = 0; i < 16; i++) {
    memset(in.planes[0], 10 + i, in.bytes[0]);
    // Neutral frames carry garbage chroma: it must not be what readers see.
    memset(in.planes[1], neutral[i] ? 0xEE : 60 + i, in.bytes[1]);
    memset(in.planes[2], neutral[i] ? 0x11 : 90 + i, in.bytes[2]);
    fbus_writer_publish_frame(w, cplanes(&in), i, 0, neutral[i]);
    CHECK_EQ_INT(fbus_reader_next(r, out.planes, 1000, NULL), FBUS_FRAME);
    bool ok = out.planes[0][0] == (uint8_t)(10 + i) &&
              out.planes[0][in.bytes[0] - 1] == (uint8_t)(10 + i);
    for (size_t j = 0; j < in.bytes[1]; j++) {
      uint8_t wu = neutral[i] ? 128 : (uint8_t)(60 + i);
      uint8_t wv = neutral[i] ? 128 : (uint8_t)(90 + i);
      ok &= out.planes[1][j] == wu && out.planes[2][j] == wv;
    }
    if (!ok)
      printf("    frame %d (neutral %d) read back wrong\n", i, neutral[i]);
    CHECK(ok);
  }
  fbus_reader_close(r);
  fbus_writer_close(w);
  free(in.buf);
  free(out.buf);
}

static void test_reader_gets_newest_and_counts_skips(void) {
  FbusWriter *w = fbus_writer_open("t-skip", 16, 8);
  FbusReader *r = fbus_reader_open("t-skip", NULL);
  Planes p;
  planes_init(&p, 16, 8);
  FbusFrameInfo info;

  for (int i = 1; i <= 5; i++) {
    planes_fill(&p, (uint8_t)i, 0, 0);
    fbus_writer_publish(w, cplanes(&p), i, 0);
  }
  CHECK_EQ_INT(fbus_reader_next(r, p.planes, 1000, &info), FBUS_FRAME);
  CHECK_EQ_INT(info.frame_no, 5); // the newest, not the oldest
  CHECK_EQ_INT(info.skipped, 0);  // nothing was owed before the first read
  CHECK_EQ_INT(info.capture_mono_ns, 0);
  CHECK_EQ_INT(info.capture_real_ns, 0); // unknown stays unknown

  for (int i = 6; i <= 8; i++) {
    planes_fill(&p, (uint8_t)i, 0, 0);
    fbus_writer_publish(w, cplanes(&p), i, 0);
  }
  CHECK_EQ_INT(fbus_reader_next(r, p.planes, 1000, &info), FBUS_FRAME);
  CHECK_EQ_INT(info.frame_no, 8);
  CHECK_EQ_INT(info.skipped, 2);
  CHECK(planes_uniform(&p, 8) == false); // only Y was set to 8
  CHECK_EQ_INT(p.planes[0][0], 8);

  // Nothing new: a timeout, not the same frame again.
  uint64_t t0 = mono_ns();
  CHECK_EQ_INT(fbus_reader_next(r, p.planes, 120, &info), FBUS_TIMEOUT);
  uint64_t took = ms_since(t0);
  CHECK(took >= 110 && took < 1000);

  fbus_reader_close(r);
  fbus_writer_close(w);
  free(p.buf);
}

typedef struct {
  FbusWriter *w;
  int delay_ms;
} DelayedPublish;

static void *publish_later(void *arg) {
  DelayedPublish *d = (DelayedPublish *)arg;
  sleep_ms(d->delay_ms);
  Planes p;
  planes_init(&p, 16, 8);
  planes_fill(&p, 9, 9, 9);
  fbus_writer_publish(d->w, cplanes(&p), 1, 0);
  free(p.buf);
  return NULL;
}

// A waiting reader is woken by the publish (futex), not by its timeout.
static void test_reader_wakes_on_publish(void) {
  FbusWriter *w = fbus_writer_open("t-wake", 16, 8);
  FbusReader *r = fbus_reader_open("t-wake", NULL);
  Planes p;
  planes_init(&p, 16, 8);

  DelayedPublish d = {w, 60};
  pthread_t t;
  pthread_create(&t, NULL, publish_later, &d);
  uint64_t t0 = mono_ns();
  CHECK_EQ_INT(fbus_reader_next(r, p.planes, 5000, NULL), FBUS_FRAME);
  uint64_t took = ms_since(t0);
  CHECK(took >= 50 && took < 400);
  CHECK(planes_uniform(&p, 9));
  pthread_join(t, NULL);

  fbus_reader_close(r);
  fbus_writer_close(w);
  free(p.buf);
}

static void test_close_makes_stream_gone(void) {
  FbusWriter *w = fbus_writer_open("t-close", 16, 8);
  FbusReader *r = fbus_reader_open("t-close", NULL);
  Planes p;
  planes_init(&p, 16, 8);
  fbus_writer_close(w);

  uint64_t t0 = mono_ns();
  CHECK_EQ_INT(fbus_reader_next(r, p.planes, 5000, NULL), FBUS_GONE);
  CHECK(ms_since(t0) < 200); // seen at once, not at the timeout
  fbus_reader_close(r);

  FbusOpenResult why;
  CHECK(fbus_reader_open("t-close", &why) == NULL);
  CHECK_EQ_INT(why, FBUS_OPEN_MISSING); // the file went with it
  free(p.buf);
}

// A restarted writer replaces the stream; its old readers see it as gone,
// and the old writer closing late must not delete the new one's file.
static void test_restart_replaces_stream(void) {
  FbusWriter *a = fbus_writer_open("t-restart", 16, 8);
  FbusReader *r = fbus_reader_open("t-restart", NULL);
  Planes p;
  planes_init(&p, 16, 8);

  FbusWriter *b = fbus_writer_open("t-restart", 32, 16);
  CHECK(b != NULL);
  uint64_t t0 = mono_ns();
  CHECK_EQ_INT(fbus_reader_next(r, p.planes, 5000, NULL), FBUS_GONE);
  CHECK(ms_since(t0) < 1000);
  fbus_reader_close(r);

  fbus_writer_close(a);
  FbusStatus st;
  CHECK_EQ_INT(fbus_status("t-restart", &st), FBUS_OPEN_OK);
  CHECK_EQ_INT(st.width, 32); // still b's file
  CHECK(st.alive);

  r = fbus_reader_open("t-restart", NULL);
  CHECK(r != NULL);
  uint32_t gw, gh;
  fbus_reader_geometry(r, &gw, &gh);
  CHECK_EQ_INT(gw, 32);
  CHECK_EQ_INT(gh, 16);
  fbus_reader_close(r);
  fbus_writer_close(b);
  free(p.buf);
}

// A writer that dies without closing leaves its file behind. Its stopped
// heartbeat is what tells readers.
static void test_dead_writer_detected(void) {
  pid_t pid = fork();
  if (pid == 0) {
    FbusWriter *w = fbus_writer_open("t-dead", 16, 8);
    Planes p;
    planes_init(&p, 16, 8);
    planes_fill(&p, 1, 1, 1);
    fbus_writer_publish(w, cplanes(&p), 1, 0);
    for (;;)
      sleep_ms(1000); // the parent kills us
  }

  FbusReader *r = NULL;
  for (int i = 0; i < 200 && !r; i++) {
    r = fbus_reader_open("t-dead", NULL);
    if (!r)
      sleep_ms(10);
  }
  CHECK(r != NULL);
  if (!r) {
    kill(pid, SIGKILL);
    waitpid(pid, NULL, 0);
    return;
  }
  Planes p;
  planes_init(&p, 16, 8);
  CHECK_EQ_INT(fbus_reader_next(r, p.planes, 2000, NULL), FBUS_FRAME);
  CHECK_EQ_INT(fbus_reader_next(r, p.planes, 300, NULL), FBUS_TIMEOUT);

  kill(pid, SIGKILL);
  waitpid(pid, NULL, 0);

  uint64_t t0 = mono_ns();
  CHECK_EQ_INT(fbus_reader_next(r, p.planes, 10000, NULL), FBUS_GONE);
  uint64_t took = ms_since(t0);
  CHECK(took >= 2000 && took < 5000); // ~heartbeat_stale_ns after the kill
  fbus_reader_close(r);

  FbusOpenResult why;
  CHECK(fbus_reader_open("t-dead", &why) == NULL);
  CHECK_EQ_INT(why, FBUS_OPEN_STALE);
  FbusStatus st;
  CHECK_EQ_INT(fbus_status("t-dead", &st), FBUS_OPEN_OK);
  CHECK(!st.alive);
  CHECK(!st.closed);
  free(p.buf);
}

// The deterministic version of the torn-read case: the writer laps the
// ring in the exact window between a reader's copy and its recheck. The
// copy must be thrown away and the newest frame returned instead.
static void test_lapped_copy_is_discarded(void) {
  FbusWriter *w = fbus_writer_open("t-lap", 16, 8);
  FbusReader *r = fbus_reader_open("t-lap", NULL);
  Planes p;
  planes_init(&p, 16, 8);
  planes_fill(&p, 0x11, 0x11, 0x11);
  fbus_writer_publish(w, cplanes(&p), 1, 0);

  uint64_t retries_before = fbus_torn_retries;
  lap_writer = w;
  FbusFrameInfo info;
  CHECK_EQ_INT(fbus_reader_next(r, p.planes, 1000, &info), FBUS_FRAME);
  CHECK_EQ_INT(fbus_torn_retries - retries_before, 1);
  // Not frame 1 (its copy was torn), but the newest one the lap wrote.
  CHECK_EQ_INT(info.frame_no, 100 + fbus_slot_count - 1);
  CHECK(planes_uniform(&p, 0xEE));

  fbus_reader_close(r);
  fbus_writer_close(w);
  free(p.buf);
}

// Reads frames for `ms` and checks every one is whole: each frame is
// written uniformly with (frame_no & 0xFF), so any mix of two frames shows.
// Returns the number of bad frames, or -1 if it never got any.
static int read_and_verify(const char *name, uint32_t w, uint32_t h, int ms,
                           uint64_t *frames_out, uint64_t *skipped_out) {
  FbusReader *r = NULL;
  for (int i = 0; i < 300 && !r; i++) {
    r = fbus_reader_open(name, NULL);
    if (!r)
      sleep_ms(10);
  }
  if (!r)
    return -1;
  Planes p;
  planes_init(&p, w, h);
  uint64_t frames = 0, skipped = 0;
  int bad = 0;
  int64_t last = -1;
  uint64_t t0 = mono_ns();
  while (ms_since(t0) < (uint64_t)ms) {
    FbusFrameInfo info;
    FbusReadResult res = fbus_reader_next(r, p.planes, 500, &info);
    if (res == FBUS_GONE)
      break;
    if (res != FBUS_FRAME)
      continue;
    frames++;
    skipped += info.skipped;
    if (!planes_uniform(&p, (uint8_t)(info.frame_no & 0xFF)) ||
        info.frame_no <= last)
      bad++;
    last = info.frame_no;
  }
  fbus_reader_close(r);
  free(p.buf);
  if (frames_out)
    *frames_out = frames;
  if (skipped_out)
    *skipped_out = skipped;
  return frames ? bad : -1;
}

// Publishes as fast as it can for `ms`; for a child process.
static void write_flat_out(const char *name, uint32_t w, uint32_t h, int ms) {
  FbusWriter *wr = fbus_writer_open(name, w, h);
  if (!wr)
    _exit(2);
  Planes p;
  planes_init(&p, w, h);
  uint64_t t0 = mono_ns();
  for (int64_t n = 1; ms_since(t0) < (uint64_t)ms; n++) {
    planes_fill(&p, (uint8_t)(n & 0xFF), (uint8_t)(n & 0xFF),
                (uint8_t)(n & 0xFF));
    fbus_writer_publish(wr, cplanes(&p), n, mono_ns());
  }
  fbus_writer_close(wr);
  free(p.buf);
  _exit(0);
}

// The seqlock under real pressure: a writer in another process lapping
// the 4-slot ring as fast as memcpy allows. Every frame a reader gets must
// be whole, and they must arrive in order.
static void check_no_torn_frames(const char *name, uint32_t w, uint32_t h) {
  pid_t pid = fork();
  if (pid == 0)
    write_flat_out(name, w, h, 1500);

  uint64_t retries_before = fbus_torn_retries;
  uint64_t frames = 0, skipped = 0;
  int bad = read_and_verify(name, w, h, 1300, &frames, &skipped);
  uint64_t retries = fbus_torn_retries - retries_before;
  int status = 0;
  waitpid(pid, &status, 0);
  CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 0);
  CHECK_EQ_INT(bad, 0);
  CHECK(frames > 50);
  CHECK(skipped > 0); // the writer really was outrunning the reader
  printf("    %ux%u: %llu frames read, %llu skipped, %llu torn copies caught "
         "and retried, %d torn frames returned\n",
         w, h, (unsigned long long)frames, (unsigned long long)skipped,
         (unsigned long long)retries, bad);
  // Whether tearing happened here is up to the scheduler;
  // test_lapped_copy_is_discarded proves the check works deterministically.
}

static void test_no_torn_frames_small(void) {
  check_no_torn_frames("t-torn-small", 32, 16);
}

static void test_no_torn_frames_vga(void) {
  check_no_torn_frames("t-torn-vga", 640, 480);
}

// Several readers at once, each in its own process, each seeing whole
// frames in order -- and none slowing the writer down.
static void test_several_readers(void) {
  enum { n_readers = 3 };
  pid_t readers[n_readers];
  for (int i = 0; i < n_readers; i++) {
    readers[i] = fork();
    if (readers[i] == 0) {
      uint64_t frames = 0;
      int bad = read_and_verify("t-multi", 160, 120, 900, &frames, NULL);
      _exit(bad == 0 && frames > 20 ? 0 : 1);
    }
  }

  FbusWriter *w = fbus_writer_open("t-multi", 160, 120);
  Planes p;
  planes_init(&p, 160, 120);
  uint64_t t0 = mono_ns();
  for (int64_t n = 1; ms_since(t0) < 1100; n++) {
    planes_fill(&p, (uint8_t)(n & 0xFF), (uint8_t)(n & 0xFF),
                (uint8_t)(n & 0xFF));
    fbus_writer_publish(w, cplanes(&p), n, 0);
    sleep_ms(2);
  }
  for (int i = 0; i < n_readers; i++) {
    int status = 0;
    waitpid(readers[i], &status, 0);
    CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 0);
  }
  fbus_writer_close(w);
  free(p.buf);
}

typedef struct {
  char names[8][fbus_max_stream_name];
  int n;
} NameList;

static void collect(const char *name, void *ctx) {
  NameList *l = (NameList *)ctx;
  if (l->n < 8)
    snprintf(l->names[l->n++], fbus_max_stream_name, "%s", name);
}

static bool listed(const NameList *l, const char *name) {
  for (int i = 0; i < l->n; i++)
    if (strcmp(l->names[i], name) == 0)
      return true;
  return false;
}

static void test_status_and_list(void) {
  FbusWriter *a = fbus_writer_open("t-list-a", 16, 8);
  FbusWriter *b = fbus_writer_open("t-list-b", 16, 8);
  Planes p;
  planes_init(&p, 16, 8);
  fbus_writer_publish(a, cplanes(&p), 41, 0);
  fbus_writer_publish(a, cplanes(&p), 42, 0);

  FbusStatus st;
  CHECK_EQ_INT(fbus_status("t-list-a", &st), FBUS_OPEN_OK);
  CHECK(st.alive);
  CHECK(!st.closed);
  CHECK_EQ_INT(st.frames, 2);
  CHECK_EQ_INT(st.last_frame_no, 42);
  CHECK_EQ_INT(st.writer_pid, getpid());
  CHECK(st.last_frame_age_ns < 1000000000ull);

  CHECK_EQ_INT(fbus_status("t-list-b", &st), FBUS_OPEN_OK);
  CHECK_EQ_INT(st.frames, 0);
  CHECK_EQ_INT(st.last_frame_no, -1);

  NameList l = {.n = 0};
  CHECK(fbus_list(collect, &l) >= 2);
  CHECK(listed(&l, "t-list-a"));
  CHECK(listed(&l, "t-list-b"));

  fbus_writer_close(a);
  fbus_writer_close(b);
  l.n = 0;
  fbus_list(collect, &l);
  CHECK(!listed(&l, "t-list-a"));
  free(p.buf);
}

static void test_bad_input_rejected(void) {
  CHECK(fbus_writer_open("", 16, 8) == NULL);
  CHECK(fbus_writer_open("../escape", 16, 8) == NULL);
  CHECK(fbus_writer_open("has space", 16, 8) == NULL);
  CHECK(fbus_writer_open("t-zero", 0, 8) == NULL);

  FbusOpenResult why;
  CHECK(fbus_reader_open("t-never-existed", &why) == NULL);
  CHECK_EQ_INT(why, FBUS_OPEN_MISSING);

  // A file that is not a stream, short and long.
  char path[PATH_MAX];
  snprintf(path, sizeof path, "%s/eeye.t-junk", bus_dir);
  FILE *f = fopen(path, "wb");
  for (int i = 0; i < 8192; i++)
    fputc(i & 0xFF, f);
  fclose(f);
  CHECK(fbus_reader_open("t-junk", &why) == NULL);
  CHECK_EQ_INT(why, FBUS_OPEN_BAD_FORMAT);
  FbusStatus st;
  CHECK_EQ_INT(fbus_status("t-junk", &st), FBUS_OPEN_BAD_FORMAT);

  snprintf(path, sizeof path, "%s/eeye.t-short", bus_dir);
  f = fopen(path, "wb");
  fputs("EEYEBUS1", f);
  fclose(f);
  CHECK(fbus_reader_open("t-short", &why) == NULL);
  CHECK_EQ_INT(why, FBUS_OPEN_BAD_FORMAT);
}

static void remove_dir(const char *dir) {
  DIR *d = opendir(dir);
  if (!d)
    return;
  struct dirent *e;
  char path[PATH_MAX];
  while ((e = readdir(d)) != NULL) {
    if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0)
      continue;
    snprintf(path, sizeof path, "%s/%s", dir, e->d_name);
    unlink(path);
  }
  closedir(d);
  rmdir(dir);
}

int main(void) {
  // tmpfs, like the real thing, where available.
  snprintf(bus_dir, sizeof bus_dir, "/dev/shm/eeye-test-fbus-XXXXXX");
  if (!mkdtemp(bus_dir)) {
    snprintf(bus_dir, sizeof bus_dir, "/tmp/eeye-test-fbus-XXXXXX");
    if (!mkdtemp(bus_dir)) {
      printf("cannot create a temporary bus directory\n");
      return 1;
    }
  }
  setenv("EEYE_BUS_DIR", bus_dir, 1);

  RUN_TEST(test_publish_then_read);
  RUN_TEST(test_neutral_chroma_publish);
  RUN_TEST(test_reader_gets_newest_and_counts_skips);
  RUN_TEST(test_reader_wakes_on_publish);
  RUN_TEST(test_close_makes_stream_gone);
  RUN_TEST(test_restart_replaces_stream);
  RUN_TEST(test_dead_writer_detected);
  RUN_TEST(test_lapped_copy_is_discarded);
  RUN_TEST(test_no_torn_frames_small);
  RUN_TEST(test_no_torn_frames_vga);
  RUN_TEST(test_several_readers);
  RUN_TEST(test_status_and_list);
  RUN_TEST(test_bad_input_rejected);

  remove_dir(bus_dir);
  TEST_MAIN_END();
}
