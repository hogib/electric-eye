// `hellion bench`: the /proc parsing it rests on, and one real run against
// a child process that calls itself "eeye", so the whole path -- finding
// the process, charging its threads, the CSV -- is exercised, not just the
// parsers. Streams live in a private EEYE_BUS_DIR so a real eeye on the
// machine does not leak into the figures.
#define _POSIX_C_SOURCE 200809L
#include "bench.c"
#include "test_harness.h"
#include <signal.h>
#include <sys/prctl.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

static char bus_dir[64];

static void test_parse_stat_plain(void) {
  ProcStat st;
  const char *line =
      "4306 (eeye) S 1 4306 4306 0 -1 4194560 2345 0 0 0 1234 567 0 0 20 0 "
      "7 0 12345 123456789 9876 18446744073709551615 1 1 0 0 0 0 0 0 0 0 0 "
      "0 17 2 0 0 0 0 0 0 0 0 0 0 0 0 0\n";
  CHECK(parse_stat(line, &st));
  CHECK(strcmp(st.comm, "eeye") == 0);
  CHECK(st.state == 'S');
  CHECK_EQ_INT(st.ppid, 1);
  CHECK_EQ_INT(st.ticks, 1234 + 567);
  CHECK_EQ_INT(st.num_threads, 7);
  CHECK_EQ_INT(st.rss_pages, 9876);
}

static void test_parse_stat_comm_with_spaces_and_parens(void) {
  // A name with spaces and a ')' inside: only the last ')' ends it.
  ProcStat st;
  const char *line =
      "77 (sink rtp (x)) R 42 1 1 0 -1 0 0 0 0 0 10 20 0 0 20 0 3 0 1 2 300 "
      "0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0\n";
  CHECK(parse_stat(line, &st));
  CHECK(strcmp(st.comm, "sink rtp (x)") == 0);
  CHECK(st.state == 'R');
  CHECK_EQ_INT(st.ppid, 42);
  CHECK_EQ_INT(st.ticks, 30);
  CHECK_EQ_INT(st.num_threads, 3);
  CHECK_EQ_INT(st.rss_pages, 300);
}

static void test_parse_stat_rejects_garbage(void) {
  ProcStat st;
  CHECK(!parse_stat("", &st));
  CHECK(!parse_stat("12 eeye S 1", &st));
  CHECK(!parse_stat("12 (eeye) S", &st));
}

static void test_status_lines(void) {
  const char *status = "Name:\teeye\nVmRSS:\t   2048 kB\nVmHWM:\t   4096 kB\n"
                       "voluntary_ctxt_switches:\t10\n"
                       "nonvoluntary_ctxt_switches:\t5\n";
  CHECK(mb_from_status_line(status, "VmRSS:") == 2.0);
  CHECK(mb_from_status_line(status, "VmHWM:") == 4.0);
  CHECK(mb_from_status_line(status, "VmSwap:") == 0.0);
  bool f;
  CHECK_EQ_INT(u64_from_status_line(status, "voluntary_ctxt_switches:", &f), 10);
  CHECK(f);
  CHECK_EQ_INT(u64_from_status_line(status, "nonvoluntary_ctxt_switches:", &f), 5);
  u64_from_status_line(status, "Threads:", &f);
  CHECK(!f);
}

static void test_throttle_words(void) {
  char buf[80];
  CHECK(strcmp(throttle_words(0, buf, sizeof buf), "") == 0);
  CHECK(strcmp(throttle_words(1, buf, sizeof buf), "under-voltage") == 0);
  CHECK(strcmp(throttle_words(0xf, buf, sizeof buf),
               "under-voltage freq-capped throttled soft-temp-limit") == 0);
}

static void test_live_proc_stat(void) {
  // The machine running the tests has a /proc/stat with at least one core.
  uint64_t busy, total, cb[max_cores], ct[max_cores];
  int cores = 0;
  CHECK(read_cpu_ticks(&busy, &total, cb, ct, &cores));
  CHECK(cores >= 1);
  CHECK(total >= busy);
  double mt, ma;
  read_meminfo(&mt, &ma);
  CHECK(mt > 0);
  CHECK(ma > 0 && ma <= mt);
}

