// _GNU_SOURCE before any header include: pipe2(), F_SETPIPE_SZ and the
// close_range syscall number are all GNU/Linux extensions that strict ISO
// mode (-std=c99) would otherwise hide.
#define _GNU_SOURCE

#include "child_proc.h"
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

// How many descriptors the child's close loop walks when close_range() is
// unavailable (kernels before 5.9). Bounded so a huge RLIMIT_NOFILE does
// not turn every spawn into millions of close() calls.
static const long max_fd_scan = 65536;

// How long child_stop() lets a child exit by itself, once its pipes are
// closed, before signalling it. See child_stop().
static const int stop_settle_ms = 100;

static int64_t now_ms(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static void sleep_ms(int ms) {
  struct timespec ts = {.tv_sec = ms / 1000, .tv_nsec = (ms % 1000) * 1000000L};
  while (nanosleep(&ts, &ts) < 0 && errno == EINTR) {
  }
}

static void close_fd(int *fd) {
  if (*fd >= 0) {
    close(*fd);
    *fd = -1;
  }
}

// strchr(), spelled out. With _GNU_SOURCE, newer glibc defines strchr as a
// C11 _Generic macro, which -pedantic flags under -std=gnu99.
static const char *find_char(const char *s, char ch) {
  for (; *s; s++)
    if (*s == ch)
      return s;
  return NULL;
}

// Resolves `name` against PATH the way execvp() would, but in the parent:
// execvp() may allocate while searching, which is not safe between fork()
// and exec() in a multi-threaded process, and doing it here also turns
// "not installed" into an error before anything is forked.
static bool resolve_program(const char *name, char *out, size_t cap) {
  if (find_char(name, '/')) {
    if (access(name, X_OK) != 0)
      return false;
    snprintf(out, cap, "%s", name);
    return true;
  }

  const char *path = getenv("PATH");
  if (!path || !*path)
    path = "/usr/local/bin:/usr/bin:/bin";

  while (*path) {
    const char *end = find_char(path, ':');
    size_t len = end ? (size_t)(end - path) : strlen(path);
    // An empty PATH element means the current directory.
    int n = len ? snprintf(out, cap, "%.*s/%s", (int)len, path, name)
                : snprintf(out, cap, "./%s", name);
    if (n > 0 && (size_t)n < cap && access(out, X_OK) == 0)
      return true;
    if (!end)
      break;
    path = end + 1;
  }
  return false;
}

bool child_program_exists(const char *name) {
  char path[PATH_MAX];
  return resolve_program(name, path, sizeof path);
}

// Child side of child_spawn(), between fork() and exec(). Only
// async-signal-safe calls from here on: the parent may be multi-threaded,
// and another thread could have held a lock (malloc's, stdio's) at the
// moment of the fork.
static void close_inherited_fds(int keep, long scan_limit) {
#if defined(SYS_close_range)
  // keep is above 2 (it came from pipe2), so these two ranges cover every
  // descriptor except stdio and keep.
  if (syscall(SYS_close_range, 3u, (unsigned)keep - 1, 0u) == 0 &&
      syscall(SYS_close_range, (unsigned)keep + 1, ~0u, 0u) == 0)
    return;
#endif
  for (long fd = 3; fd < scan_limit; fd++)
    if (fd != keep)
      close((int)fd);
}

static void report_exec_error(int exec_fd, int err) {
  ssize_t ignored = write(exec_fd, &err, sizeof err);
  (void)ignored;
  _exit(127);
}

static void child_side(const char *path, char *const argv[], unsigned flags,
                       int in_read, int out_write, int stderr_write,
                       int exec_fd, long scan_limit) {
  // Back to defaults: an ignored SIGPIPE survives exec, and the parent's
  // signal mask is inherited by the child.
  static const int reset[] = {SIGPIPE, SIGINT, SIGTERM, SIGQUIT, SIGCHLD,
                              SIGHUP};
  for (size_t i = 0; i < sizeof reset / sizeof reset[0]; i++)
    signal(reset[i], SIG_DFL);
  sigset_t none;
  sigemptyset(&none);
  sigprocmask(SIG_SETMASK, &none, NULL);

  // Its own process group: see child_proc.h.
  setpgid(0, 0);

  if (flags & CHILD_PIPE_STDIN) {
    if (dup2(in_read, STDIN_FILENO) < 0)
      report_exec_error(exec_fd, errno);
  } else if (flags & CHILD_NULL_STDIN) {
    int null_fd = open("/dev/null", O_RDONLY);
    if (null_fd >= 0 && null_fd != STDIN_FILENO) {
      dup2(null_fd, STDIN_FILENO);
      close(null_fd);
    }
  }

  if (flags & CHILD_PIPE_STDOUT) {
    if (dup2(out_write, STDOUT_FILENO) < 0)
      report_exec_error(exec_fd, errno);
    if ((flags & CHILD_STDERR_TO_STDOUT) &&
        dup2(out_write, STDERR_FILENO) < 0)
      report_exec_error(exec_fd, errno);
  }
  if (flags & CHILD_PIPE_STDERR) {
    if (dup2(stderr_write, STDERR_FILENO) < 0)
      report_exec_error(exec_fd, errno);
  } else if (flags & CHILD_QUIET_STDERR) {
    int null_fd = open("/dev/null", O_WRONLY);
    if (null_fd >= 0 && null_fd != STDERR_FILENO) {
      dup2(null_fd, STDERR_FILENO);
      close(null_fd);
    }
  }

  close_inherited_fds(exec_fd, scan_limit);

  execv(path, argv);
  report_exec_error(exec_fd, errno);
}

bool child_spawn(ChildProc *c, const char *tag, char *const argv[],
                 unsigned flags) {
  *c = (ChildProc){.pid = 0, .in_fd = -1, .out_fd = -1, .err_fd = -1};

  char path[PATH_MAX];
  if (!resolve_program(argv[0], path, sizeof path)) {
    if (find_char(argv[0], '/'))
      printf("%s: cannot run %s: no such executable file\n", tag, argv[0]);
    else
      printf("%s: cannot run %s: not found in PATH (is it installed?)\n", tag,
             argv[0]);
    return false;
  }

  int in_pipe[2] = {-1, -1}, out_pipe[2] = {-1, -1}, se_pipe[2] = {-1, -1},
      exec_pipe[2] = {-1, -1};
  if (((flags & CHILD_PIPE_STDIN) && pipe2(in_pipe, O_CLOEXEC) != 0) ||
      ((flags & CHILD_PIPE_STDOUT) && pipe2(out_pipe, O_CLOEXEC) != 0) ||
      ((flags & CHILD_PIPE_STDERR) && pipe2(se_pipe, O_CLOEXEC) != 0) ||
      pipe2(exec_pipe, O_CLOEXEC) != 0) {
    printf("%s: pipe failed: %s\n", tag, strerror(errno));
    close_fd(&in_pipe[0]);
    close_fd(&in_pipe[1]);
    close_fd(&out_pipe[0]);
    close_fd(&out_pipe[1]);
    close_fd(&se_pipe[0]);
    close_fd(&se_pipe[1]);
    close_fd(&exec_pipe[0]);
    close_fd(&exec_pipe[1]);
    return false;
  }

  struct rlimit rl;
  long scan_limit = 1024;
  if (getrlimit(RLIMIT_NOFILE, &rl) == 0 && rl.rlim_cur != RLIM_INFINITY)
    scan_limit = (long)rl.rlim_cur;
  if (scan_limit > max_fd_scan)
    scan_limit = max_fd_scan;

  // Make sure stdout reaches its destination before the child starts
  // writing to the same descriptor; otherwise our buffered lines land
  // after the child's.
  fflush(stdout);

  pid_t pid = fork();
  if (pid < 0) {
    printf("%s: fork failed: %s\n", tag, strerror(errno));
    close_fd(&in_pipe[0]);
    close_fd(&in_pipe[1]);
    close_fd(&out_pipe[0]);
    close_fd(&out_pipe[1]);
    close_fd(&se_pipe[0]);
    close_fd(&se_pipe[1]);
    close_fd(&exec_pipe[0]);
    close_fd(&exec_pipe[1]);
    return false;
  }
  if (pid == 0)
    child_side(path, argv, flags, in_pipe[0], out_pipe[1], se_pipe[1],
               exec_pipe[1], scan_limit);

  // Also set from the parent: whichever of the two runs first, the group
  // exists before child_stop() could signal it.
  setpgid(pid, pid);

  close_fd(&in_pipe[0]);
  close_fd(&out_pipe[1]);
  close_fd(&se_pipe[1]);
  close_fd(&exec_pipe[1]);

  // exec succeeded exactly when the error pipe closes with nothing in it
  // (close-on-exec closed the child's end).
  int child_errno = 0;
  ssize_t n;
  do {
    n = read(exec_pipe[0], &child_errno, sizeof child_errno);
  } while (n < 0 && errno == EINTR);
  close_fd(&exec_pipe[0]);

  if (n > 0) {
    printf("%s: could not execute %s: %s\n", tag, path,
           strerror(child_errno));
    close_fd(&in_pipe[1]);
    close_fd(&out_pipe[0]);
    close_fd(&se_pipe[0]);
    while (waitpid(pid, NULL, 0) < 0 && errno == EINTR) {
    }
    return false;
  }

  if (in_pipe[1] >= 0)
    fcntl(in_pipe[1], F_SETFL, fcntl(in_pipe[1], F_GETFL) | O_NONBLOCK);
  if (out_pipe[0] >= 0)
    fcntl(out_pipe[0], F_SETFL, fcntl(out_pipe[0], F_GETFL) | O_NONBLOCK);
  if (se_pipe[0] >= 0)
    fcntl(se_pipe[0], F_SETFL, fcntl(se_pipe[0], F_GETFL) | O_NONBLOCK);

  c->pid = pid;
  c->in_fd = in_pipe[1];
  c->out_fd = out_pipe[0];
  c->err_fd = se_pipe[0];
  return true;
}

// Waits for `events` on fd until deadline. Returns 1 when ready, 0 on
// timeout, -1 on error (errno set).
static int wait_ready(int fd, short events, int64_t deadline) {
  for (;;) {
    int64_t left = deadline - now_ms();
    if (left < 0)
      left = 0;
    struct pollfd pfd = {.fd = fd, .events = events, .revents = 0};
    int r = poll(&pfd, 1, (int)(left > INT_MAX ? INT_MAX : left));
    if (r < 0 && errno == EINTR)
      continue;
    // POLLHUP/POLLERR count as ready: the read or write that follows is
    // what reports EOF or EPIPE.
    return r;
  }
}

ChildIo child_read_some(int fd, void *buf, size_t cap, int timeout_ms,
                        size_t *got) {
  *got = 0;
  int64_t deadline = now_ms() + timeout_ms;
  for (;;) {
    ssize_t n = read(fd, buf, cap);
    if (n > 0) {
      *got = (size_t)n;
      return CHILD_IO_OK;
    }
    if (n == 0)
      return CHILD_IO_EOF;
    if (errno == EINTR)
      continue;
    if (errno != EAGAIN && errno != EWOULDBLOCK)
      return CHILD_IO_ERROR;

    int r = wait_ready(fd, POLLIN, deadline);
    if (r < 0)
      return CHILD_IO_ERROR;
    if (r == 0)
      return CHILD_IO_TIMEOUT;
  }
}

ChildIo child_read_exact(int fd, void *buf, size_t len, int timeout_ms,
                         size_t *got) {
  uint8_t *p = (uint8_t *)buf;
  size_t total = 0;
  int64_t deadline = now_ms() + timeout_ms;
  ChildIo result = CHILD_IO_OK;

  while (total < len) {
    int64_t left = deadline - now_ms();
    if (left < 0)
      left = 0;
    size_t n = 0;
    result = child_read_some(fd, p + total, len - total, (int)left, &n);
    if (result != CHILD_IO_OK)
      break;
    total += n;
  }

  if (got)
    *got = total;
  return total == len ? CHILD_IO_OK : result;
}

ChildIo child_write_all(int fd, const void *buf, size_t len, int timeout_ms) {
  const uint8_t *p = (const uint8_t *)buf;
  size_t total = 0;
  int64_t deadline = now_ms() + timeout_ms;

  while (total < len) {
    ssize_t n = write(fd, p + total, len - total);
    if (n > 0) {
      total += (size_t)n;
      continue;
    }
    if (n < 0 && errno == EINTR)
      continue;
    if (n < 0 && errno == EPIPE)
      return CHILD_IO_EOF;
    if (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK)
      return CHILD_IO_ERROR;

    int r = wait_ready(fd, POLLOUT, deadline);
    if (r < 0)
      return CHILD_IO_ERROR;
    if (r == 0)
      return CHILD_IO_TIMEOUT;
  }
  return CHILD_IO_OK;
}

size_t child_read_all(int fd, char *out, size_t cap, int timeout_ms) {
  size_t total = 0;
  int64_t deadline = now_ms() + timeout_ms;
  char discard[512];

  for (;;) {
    int64_t left = deadline - now_ms();
    if (left <= 0)
      break;
    size_t room = total + 1 < cap ? cap - 1 - total : 0;
    size_t n = 0;
    ChildIo r = room ? child_read_some(fd, out + total, room, (int)left, &n)
                     : child_read_some(fd, discard, sizeof discard,
                                       (int)left, &n);
    if (r != CHILD_IO_OK)
      break;
    if (room)
      total += n;
  }

  if (cap)
    out[total] = '\0';
  return total;
}

bool child_set_pipe_size(int fd, int bytes) {
#if defined(F_SETPIPE_SZ)
  return fcntl(fd, F_SETPIPE_SZ, bytes) >= bytes;
#else
  (void)fd;
  (void)bytes;
  return false;
#endif
}

bool child_wait(ChildProc *c, int timeout_ms, int *status) {
  if (c->pid <= 0)
    return false;

  int64_t deadline = now_ms() + timeout_ms;
  for (;;) {
    int st = 0;
    pid_t r = waitpid(c->pid, &st, WNOHANG);
    if (r == c->pid || (r < 0 && errno == ECHILD)) {
      // ECHILD: already reaped elsewhere (or never ours). Either way there
      // is nothing left to wait for.
      if (status)
        *status = r == c->pid ? st : 0;
      c->pid = 0;
      return true;
    }
    if (r < 0 && errno != EINTR)
      return false;
    if (now_ms() >= deadline)
      return false;
    sleep_ms(5);
  }
}

// The group if the child leads one (it always should), else the child.
static void signal_child(const ChildProc *c, int sig) {
  if (kill(-c->pid, sig) < 0)
    kill(c->pid, sig);
}

int child_stop(ChildProc *c, int grace_ms) {
  int status = -1;
  bool had_stdin = c->in_fd >= 0;

  close_fd(&c->in_fd);
  if (c->pid > 0 && had_stdin && child_wait(c, grace_ms, &status)) {
    close_fd(&c->out_fd);
    close_fd(&c->err_fd);
    return status;
  }

  close_fd(&c->out_fd);
  close_fd(&c->err_fd);
  if (c->pid <= 0)
    return status;

  // A child that already finished its output is usually in the middle of
  // exiting: the caller saw EOF because the exit closed the pipe. A moment
  // to finish keeps a clean exit from being reported as killed. Closing
  // stdout also lets a child that is still writing die of SIGPIPE here.
  int settle_ms = grace_ms < stop_settle_ms ? grace_ms : stop_settle_ms;
  if (child_wait(c, settle_ms, &status))
    return status;

  signal_child(c, SIGTERM);
  if (child_wait(c, grace_ms, &status))
    return status;

  signal_child(c, SIGKILL);
  pid_t pid = c->pid;
  while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {
  }
  c->pid = 0;
  return status;
}
