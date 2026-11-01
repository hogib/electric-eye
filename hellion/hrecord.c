#define _POSIX_C_SOURCE 200809L

#include "hrecord.h"
#include "child_proc.h"
#include "fbus.h"
#include "ffmpeg.h"
#include "json_out.h"
#include "udp_relay.h"
#include <errno.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

// Below this much free space a recording is not started: it would fill
// the disk within minutes (~1.8 GB/hour at 4 Mbit/s) and the operator
// would not know until surfacing.
static const uint64_t min_free_bytes = 500ull * 1024 * 1024;
// ffmpeg flushes the last fragment and exits on SIGINT; this is how long
// it gets before being killed.
static const int stop_grace_ms = 10000;

struct HRec {
  char dir[256];
  Pipeline *pipe; // NULL: nothing to record here
  char why[160];  // why not, when unavailable

  pthread_mutex_t lock; // everything below
  bool recording;
  bool busy; // a start or stop is in progress
  ChildProc ff;
  int relay_id;
  UdpRelay *relay;
  char sdp[300];
  char file[96];
  char path[400];
  time_t started;
  char error[200];
  char line[256]; // ffmpeg's stderr line being assembled
  size_t line_len;
};

// Passes on what ffmpeg says, except the H.264 parser's complaints while
// a recording joins the stream mid-way (until the first keyframe brings
// its parameter sets: "non-existing PPS", "no frame!" -- dozens of lines
// on every start) and the input's no-packets timeout, which shows up as
// the recording stopping. Called with the lock held.
static void drain_log(HRec *r) {
  if (r->ff.err_fd < 0)
    return;
  char buf[1024];
  for (;;) {
    ssize_t n = read(r->ff.err_fd, buf, sizeof buf);
    if (n <= 0)
      return;
    for (ssize_t i = 0; i < n; i++) {
      if (buf[i] != '\n' && buf[i] != '\r') {
        if (r->line_len < sizeof r->line - 1)
          r->line[r->line_len++] = buf[i];
        continue;
      }
      r->line[r->line_len] = '\0';
      if (r->line_len && strncmp(r->line, "[h264 @", 7) != 0 &&
          !strstr(r->line, "Last message repeated") &&
          !strstr(r->line, "Connection timed out"))
        printf("hellion: recorder: %s\n", r->line);
      r->line_len = 0;
    }
  }
}

static uint64_t disk_free(const char *dir) {
  struct statvfs vs;
  if (statvfs(dir, &vs) != 0)
    return 0;
  return (uint64_t)vs.f_bavail * (uint64_t)vs.f_frsize;
}

static bool mkdirs(const char *path) {
  char buf[256];
  snprintf(buf, sizeof buf, "%s", path);
  for (char *p = buf + 1; *p; p++) {
    if (*p == '/') {
      *p = '\0';
      if (mkdir(buf, 0775) != 0 && errno != EEXIST)
        return false;
      *p = '/';
    }
  }
  return mkdir(buf, 0775) == 0 || errno == EEXIST;
}

HRec *hrec_create(const HellionConfig *cfg, Pipeline *const *pipes,
                          size_t pipe_count) {
  HRec *r = (HRec *)calloc(1, sizeof *r);
  if (!r)
    return NULL;
  pthread_mutex_init(&r->lock, NULL);
  snprintf(r->dir, sizeof r->dir, "%s", cfg->record_dir);
  r->ff = (ChildProc){.pid = 0, .in_fd = -1, .out_fd = -1, .err_fd = -1};
  r->relay_id = -1;

  for (size_t i = 0; i < pipe_count && !r->pipe; i++) {
    const PipelineConfig *pc = pipeline_config(pipes[i]);
    if (!pc || pc->source != SRC_RTP)
      continue;
    if (!cfg->record_from[0] || strcmp(pc->name, cfg->record_from) == 0)
      r->pipe = pipes[i];
  }
  if (!r->pipe)
    snprintf(r->why, sizeof r->why,
             "no RTP source here to record (recording is topside, from the "
             "stream arriving over the tether)");
  else if (!ff_available())
    snprintf(r->why, sizeof r->why, "ffmpeg is not installed");
  return r;
}