static void test_root_of(void) {
  // pid 10 is eeye; 20 is its ffmpeg; 30 is unrelated; 40 is a hellion
  // that happens to be this process (excluded).
  Seen seen[4];
  memset(seen, 0, sizeof seen);
  seen[0].pid = 10; seen[0].ppid = 1; strcpy(seen[0].comm, "eeye");
  seen[1].pid = 20; seen[1].ppid = 10; strcpy(seen[1].comm, "ffmpeg");
  seen[2].pid = 30; seen[2].ppid = 1; strcpy(seen[2].comm, "ffmpeg");
  seen[3].pid = 40; seen[3].ppid = 1; strcpy(seen[3].comm, "hellion");
  CHECK_EQ_INT(root_of(seen, 4, 10, 40), 10);
  CHECK_EQ_INT(root_of(seen, 4, 20, 40), 10);
  CHECK_EQ_INT(root_of(seen, 4, 30, 40), 0);
  CHECK_EQ_INT(root_of(seen, 4, 40, 40), 0);
  CHECK_EQ_INT(root_of(seen, 4, 40, 99), 40);
}

static char *slurp(const char *path) {
  FILE *f = fopen(path, "rb");
  if (!f)
    return NULL;
  static char buf[1 << 16];
  size_t n = fread(buf, 1, sizeof buf - 1, f);
  fclose(f);
  buf[n] = '\0';
  return buf;
}

static void test_real_run_follows_a_process_named_eeye(void) {
  pid_t child = fork();
  CHECK(child >= 0);
  if (child == 0) {
    prctl(PR_SET_NAME, "eeye", 0, 0, 0);
    // Burns one core until killed, so its figure is a real one.
    volatile unsigned x = 0;
    for (;;)
      x += 1;
  }
  char csv[256];
  snprintf(csv, sizeof csv, "%s/bench.csv", bus_dir);
  // Quiet: the run prints its table to stdout, which the suite log keeps.
  int rc = hellion_bench(NULL, 2, csv);
  CHECK_EQ_INT(rc, 0);
  kill(child, SIGKILL);
  waitpid(child, NULL, 0);

  char *text = slurp(csv);
  CHECK(text != NULL);
  if (text) {
    CHECK(strncmp(text, "t_s,kind,name,id,value,value2\n", 30) == 0);
    char needle[64];
    snprintf(needle, sizeof needle, ",process,eeye,%d,", (int)child);
    const char *row = strstr(text, needle);
    CHECK(row != NULL);
    // A core-burning child reads near 100%; 10% leaves room for a loaded
    // test machine and a coarse first sample.
    if (row)
      CHECK(strtod(row + strlen(needle), NULL) > 10.0);
    snprintf(needle, sizeof needle, ",thread,eeye,%d,", (int)child);
    CHECK(strstr(text, needle) != NULL);
    CHECK(strstr(text, ",system,cpu_pct,,") != NULL);
    CHECK(strstr(text, ",system,mem_avail_mb,,") != NULL);
  }
  CHECK_EQ_INT(hellion_bench(NULL, 1, "/nonexistent-dir/x.csv"), 2);
}

int main(void) {
  snprintf(bus_dir, sizeof bus_dir, "/tmp/eeye_bench_test_%d", (int)getpid());
  mkdir(bus_dir, 0700);
  setenv("EEYE_BUS_DIR", bus_dir, 1);
  RUN_TEST(test_parse_stat_plain);
  RUN_TEST(test_parse_stat_comm_with_spaces_and_parens);
  RUN_TEST(test_parse_stat_rejects_garbage);
  RUN_TEST(test_status_lines);
  RUN_TEST(test_throttle_words);
  RUN_TEST(test_live_proc_stat);
  RUN_TEST(test_root_of);
  RUN_TEST(test_real_run_follows_a_process_named_eeye);
  TEST_MAIN_END();
}
