// child_proc.c runs every helper program eeye and hellion depend on. The
// failures it guards against are quiet ones -- a pipe held open by the
// wrong process, a signal left ignored across exec -- so each test drives
// a real child (/bin/sh and coreutils, which every GNU/Linux has) and
// checks the observable effect.
#include "child_proc.c"
#include "test_harness.h"

static int64_t elapsed_since(int64_t t0) { return now_ms() - t0; }

static void test_missing_program_fails_at_spawn(void) {
  ChildProc c;
  char *const argv[] = {"eeye-no-such-program-xyz", NULL};
  CHECK(!child_spawn(&c, "test", argv, CHILD_PIPE_STDOUT));
  CHECK_EQ_INT(c.pid, 0);
  CHECK_EQ_INT(c.out_fd, -1);
}

static void test_non_executable_path_fails_at_spawn(void) {
  ChildProc c;
  char *const argv[] = {"/dev/null", NULL}; // exists, not executable
  CHECK(!child_spawn(&c, "test", argv, 0));
}

static void test_read_exact_gets_every_byte_then_eof(void) {
  ChildProc c;
  char *const argv[] = {"head", "-c", "300000", "/dev/zero", NULL};
  CHECK(child_spawn(&c, "test", argv, CHILD_PIPE_STDOUT));

  static uint8_t buf[300000];
  memset(buf, 0xAA, sizeof buf);
  size_t got = 0;
  CHECK_EQ_INT(child_read_exact(c.out_fd, buf, sizeof buf, 5000, &got),
               CHILD_IO_OK);
  CHECK_EQ_INT(got, sizeof buf);
  CHECK_EQ_INT(buf[0], 0);
  CHECK_EQ_INT(buf[sizeof buf - 1], 0);

  uint8_t more;
  CHECK_EQ_INT(child_read_exact(c.out_fd, &more, 1, 5000, &got), CHILD_IO_EOF);
  CHECK_EQ_INT(got, 0);

  // child_wait, not child_stop: the natural exit status is what is being
  // checked, and child_stop would signal a child slow to finish exiting.
  int status = -1;
  CHECK(child_wait(&c, 5000, &status));
  CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 0);
  child_stop(&c, 100);
}

static void test_read_exact_short_stream_reports_partial(void) {
  ChildProc c;
  char *const argv[] = {"head", "-c", "10", "/dev/zero", NULL};
  CHECK(child_spawn(&c, "test", argv, CHILD_PIPE_STDOUT));
  uint8_t buf[64];
  size_t got = 0;
  CHECK_EQ_INT(child_read_exact(c.out_fd, buf, sizeof buf, 5000, &got),
               CHILD_IO_EOF);
  CHECK_EQ_INT(got, 10);
  child_stop(&c, 500);
}

static void test_read_times_out_on_silent_child(void) {
  ChildProc c;
  char *const argv[] = {"sleep", "30", NULL};
  CHECK(child_spawn(&c, "test", argv, CHILD_PIPE_STDOUT));
  uint8_t b;
  size_t got = 0;
  int64_t t0 = now_ms();
  CHECK_EQ_INT(child_read_exact(c.out_fd, &b, 1, 150, &got), CHILD_IO_TIMEOUT);
  int64_t took = elapsed_since(t0);
  CHECK(took >= 140 && took < 1000);
  child_stop(&c, 200);
  CHECK_EQ_INT(c.pid, 0);
}

// The polite stop: closing stdin is enough for a well-behaved filter, and
// it must exit on its own (status 0), not be signalled.
static void test_stop_closes_stdin_first(void) {
  ChildProc c;
  char *const argv[] = {"cat", NULL};
  CHECK(child_spawn(&c, "test", argv, CHILD_PIPE_STDIN | CHILD_PIPE_STDOUT));

  const char msg[] = "hello, child";
  CHECK_EQ_INT(child_write_all(c.in_fd, msg, sizeof msg, 2000), CHILD_IO_OK);
  char back[sizeof msg];
  size_t got = 0;
  CHECK_EQ_INT(child_read_exact(c.out_fd, back, sizeof back, 2000, &got),
               CHILD_IO_OK);
  CHECK_MEM_EQ(back, msg, sizeof msg);

  int64_t t0 = now_ms();
  int status = child_stop(&c, 2000);
  CHECK(elapsed_since(t0) < 1000);
  CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 0);
  CHECK_EQ_INT(c.in_fd, -1);
  CHECK_EQ_INT(c.out_fd, -1);
}

// A child that ignores SIGTERM still goes, after the grace period.
static void test_stop_escalates_to_sigkill(void) {
  ChildProc c;
  char *const argv[] = {"sh", "-c", "trap '' TERM; sleep 30", NULL};
  CHECK(child_spawn(&c, "test", argv, CHILD_NULL_STDIN));
  sleep_ms(100); // let the shell install its trap

  int64_t t0 = now_ms();
  int status = child_stop(&c, 150);
  int64_t took = elapsed_since(t0);
  CHECK(WIFSIGNALED(status) && WTERMSIG(status) == SIGKILL);
  CHECK(took >= 140 && took < 2000);
  CHECK_EQ_INT(c.pid, 0);
}

// The bug this module exists for: a descriptor the parent opened without
// close-on-exec must not survive into the child. If it did, the read below
// would block until the child exited instead of seeing EOF at once.
static void test_parent_fds_do_not_leak_into_child(void) {
  int p[2];
  CHECK(pipe(p) == 0); // deliberately without O_CLOEXEC

  ChildProc c;
  char *const argv[] = {"sleep", "5", NULL};
  CHECK(child_spawn(&c, "test", argv, CHILD_NULL_STDIN));

  close(p[1]);
  struct pollfd pfd = {.fd = p[0], .events = POLLIN, .revents = 0};
  CHECK_EQ_INT(poll(&pfd, 1, 500), 1);
  char b;
  CHECK_EQ_INT(read(p[0], &b, 1), 0); // EOF: no other writer exists
  close(p[0]);
  child_stop(&c, 200);
}

