// _GNU_SOURCE: pipe2().
#define _GNU_SOURCE

#include "hlog.h"
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static int journal_fd = -1; // where stdout went before capture
static int pipe_read = -1;
static pthread_t reader;
static bool running = false;

static pthread_mutex_t ring_lock = PTHREAD_MUTEX_INITIALIZER;
static char ring[hlog_lines][hlog_line_len];
static uint64_t ring_total; // lines ever added; slot = total % hlog_lines

// The line being assembled from pipe reads, which can split anywhere.
static char partial[hlog_line_len];
static size_t partial_len;

static void add_line(const char *text, size_t len) {
  // Drop the trailing carriage returns ffmpeg-style progress output uses.
  while (len > 0 && (text[len - 1] == '\r' || text[len - 1] == '\n'))
    len--;
  if (len == 0)
    return;

  char stamp[16];
  time_t now = time(NULL);
  struct tm tm;
  localtime_r(&now, &tm);
  strftime(stamp, sizeof stamp, "%H:%M:%S ", &tm);

  pthread_mutex_lock(&ring_lock);
  char *slot = ring[ring_total % hlog_lines];
  snprintf(slot, hlog_line_len, "%s%.*s", stamp, (int)len, text);
  ring_total++;
  pthread_mutex_unlock(&ring_lock);
}

static void take_bytes(const char *buf, size_t n) {
  for (size_t i = 0; i < n; i++) {
    char ch = buf[i];
    if (ch == '\n' || ch == '\r') {
      add_line(partial, partial_len);
      partial_len = 0;
    } else if (partial_len < sizeof partial - 1) {
      partial[partial_len++] = ch;
    }
    // Past the line length the rest is dropped from the ring's copy only;
    // the journal still gets every byte.
  }
}

static void write_all(int fd, const char *buf, size_t n) {
  while (n > 0) {
    ssize_t w = write(fd, buf, n);
    if (w < 0 && errno == EINTR)
      continue;
    if (w <= 0)
      return; // the journal went away; keep the ring going regardless
    buf += w;
    n -= (size_t)w;
  }
}

static void *reader_loop(void *arg) {
  (void)arg;
  char buf[4096];
  for (;;) {
    ssize_t n = read(pipe_read, buf, sizeof buf);
    if (n < 0 && errno == EINTR)
      continue;
    if (n <= 0)
      break; // every writer closed: hlog_stop()
    write_all(journal_fd, buf, (size_t)n);
    take_bytes(buf, (size_t)n);
  }
  if (partial_len)
    add_line(partial, partial_len);
  return NULL;
}

bool hlog_start(void) {
  int p[2];
  if (pipe2(p, O_CLOEXEC) != 0)
    return false;
  fflush(stdout);
  fflush(stderr);
  journal_fd = fcntl(STDOUT_FILENO, F_DUPFD_CLOEXEC, 3);
  if (journal_fd < 0) {
    close(p[0]);
    close(p[1]);
    return false;
  }
  pipe_read = p[0];
  if (pthread_create(&reader, NULL, reader_loop, NULL) != 0) {
    close(p[0]);
    close(p[1]);
    close(journal_fd);
    journal_fd = -1;
    return false;
  }
  // dup2 clears close-on-exec on the copies, so children inherit them as
  // their stdout/stderr (child_proc.c keeps 0-2) and their output lands here.
  dup2(p[1], STDOUT_FILENO);
  dup2(p[1], STDERR_FILENO);
  close(p[1]);
  // stdout is a pipe now, which stdio would fully buffer: lines would reach
  // the journal (and top) only in 4KB lumps.
  setvbuf(stdout, NULL, _IOLBF, 0);
  running = true;
  return true;
}

size_t hlog_snapshot(char (*lines)[hlog_line_len], size_t max,
                     uint64_t *total) {
  pthread_mutex_lock(&ring_lock);
  uint64_t have = ring_total < hlog_lines ? ring_total : hlog_lines;
  size_t n = have < max ? (size_t)have : max;
  for (size_t i = 0; i < n; i++) {
    uint64_t idx = ring_total - n + i;
    memcpy(lines[i], ring[idx % hlog_lines], hlog_line_len);
  }
  if (total)
    *total = ring_total;
  pthread_mutex_unlock(&ring_lock);
  return n;
}

void hlog_stop(void) {
  if (!running)
    return;
  running = false;
  fflush(stdout);
  fflush(stderr);
  // Point stdout/stderr back at the journal; that closes the pipe's last
  // write ends, so the reader drains what is left and sees EOF.
  dup2(journal_fd, STDOUT_FILENO);
  dup2(journal_fd, STDERR_FILENO);
  pthread_join(reader, NULL);
  close(pipe_read);
  close(journal_fd);
  pipe_read = journal_fd = -1;
}
