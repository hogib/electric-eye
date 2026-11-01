#define _POSIX_C_SOURCE 200809L

#include "hstatus.h"
#include "fbus.h"
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

static const char hs_magic[8] = {'H', 'E', 'L', 'L', 'I', 'O', 'N', '1'};

struct HsWriter {
  char path[PATH_MAX];
  HsStatus *map;
  dev_t dev;
  ino_t ino;
};

static uint64_t clock_ns(clockid_t clk) {
  struct timespec ts;
  clock_gettime(clk, &ts);
  return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

static void status_path(const char *name, char *out, size_t cap) {
  snprintf(out, cap, "%s/hellion.%s", fbus_dir(), name);
}

HsWriter *hs_writer_open(const char *name) {
  HsWriter *w = (HsWriter *)calloc(1, sizeof *w);
  if (!w)
    return NULL;
  status_path(name, w->path, sizeof w->path);

  // Same replace-by-rename as the frame bus, so a restarted hellion never
  // shares a file with the old one.
  char tmp[PATH_MAX];
  snprintf(tmp, sizeof tmp, "%s/.hellion.%s.%ld.tmp", fbus_dir(), name,
           (long)getpid());
  unlink(tmp);
  int fd = open(tmp, O_RDWR | O_CREAT | O_EXCL | O_CLOEXEC, 0660);
  if (fd < 0 || fchmod(fd, 0660) != 0 ||
      ftruncate(fd, (off_t)sizeof(HsStatus)) != 0) {
    printf("hellion: cannot create status file %s: %s (top and check will "
           "not see this instance)\n",
           tmp, strerror(errno));
    if (fd >= 0)
      close(fd);
    unlink(tmp);
    free(w);
    return NULL;
  }
  struct stat st;
  fstat(fd, &st);
  w->dev = st.st_dev;
  w->ino = st.st_ino;
  w->map = (HsStatus *)mmap(NULL, sizeof(HsStatus), PROT_READ | PROT_WRITE,
                            MAP_SHARED, fd, 0);
  close(fd);
  if (w->map == MAP_FAILED || rename(tmp, w->path) != 0) {
    printf("hellion: cannot publish status file %s: %s\n", w->path,
           strerror(errno));
    if (w->map != MAP_FAILED)
      munmap(w->map, sizeof(HsStatus));
    unlink(tmp);
    free(w);
    return NULL;
  }
  return w;
}

void hs_writer_publish(HsWriter *w, HsStatus *snap) {
  if (!w)
    return;
  memcpy(snap->magic, hs_magic, sizeof hs_magic);
  snap->version = hs_version;
  snap->struct_size = sizeof(HsStatus);
  snap->pid = (int32_t)getpid();
  snap->heartbeat_mono_ns = clock_ns(CLOCK_MONOTONIC);
  snap->log_count =
      (uint32_t)hlog_snapshot(snap->log, hlog_lines, &snap->log_total);

  HsStatus *m = w->map;
  uint64_t seq = __atomic_load_n(&m->seq, __ATOMIC_RELAXED);
  __atomic_store_n(&m->seq, seq + 1, __ATOMIC_RELAXED); // odd: writing
  __atomic_thread_fence(__ATOMIC_RELEASE);
  // Everything but the seq field itself.
  memcpy(m->magic, snap->magic, offsetof(HsStatus, seq));
  memcpy((char *)m + offsetof(HsStatus, pid), (char *)snap + offsetof(HsStatus, pid),
         sizeof(HsStatus) - offsetof(HsStatus, pid));
  __atomic_store_n(&m->seq, seq + 2, __ATOMIC_RELEASE);
}

void hs_writer_close(HsWriter *w) {
  if (!w)
    return;
  struct stat st;
  if (stat(w->path, &st) == 0 && st.st_dev == w->dev && st.st_ino == w->ino)
    unlink(w->path);
  munmap(w->map, sizeof(HsStatus));
  free(w);
}

HsReadResult hs_read(const char *name, HsStatus *out) {
  char path[PATH_MAX];
  status_path(name, path, sizeof path);
  int fd = open(path, O_RDONLY | O_CLOEXEC);
  if (fd < 0)
    return errno == ENOENT ? HS_READ_MISSING : HS_READ_ERROR;
  struct stat st;
  if (fstat(fd, &st) != 0 || (size_t)st.st_size != sizeof(HsStatus)) {
    close(fd);
    return HS_READ_MISMATCH;
  }
  const HsStatus *m = (const HsStatus *)mmap(NULL, sizeof(HsStatus),
                                             PROT_READ, MAP_SHARED, fd, 0);
  close(fd);
  if (m == MAP_FAILED)
    return HS_READ_ERROR;

  HsReadResult res = HS_READ_BUSY;
  for (int attempt = 0; attempt < 50; attempt++) {
    uint64_t s1 = __atomic_load_n(&m->seq, __ATOMIC_ACQUIRE);
    if (s1 & 1) {
      struct timespec ts = {0, 1000000L};
      nanosleep(&ts, NULL);
      continue;
    }
    memcpy(out, m, sizeof *out);
    __atomic_thread_fence(__ATOMIC_ACQUIRE);
    if (__atomic_load_n(&m->seq, __ATOMIC_RELAXED) != s1)
      continue;
    res = memcmp(out->magic, hs_magic, sizeof hs_magic) == 0 &&
                  out->version == hs_version &&
                  out->struct_size == sizeof(HsStatus)
              ? HS_READ_OK
              : HS_READ_MISMATCH;
    break;
  }
  munmap((void *)m, sizeof(HsStatus));
  if (res == HS_READ_OK) {
    // Defend the reader against a corrupt file: strings end in the struct.
    out->name[sizeof out->name - 1] = '\0';
    out->role[sizeof out->role - 1] = '\0';
    out->eeye_config[sizeof out->eeye_config - 1] = '\0';
    if (out->pipeline_count > hs_max_pipelines)
      out->pipeline_count = hs_max_pipelines;
    if (out->log_count > hlog_lines)
      out->log_count = hlog_lines;
    for (uint32_t i = 0; i < hs_max_pipelines; i++) {
      HsPipeline *p = &out->pipelines[i];
      p->name[sizeof p->name - 1] = '\0';
      if (p->sink_count > hs_max_sinks)
        p->sink_count = hs_max_sinks;
      HsEndpoint *eps[1 + hs_max_sinks];
      eps[0] = &p->source;
      for (int s = 0; s < hs_max_sinks; s++)
        eps[1 + s] = &p->sinks[s];
      for (int e = 0; e < 1 + hs_max_sinks; e++) {
        eps[e]->spec[hs_spec_len - 1] = '\0';
        eps[e]->state[hs_state_len - 1] = '\0';
        eps[e]->detail[hs_detail_len - 1] = '\0';
      }
    }
    for (uint32_t i = 0; i < hlog_lines; i++)
      out->log[i][hlog_line_len - 1] = '\0';
  }
  return res;
}

double hs_heartbeat_age(const HsStatus *s) {
  uint64_t now = clock_ns(CLOCK_MONOTONIC);
  return now > s->heartbeat_mono_ns
             ? (double)(now - s->heartbeat_mono_ns) / 1e9
             : 0.0;
}

int hs_list(char (*names)[hs_name_len], int max) {
  DIR *d = opendir(fbus_dir());
  if (!d)
    return 0;
  int n = 0;
  struct dirent *e;
  while ((e = readdir(d)) != NULL && n < max) {
    if (strncmp(e->d_name, "hellion.", 8) != 0)
      continue;
    const char *name = e->d_name + 8;
    if (!fbus_valid_name(name))
      continue;
    snprintf(names[n++], hs_name_len, "%s", name);
  }
  closedir(d);
  return n;
}