// Has the ffmpeg child died on its own? Called with the lock held. Not
// while a stop is in progress: that holds the child and reaps it itself.
static void reap_if_dead(HRec *r) {
  int status;
  if (!r->busy)
    drain_log(r);
  if (!r->recording || r->busy || !child_wait(&r->ff, 0, &status))
    return;
  udp_relay_detach(r->relay, r->relay_id);
  r->relay_id = -1;
  unlink(r->sdp);
  r->recording = false;
  snprintf(r->error, sizeof r->error,
           "the recorder (ffmpeg) exited on its own (status %d); %s was "
           "kept as far as it got",
           WIFEXITED(status) ? WEXITSTATUS(status) : -1, r->file);
  printf("hellion: recording: %s\n", r->error);
  child_stop(&r->ff, 0);
}

bool hrec_start(HRec *r, char *detail, size_t cap) {
  pthread_mutex_lock(&r->lock);
  reap_if_dead(r);
  if (r->why[0] || r->busy || r->recording) {
    snprintf(detail, cap, "%s",
             r->why[0] ? r->why
             : r->busy ? "busy starting or stopping"
                       : "already recording");
    pthread_mutex_unlock(&r->lock);
    return false;
  }
  UdpRelay *relay = pipeline_rtp_relay(r->pipe);
  if (!relay) {
    snprintf(detail, cap, "not receiving yet");
    pthread_mutex_unlock(&r->lock);
    return false;
  }
  if (!mkdirs(r->dir)) {
    snprintf(detail, cap, "cannot create %s: %s", r->dir, strerror(errno));
    pthread_mutex_unlock(&r->lock);
    return false;
  }
  uint64_t free_bytes = disk_free(r->dir);
  if (free_bytes < min_free_bytes) {
    snprintf(detail, cap, "only %llu MB free in %s",
             (unsigned long long)(free_bytes >> 20), r->dir);
    pthread_mutex_unlock(&r->lock);
    return false;
  }

  int port = udp_free_port_pair();
  time_t now = time(NULL);
  struct tm tm;
  localtime_r(&now, &tm);
  strftime(r->file, sizeof r->file, "dive-%Y%m%d-%H%M%S.mp4", &tm);
  snprintf(r->path, sizeof r->path, "%s/%s", r->dir, r->file);
  snprintf(r->sdp, sizeof r->sdp, "%s/.hellion-record-%ld.sdp", fbus_dir(),
           (long)getpid());
  if (!port || !ff_write_sdp(r->sdp, port)) {
    snprintf(detail, cap, "could not set up the recorder's input");
    pthread_mutex_unlock(&r->lock);
    return false;
  }

  FfArgs args;
  ff_recorder_args(r->sdp, r->path, &args);
  if (!child_spawn(&r->ff, "hellion", args.argv,
                   CHILD_NULL_STDIN | CHILD_PIPE_STDERR)) {
    snprintf(detail, cap, "could not start ffmpeg (see log)");
    unlink(r->sdp);
    pthread_mutex_unlock(&r->lock);
    return false;
  }
  char dest[64], err[160];
  snprintf(dest, sizeof dest, "127.0.0.1:%d", port);
  r->relay_id = udp_relay_attach(relay, dest, err, sizeof err);
  if (r->relay_id < 0) {
    child_stop(&r->ff, 1000);
    unlink(r->sdp);
    snprintf(detail, cap, "%s", err);
    pthread_mutex_unlock(&r->lock);
    return false;
  }
  r->relay = relay;
  r->line_len = 0;
  r->recording = true;
  r->started = now;
  r->error[0] = '\0';
  printf("hellion: recording %s from pipeline \"%s\"\n", r->path,
         pipeline_config(r->pipe)->name);
  snprintf(detail, cap, "%s", r->file);
  pthread_mutex_unlock(&r->lock);
  return true;
}