// eeye ignores SIGPIPE; its children must not. With the disposition reset,
// `yes` dies of SIGPIPE when its reader goes away. Inheriting SIG_IGN
// would have it exit with a write error instead.
static void test_child_gets_default_sigpipe(void) {
  ChildProc c;
  char *const argv[] = {"yes", NULL};
  CHECK(child_spawn(&c, "test", argv, CHILD_PIPE_STDOUT));
  char buf[64];
  size_t got = 0;
  CHECK_EQ_INT(child_read_some(c.out_fd, buf, sizeof buf, 2000, &got),
               CHILD_IO_OK);
  close_fd(&c.out_fd);

  int status = 0;
  CHECK(child_wait(&c, 2000, &status));
  CHECK(WIFSIGNALED(status) && WTERMSIG(status) == SIGPIPE);
}

static void test_stderr_can_join_stdout(void) {
  ChildProc c;
  char *const argv[] = {"sh", "-c", "echo out; echo err >&2", NULL};
  CHECK(child_spawn(&c, "test", argv,
                    CHILD_PIPE_STDOUT | CHILD_STDERR_TO_STDOUT));
  char out[64];
  child_read_all(c.out_fd, out, sizeof out, 2000);
  CHECK(strcmp(out, "out\nerr\n") == 0);
  child_stop(&c, 500);
}

static void test_read_all_truncates_and_drains(void) {
  ChildProc c;
  char *const argv[] = {"head", "-c", "100000", "/dev/zero", NULL};
  CHECK(child_spawn(&c, "test", argv, CHILD_PIPE_STDOUT));
  char out[16];
  // The child writes far more than fits; read_all must keep draining so
  // head is not left blocked, and exit 0 proves it finished writing.
  CHECK_EQ_INT(child_read_all(c.out_fd, out, sizeof out, 5000), 15);
  int status = -1;
  CHECK(child_wait(&c, 5000, &status));
  CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 0);
  child_stop(&c, 100);
}

// Writing to a child that has gone is an error result, not a SIGPIPE that
// kills the parent (main() ignores SIGPIPE, as eeye's does).
static void test_write_to_exited_child_is_eof(void) {
  ChildProc c;
  char *const argv[] = {"true", NULL};
  CHECK(child_spawn(&c, "test", argv, CHILD_PIPE_STDIN));
  CHECK(child_wait(&c, 2000, NULL));
  char buf[4096] = {0};
  CHECK_EQ_INT(child_write_all(c.in_fd, buf, sizeof buf, 1000), CHILD_IO_EOF);
  child_stop(&c, 100);
}

// child_stop signals the whole group, so a wrapper's own children go too
// -- otherwise a shell wrapper could die and leave ffmpeg writing on.
static void test_stop_reaches_grandchildren(void) {
  ChildProc c;
  char *const argv[] = {"sh", "-c", "sleep 30 & echo $!; wait", NULL};
  CHECK(child_spawn(&c, "test", argv, CHILD_PIPE_STDOUT));

  char line[32] = "";
  size_t got = 0;
  size_t total = 0;
  while (total < sizeof line - 1 && !find_char(line, '\n')) {
    if (child_read_some(c.out_fd, line + total, sizeof line - 1 - total, 2000,
                        &got) != CHILD_IO_OK)
      break;
    total += got;
    line[total] = '\0';
  }
  pid_t grandchild = (pid_t)atol(line);
  CHECK(grandchild > 0);

  child_stop(&c, 300);

  // Reparented and reaped by init (or a subreaper) shortly after dying.
  bool gone = false;
  for (int i = 0; i < 200 && !gone; i++) {
    gone = kill(grandchild, 0) < 0 && errno == ESRCH;
    if (!gone)
      sleep_ms(10);
  }
  CHECK(gone);
  if (!gone)
    kill(grandchild, SIGKILL);
}

static void test_stop_is_safe_on_empty_and_reaped(void) {
  ChildProc empty = {.pid = 0, .in_fd = -1, .out_fd = -1};
  CHECK_EQ_INT(child_stop(&empty, 10), -1);

  ChildProc c;
  char *const argv[] = {"true", NULL};
  CHECK(child_spawn(&c, "test", argv, 0));
  CHECK(child_wait(&c, 2000, NULL));
  child_stop(&c, 10); // already reaped: must not block or signal anything
  child_stop(&c, 10); // and twice is fine
  CHECK_EQ_INT(c.pid, 0);
}

int main(void) {
  signal(SIGPIPE, SIG_IGN); // as eeye's and hellion's main() do

  RUN_TEST(test_missing_program_fails_at_spawn);
  RUN_TEST(test_non_executable_path_fails_at_spawn);
  RUN_TEST(test_read_exact_gets_every_byte_then_eof);
  RUN_TEST(test_read_exact_short_stream_reports_partial);
  RUN_TEST(test_read_times_out_on_silent_child);
  RUN_TEST(test_stop_closes_stdin_first);
  RUN_TEST(test_stop_escalates_to_sigkill);
  RUN_TEST(test_parent_fds_do_not_leak_into_child);
  RUN_TEST(test_child_gets_default_sigpipe);
  RUN_TEST(test_stderr_can_join_stdout);
  RUN_TEST(test_read_all_truncates_and_drains);
  RUN_TEST(test_write_to_exited_child_is_eof);
  RUN_TEST(test_stop_reaches_grandchildren);
  RUN_TEST(test_stop_is_safe_on_empty_and_reaped);
  TEST_MAIN_END();
}