bool hrec_stop(HRec *r, char *detail, size_t cap) {
  pthread_mutex_lock(&r->lock);
  reap_if_dead(r);
  if (!r->recording || r->busy) {
    snprintf(detail, cap, "%s",
             r->busy ? "busy starting or stopping" : "not recording");
    pthread_mutex_unlock(&r->lock);
    return false;
  }
  r->busy = true;
  ChildProc ff = r->ff;
  pthread_mutex_unlock(&r->lock);

  // Outside the lock: ffmpeg may take a moment to write its last fragment,
  // and the page keeps polling meanwhile.
  //
  // Packets keep flowing to it until it has exited: ffmpeg only acts on
  // SIGINT once its input read returns, and with the packets cut off that
  // read waits for the RTP input's own ~20s timeout -- longer than the
  // grace period, so the file would be killed mid-fragment instead.
  child_signal(&ff, SIGINT);
  int status = -1;
  bool exited = false;
  // Keep reading its stderr meanwhile: a child blocked on a full pipe
  // would never exit. (busy keeps everyone else off r->line and r->ff.)
  for (int waited = 0; waited < stop_grace_ms && !exited; waited += 100) {
    exited = child_wait(&ff, 100, &status);
    drain_log(r);
  }
  // ffmpeg exits with 255 when it stops because of a signal, after writing
  // out the file properly; we sent the signal, so that is a clean stop.
  bool clean = exited && WIFEXITED(status) &&
               (WEXITSTATUS(status) == 0 || WEXITSTATUS(status) == 255);
  child_stop(&ff, 1000);

  pthread_mutex_lock(&r->lock);
  udp_relay_detach(r->relay, r->relay_id);
  r->relay_id = -1;
  r->ff = ff;
  unlink(r->sdp);
  r->recording = false;
  r->busy = false;
  struct stat st;
  long long bytes = stat(r->path, &st) == 0 ? (long long)st.st_size : 0;
  if (!clean)
    snprintf(r->error, sizeof r->error,
             "ffmpeg did not finish cleanly (wait status 0x%x); %s may end "
             "early",
             (unsigned)status, r->file);
  printf("hellion: recording stopped: %s (%lld bytes)%s\n", r->path, bytes,
         clean ? "" : " -- ffmpeg did not exit cleanly");
  snprintf(detail, cap, "%s", r->file);
  pthread_mutex_unlock(&r->lock);
  return true;
}

void hrec_status_json(HRec *r, char *out, size_t cap) {
  pthread_mutex_lock(&r->lock);
  reap_if_dead(r);
  char why[340], file[200], err[420];
  json_escape(r->why, why, sizeof why);
  json_escape(r->file, file, sizeof file);
  json_escape(r->error, err, sizeof err);
  long long bytes = 0;
  struct stat st;
  if (r->file[0] && stat(r->path, &st) == 0)
    bytes = (long long)st.st_size;
  double elapsed = r->recording ? difftime(time(NULL), r->started) : 0;
  snprintf(out, cap,
           "{\"available\":%s,\"why\":\"%s\",\"recording\":%s,"
           "\"file\":\"%s\",\"elapsed_s\":%.0f,\"bytes\":%lld,"
           "\"disk_free_bytes\":%llu,\"error\":\"%s\"}",
           r->why[0] ? "false" : "true", why,
           r->recording ? "true" : "false", file, elapsed, bytes,
           (unsigned long long)(r->why[0] ? 0 : disk_free(r->dir)), err);
  pthread_mutex_unlock(&r->lock);
}

void hrec_summary(HRec *r, char *out, size_t cap) {
  out[0] = '\0';
  if (!r)
    return;
  pthread_mutex_lock(&r->lock);
  reap_if_dead(r);
  if (r->recording) {
    struct stat st;
    long long mb = stat(r->path, &st) == 0 ? (long long)(st.st_size >> 20) : 0;
    snprintf(out, cap, "recording %s, %.0fs, %lld MB", r->file,
             difftime(time(NULL), r->started), mb);
  } else if (r->error[0]) {
    snprintf(out, cap, "recording stopped: %s", r->error);
  }
  pthread_mutex_unlock(&r->lock);
}

void hrec_destroy(HRec *r) {
  if (!r)
    return;
  char detail[128];
  if (r->recording)
    hrec_stop(r, detail, sizeof detail);
  pthread_mutex_destroy(&r->lock);
  free(r);
}
