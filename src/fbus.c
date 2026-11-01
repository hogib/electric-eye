// _GNU_SOURCE before any header include: the futex syscall number and
// posix_fallocate/clock_gettime under strict ISO mode (-std=c99).
#define _GNU_SOURCE

#include "fbus.h"
#include "compat.h"
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <linux/futex.h>
#include <pthread.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>

// ---- The file layout (docs/bus.md is the reader's reference) -------------
//
// [ header: 4096 bytes ][ slot 0 ][ slot 1 ] ... [ slot N-1 ]
// slot: [ slot header: 64 bytes ][ Y ][ U ][ V ], padded to a page.
//
// Every field is native-endian: the bus never leaves the machine.

static const char fbus_magic[8] = {'E', 'E', 'Y', 'E', 'B', 'U', 'S', '1'};
enum {
  fbus_version = 1,
  fbus_header_bytes = 4096,
  fbus_slot_header_bytes = 64,
  // Frames in the ring. A reader is only ever after the newest frame, so a
  // few suffice: they are what lets a reader finish copying one frame
  // while the writer moves on to the next without tearing it.
  fbus_slot_count = 4,
  fbus_page = 4096,
};
// V4L2_PIX_FMT_YUV422P, spelled out so the header needs no kernel headers.
static const uint32_t fbus_fourcc_422p = 0x50323234u; // '4','2','2','P'
static const uint32_t fbus_flag_closed = 1u << 0;

// How often the writer's heartbeat ticks, and how old it may get before a
// reader calls the writer gone.
static const int heartbeat_period_ms = 500;
static const uint64_t heartbeat_stale_ns = 3000000000ull;
// How often a reader rechecks that the file is still the live one.
static const uint64_t alive_check_period_ns = 500000000ull;

typedef struct {
  char magic[8];               // 0   "EEYEBUS1"
  uint32_t version;            // 8
  uint32_t header_bytes;       // 12
  uint64_t generation;         // 16  differs for every writer run
  uint32_t width;              // 24
  uint32_t height;             // 28
  uint32_t fourcc;             // 32  '422P'
  uint32_t full_range;         // 36  1: Y and UV use 0-255
  uint32_t plane_offset[3];    // 40  from the start of a slot's image data
  uint32_t plane_stride[3];    // 52
  uint32_t plane_bytes[3];     // 64
  uint32_t slot_count;         // 76
  uint32_t slot_header_bytes;  // 80
  uint32_t reserved0;          // 84
  uint64_t slot_stride;        // 88  bytes from one slot to the next
  uint64_t slots_offset;       // 96  file offset of slot 0
  int32_t writer_pid;          // 104
  uint32_t flags;              // 108 bit 0: closed by its writer
  uint64_t created_real_ns;    // 112
  uint8_t pad0[8];             // 120
  // Second cache line: the fields that change every frame.
  uint32_t futex_word;         // 128 low 32 bits of write_seq; readers wait on it
  uint32_t reserved1;          // 132
  uint64_t write_seq;          // 136 frames published; frame n is in slot (n-1) % slot_count
  uint64_t last_publish_mono_ns; // 144
  uint64_t heartbeat_mono_ns;  // 152
} FbusHeader;

typedef struct {
  uint64_t seq;             // 0   2n once frame n is complete; odd while being written
  int64_t frame_no;         // 8
  uint64_t capture_mono_ns; // 16
  uint64_t capture_real_ns; // 24
  uint64_t publish_mono_ns; // 32
  uint64_t reserved[3];     // 40
} FbusSlotHeader;

// The layout is a contract with readers in other languages: pin it.
STATIC_ASSERT(offsetof(FbusHeader, version) == 8, "fbus header layout");
STATIC_ASSERT(offsetof(FbusHeader, generation) == 16, "fbus header layout");
STATIC_ASSERT(offsetof(FbusHeader, plane_offset) == 40, "fbus header layout");
STATIC_ASSERT(offsetof(FbusHeader, slot_count) == 76, "fbus header layout");
STATIC_ASSERT(offsetof(FbusHeader, slot_stride) == 88, "fbus header layout");
STATIC_ASSERT(offsetof(FbusHeader, writer_pid) == 104, "fbus header layout");
STATIC_ASSERT(offsetof(FbusHeader, created_real_ns) == 112, "fbus header layout");
STATIC_ASSERT(offsetof(FbusHeader, futex_word) == 128, "fbus header layout");
STATIC_ASSERT(offsetof(FbusHeader, write_seq) == 136, "fbus header layout");
STATIC_ASSERT(offsetof(FbusHeader, heartbeat_mono_ns) == 152, "fbus header layout");
STATIC_ASSERT(sizeof(FbusHeader) <= fbus_header_bytes, "fbus header size");
STATIC_ASSERT(sizeof(FbusSlotHeader) == fbus_slot_header_bytes, "fbus slot header");

// ---- Small helpers ------------------------------------------------------

static uint64_t clock_ns(clockid_t clk) {
  struct timespec ts;
  clock_gettime(clk, &ts);
  return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

static uint64_t mono_ns(void) { return clock_ns(CLOCK_MONOTONIC); }

const char *fbus_dir(void) {
  const char *d = getenv("EEYE_BUS_DIR");
  return d && d[0] ? d : "/dev/shm";
}

bool fbus_valid_name(const char *name) {
  size_t n = strlen(name);
  if (n == 0 || n >= fbus_max_stream_name)
    return false;
  for (size_t i = 0; i < n; i++) {
    char c = name[i];
    if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
          (c >= '0' && c <= '9') || c == '-' || c == '_'))
      return false;
  }
  return true;
}

static void stream_path(const char *name, char *out, size_t cap) {
  snprintf(out, cap, "%s/eeye.%s", fbus_dir(), name);
}

static int futex(uint32_t *addr, int op, uint32_t val,
                 const struct timespec *timeout) {
  return (int)syscall(SYS_futex, addr, op, val, timeout, NULL, 0);
}

static uint64_t round_up(uint64_t v, uint64_t to) {
  return (v + to - 1) / to * to;
}

static FbusSlotHeader *slot_at(uint8_t *base, const FbusHeader *h,
                               uint64_t index) {
  return (FbusSlotHeader *)(base + h->slots_offset + index * h->slot_stride);
}

// ---- Writer -------------------------------------------------------------

struct FbusWriter {
  char name[fbus_max_stream_name];
  char path[PATH_MAX];
  uint8_t *base;
  size_t size;
  FbusHeader *hdr;
  dev_t dev;
  ino_t ino;
  uint64_t seq; // frames published (mirrors hdr->write_seq)
  // The layout this writer created, kept here rather than read back from
  // the shared header on every frame: anything in the stream's group can
  // write to that file, and a corrupted header must not be able to steer
  // the writer's copies out of bounds.
  size_t plane_offset[3], plane_bytes[3];
  size_t slots_offset, slot_stride, slot_header_bytes;
  uint32_t slot_count;
  // Which slots already hold all-128 chroma (fbus_writer_publish_frame).
  bool slot_neutral[fbus_slot_count];

  pthread_t heartbeat;
  bool heartbeat_started;
  bool stop; // atomic (see compat.h)
};

static void *heartbeat_loop(void *arg) {
  FbusWriter *w = (FbusWriter *)arg;
  while (!__atomic_load_n(&w->stop, __ATOMIC_ACQUIRE)) {
    __atomic_store_n(&w->hdr->heartbeat_mono_ns, mono_ns(), __ATOMIC_RELEASE);
    for (int waited = 0; waited < heartbeat_period_ms &&
                         !__atomic_load_n(&w->stop, __ATOMIC_ACQUIRE);
         waited += 50) {
      struct timespec ts = {0, 50000000L};
      nanosleep(&ts, NULL);
    }
  }
  return NULL;
}

FbusWriter *fbus_writer_open(const char *name, uint32_t width,
                             uint32_t height) {
  if (!fbus_valid_name(name)) {
    printf("fbus: invalid stream name \"%s\" (letters, digits, '-' and '_', "
           "under %d characters)\n",
           name, fbus_max_stream_name);
    return NULL;
  }
  if (width == 0 || height == 0) {
    printf("fbus: stream \"%s\": zero geometry %ux%u\n", name, width, height);
    return NULL;
  }

  FbusWriter *w = (FbusWriter *)calloc(1, sizeof *w);
  if (!w)
    return NULL;
  snprintf(w->name, sizeof w->name, "%s", name);
  stream_path(name, w->path, sizeof w->path);

  const uint32_t cw = (width + 1) / 2;
  const uint64_t y_bytes = (uint64_t)width * height;
  const uint64_t c_bytes = (uint64_t)cw * height;
  const uint64_t slot_stride = round_up(
      fbus_slot_header_bytes + y_bytes + 2 * c_bytes, fbus_page);
  w->size = (size_t)(fbus_header_bytes + fbus_slot_count * slot_stride);

  // Built under a temporary name and renamed into place, so a reader never
  // opens a half-initialized file, and a restarted writer replaces the old
  // file in one step.
  char tmp[PATH_MAX];
  snprintf(tmp, sizeof tmp, "%s/.eeye.%s.%ld.tmp", fbus_dir(), name,
           (long)getpid());
  unlink(tmp); // a leftover from a crashed run with the same pid
  int fd = open(tmp, O_RDWR | O_CREAT | O_EXCL | O_CLOEXEC, 0660);
  if (fd < 0) {
    printf("fbus: cannot create %s: %s\n", tmp, strerror(errno));
    free(w);
    return NULL;
  }
  // Exactly 0660 whatever the umask: readers run as other users in the
  // same group (the service's Group=video).
  fchmod(fd, 0660);

  // Reserve the memory now, so a full /dev/shm fails here with a clear
  // message instead of as a SIGBUS on some later frame. Filesystems
  // without fallocate get a plain (sparse) truncate.
  int fa = posix_fallocate(fd, 0, (off_t)w->size);
  if (fa != 0 && (fa == EOPNOTSUPP || fa == EINVAL)) {
    fa = ftruncate(fd, (off_t)w->size) == 0 ? 0 : errno;
  }
  if (fa != 0) {
    printf("fbus: cannot size %s to %zu bytes: %s (is %s full?)\n", tmp,
           w->size, strerror(fa), fbus_dir());
    close(fd);
    unlink(tmp);
    free(w);
    return NULL;
  }

  w->base = (uint8_t *)mmap(NULL, w->size, PROT_READ | PROT_WRITE,
                            MAP_SHARED, fd, 0);
  struct stat st;
  bool stat_ok = fstat(fd, &st) == 0;
  close(fd);
  if (w->base == MAP_FAILED || !stat_ok) {
    printf("fbus: cannot map %s: %s\n", tmp, strerror(errno));
    if (w->base != MAP_FAILED)
      munmap(w->base, w->size);
    unlink(tmp);
    free(w);
    return NULL;
  }
  w->dev = st.st_dev;
  w->ino = st.st_ino;

  FbusHeader *h = (FbusHeader *)w->base;
  w->hdr = h;
  memset(h, 0, sizeof *h);
  h->version = fbus_version;
  h->header_bytes = fbus_header_bytes;
  h->generation = clock_ns(CLOCK_REALTIME) ^ ((uint64_t)getpid() << 40);
  h->width = width;
  h->height = height;
  h->fourcc = fbus_fourcc_422p;
  h->full_range = 1;
  h->plane_offset[0] = 0;
  h->plane_offset[1] = (uint32_t)y_bytes;
  h->plane_offset[2] = (uint32_t)(y_bytes + c_bytes);
  h->plane_stride[0] = width;
  h->plane_stride[1] = cw;
  h->plane_stride[2] = cw;
  h->plane_bytes[0] = (uint32_t)y_bytes;
  h->plane_bytes[1] = (uint32_t)c_bytes;
  h->plane_bytes[2] = (uint32_t)c_bytes;
  h->slot_count = fbus_slot_count;
  h->slot_header_bytes = fbus_slot_header_bytes;
  h->slot_stride = slot_stride;
  h->slots_offset = fbus_header_bytes;
  for (int p = 0; p < 3; p++) {
    w->plane_offset[p] = h->plane_offset[p];
    w->plane_bytes[p] = h->plane_bytes[p];
  }
  w->slots_offset = h->slots_offset;
  w->slot_stride = h->slot_stride;
  w->slot_header_bytes = h->slot_header_bytes;
  w->slot_count = h->slot_count;
  h->writer_pid = (int32_t)getpid();
  h->created_real_ns = clock_ns(CLOCK_REALTIME);
  h->heartbeat_mono_ns = mono_ns();
  // The magic goes last: a reader that sees it sees a complete header.
  __atomic_thread_fence(__ATOMIC_RELEASE);
  memcpy(h->magic, fbus_magic, sizeof fbus_magic);

  if (rename(tmp, w->path) != 0) {
    printf("fbus: cannot publish %s as %s: %s\n", tmp, w->path,
           strerror(errno));
    munmap(w->base, w->size);
    unlink(tmp);
    free(w);
    return NULL;
  }

  w->heartbeat_started =
      pthread_create(&w->heartbeat, NULL, heartbeat_loop, w) == 0;
  if (!w->heartbeat_started)
    printf("fbus: stream \"%s\": could not start its heartbeat; readers "
           "will think it stopped whenever no frames flow\n",
           name);

  printf("fbus: publishing \"%s\" at %ux%u (%s)\n", name, width, height,
         w->path);
  return w;
}

void fbus_writer_publish(FbusWriter *w, const uint8_t *const planes[3],
                         int64_t frame_no, uint64_t capture_mono_ns) {
  fbus_writer_publish_frame(w, planes, frame_no, capture_mono_ns, false);
}

void fbus_writer_publish_frame(FbusWriter *w, const uint8_t *const planes[3],
                               int64_t frame_no, uint64_t capture_mono_ns,
                               bool chroma_neutral) {
  if (!w)
    return;
  FbusHeader *h = w->hdr;
  uint64_t n = w->seq + 1;
  uint32_t k = (uint32_t)((n - 1) % w->slot_count);
  FbusSlotHeader *s =
      (FbusSlotHeader *)(w->base + w->slots_offset + k * w->slot_stride);
  uint8_t *data = (uint8_t *)s + w->slot_header_bytes;

  // Seqlock write: odd while the slot is inconsistent. The release fence
  // keeps the odd value from being reordered after the copies.
  __atomic_store_n(&s->seq, 2 * n - 1, __ATOMIC_RELAXED);
  __atomic_thread_fence(__ATOMIC_RELEASE);

  memcpy(data + w->plane_offset[0], planes[0], w->plane_bytes[0]);
  if (!chroma_neutral) {
    memcpy(data + w->plane_offset[1], planes[1], w->plane_bytes[1]);
    memcpy(data + w->plane_offset[2], planes[2], w->plane_bytes[2]);
    w->slot_neutral[k] = false;
  } else if (!w->slot_neutral[k]) {
    // An edge map's chroma is all 128: written once per slot, then left.
    // Half the bytes of a 4:2:2 frame not copied, every frame after.
    memset(data + w->plane_offset[1], 128, w->plane_bytes[1]);
    memset(data + w->plane_offset[2], 128, w->plane_bytes[2]);
    w->slot_neutral[k] = true;
  }

  uint64_t now = mono_ns();
  s->frame_no = frame_no;
  s->capture_mono_ns = capture_mono_ns;
  s->capture_real_ns =
      capture_mono_ns ? clock_ns(CLOCK_REALTIME) - (now - capture_mono_ns) : 0;
  s->publish_mono_ns = now;

  __atomic_store_n(&s->seq, 2 * n, __ATOMIC_RELEASE);
  __atomic_store_n(&h->write_seq, n, __ATOMIC_RELEASE);
  __atomic_store_n(&h->last_publish_mono_ns, now, __ATOMIC_RELAXED);
  __atomic_store_n(&h->futex_word, (uint32_t)n, __ATOMIC_RELEASE);
  futex(&h->futex_word, FUTEX_WAKE, INT_MAX, NULL);
  w->seq = n;
}

void fbus_writer_close(FbusWriter *w) {
  if (!w)
    return;
  __atomic_store_n(&w->stop, true, __ATOMIC_RELEASE);
  if (w->heartbeat_started)
    pthread_join(w->heartbeat, NULL);

  __atomic_fetch_or(&w->hdr->flags, fbus_flag_closed, __ATOMIC_RELEASE);
  // Wake any reader waiting for a frame so it sees the close at once.
  __atomic_fetch_add(&w->hdr->futex_word, 1u << 31, __ATOMIC_RELEASE);
  futex(&w->hdr->futex_word, FUTEX_WAKE, INT_MAX, NULL);

  // Only remove the file if it is still ours: a restarted writer may
  // already have renamed its own over this path.
  struct stat st;
  if (stat(w->path, &st) == 0 && st.st_dev == w->dev && st.st_ino == w->ino)
    unlink(w->path);

  munmap(w->base, w->size);
  free(w);
}

// ---- Reader -------------------------------------------------------------

struct FbusReader {
  char path[PATH_MAX];
  uint8_t *base;
  size_t size;
  const FbusHeader *hdr;
  dev_t dev;
  ino_t ino;
  uint64_t last_seq;       // 0 = nothing read yet
  uint64_t next_alive_check_ns;
  bool gone;
  bool futex_works;        // false: fall back to short sleeps
};

const char *fbus_open_result_str(FbusOpenResult r) {
  switch (r) {
  case FBUS_OPEN_OK: return "ok";
  case FBUS_OPEN_MISSING: return "no such stream";
  case FBUS_OPEN_DENIED: return "permission denied";
  case FBUS_OPEN_BAD_FORMAT: return "not a frame bus stream (or a newer version)";
  case FBUS_OPEN_STALE: return "its writer is not running";
  case FBUS_OPEN_ERROR: break;
  }
  return "error";
}

static bool header_stale(const FbusHeader *h, uint64_t now) {
  if (__atomic_load_n(&h->flags, __ATOMIC_ACQUIRE) & fbus_flag_closed)
    return true;
  uint64_t beat = __atomic_load_n(&h->heartbeat_mono_ns, __ATOMIC_ACQUIRE);
  return beat < now && now - beat > heartbeat_stale_ns;
}

// Maps and validates a stream file. On success *base/*size/*st describe
// the mapping.
static FbusOpenResult map_stream(const char *path, uint8_t **base,
                                 size_t *size, struct stat *st) {
  int fd = open(path, O_RDONLY | O_CLOEXEC);
  if (fd < 0)
    return errno == ENOENT ? FBUS_OPEN_MISSING
           : errno == EACCES || errno == EPERM ? FBUS_OPEN_DENIED
                                               : FBUS_OPEN_ERROR;
  if (fstat(fd, st) != 0 || st->st_size < fbus_header_bytes) {
    close(fd);
    return FBUS_OPEN_BAD_FORMAT;
  }
  *size = (size_t)st->st_size;
  *base = (uint8_t *)mmap(NULL, *size, PROT_READ, MAP_SHARED, fd, 0);
  close(fd);
  if (*base == MAP_FAILED)
    return FBUS_OPEN_ERROR;

  const FbusHeader *h = (const FbusHeader *)*base;
  __atomic_thread_fence(__ATOMIC_ACQUIRE);
  bool ok = memcmp(h->magic, fbus_magic, sizeof fbus_magic) == 0 &&
            h->version == fbus_version &&
            h->header_bytes == fbus_header_bytes &&
            h->fourcc == fbus_fourcc_422p && h->slot_count > 0 &&
            h->slot_header_bytes == fbus_slot_header_bytes &&
            h->slots_offset + (uint64_t)h->slot_count * h->slot_stride <=
                (uint64_t)*size &&
            (uint64_t)h->plane_offset[2] + h->plane_bytes[2] +
                    fbus_slot_header_bytes <=
                h->slot_stride;
  if (!ok) {
    munmap(*base, *size);
    return FBUS_OPEN_BAD_FORMAT;
  }
  return FBUS_OPEN_OK;
}

FbusReader *fbus_reader_open(const char *name, FbusOpenResult *why) {
  FbusOpenResult dummy;
  if (!why)
    why = &dummy;
  if (!fbus_valid_name(name)) {
    *why = FBUS_OPEN_MISSING;
    return NULL;
  }

  FbusReader *r = (FbusReader *)calloc(1, sizeof *r);
  if (!r) {
    *why = FBUS_OPEN_ERROR;
    return NULL;
  }
  stream_path(name, r->path, sizeof r->path);

  struct stat st;
  *why = map_stream(r->path, &r->base, &r->size, &st);
  if (*why != FBUS_OPEN_OK) {
    free(r);
    return NULL;
  }
  r->hdr = (const FbusHeader *)r->base;
  if (header_stale(r->hdr, mono_ns())) {
    *why = FBUS_OPEN_STALE;
    munmap(r->base, r->size);
    free(r);
    return NULL;
  }
  r->dev = st.st_dev;
  r->ino = st.st_ino;
  r->futex_works = true;
  return r;
}

void fbus_reader_geometry(const FbusReader *r, uint32_t *width,
                          uint32_t *height) {
  *width = r ? r->hdr->width : 0;
  *height = r ? r->hdr->height : 0;
}

// Whether this mapping is still the live stream: not closed, heartbeat
// fresh, and still the file at the stream's path.
static bool still_alive(FbusReader *r, uint64_t now, bool force) {
  if (r->gone)
    return false;
  if (!force && now < r->next_alive_check_ns)
    return true;
  r->next_alive_check_ns = now + alive_check_period_ns;

  struct stat st;
  if (header_stale(r->hdr, now) || stat(r->path, &st) != 0 ||
      st.st_dev != r->dev || st.st_ino != r->ino)
    r->gone = true;
  return !r->gone;
}

// Copies discarded because the writer overwrote the slot mid-copy.
static uint64_t fbus_torn_retries; // atomic (see compat.h)

// Test seam: runs between a reader's copy and its recheck, the exact
// window a lapping writer would hit. Nothing in the shipping build.
#ifndef FBUS_AFTER_COPY_HOOK
#define FBUS_AFTER_COPY_HOOK(slot) ((void)0)
#endif

// One attempt at copying frame n out of its slot. False if the slot no
// longer (or not yet) holds exactly frame n, or changed while copying.
static bool copy_frame(const FbusReader *r, uint64_t n,
                       uint8_t *const planes[3], FbusFrameInfo *info) {
  const FbusHeader *h = r->hdr;
  const FbusSlotHeader *s = slot_at(r->base, h, (n - 1) % h->slot_count);
  const uint8_t *data = (const uint8_t *)s + h->slot_header_bytes;

  uint64_t s1 = __atomic_load_n(&s->seq, __ATOMIC_ACQUIRE);
  if (s1 != 2 * n)
    return false;

  for (int p = 0; p < 3; p++)
    memcpy(planes[p], data + h->plane_offset[p], h->plane_bytes[p]);
  FbusFrameInfo got = {
      .frame_no = s->frame_no,
      .capture_mono_ns = s->capture_mono_ns,
      .capture_real_ns = s->capture_real_ns,
      .publish_mono_ns = s->publish_mono_ns,
  };

  FBUS_AFTER_COPY_HOOK(s);
  __atomic_thread_fence(__ATOMIC_ACQUIRE);
  if (__atomic_load_n(&s->seq, __ATOMIC_RELAXED) != s1) {
    // Torn: the writer lapped us mid-copy. Counted so the tests can show
    // this check really fires, rather than passing because it never had to.
    __atomic_fetch_add(&fbus_torn_retries, 1, __ATOMIC_RELAXED);
    return false;
  }
  if (info)
    *info = got;
  return true;
}

FbusReadResult fbus_reader_next(FbusReader *r, uint8_t *const planes[3],
                                int timeout_ms, FbusFrameInfo *info) {
  const FbusHeader *h = r->hdr;
  const uint64_t deadline = mono_ns() + (uint64_t)timeout_ms * 1000000ull;

  for (;;) {
    uint64_t now = mono_ns();
    uint64_t n = __atomic_load_n(&h->write_seq, __ATOMIC_ACQUIRE);

    if (n > r->last_seq) {
      if (!still_alive(r, now, false))
        return FBUS_GONE;
      if (copy_frame(r, n, planes, info)) {
        if (info)
          info->skipped = r->last_seq ? n - r->last_seq - 1 : 0;
        r->last_seq = n;
        return FBUS_FRAME;
      }
      continue; // a newer frame landed meanwhile; go for that one
    }

    if (!still_alive(r, now, true))
      return FBUS_GONE;
    if (now >= deadline)
      return FBUS_TIMEOUT;

    // Sleep until the writer publishes (or closes), at most until the
    // next liveness check.
    uint64_t left = deadline - now;
    if (left > alive_check_period_ns)
      left = alive_check_period_ns;
    if (r->futex_works) {
      struct timespec ts = {.tv_sec = (time_t)(left / 1000000000ull),
                            .tv_nsec = (long)(left % 1000000000ull)};
      uint32_t expect = __atomic_load_n(&h->futex_word, __ATOMIC_ACQUIRE);
      // Re-check after reading the futex word: a publish between the
      // write_seq load above and here would otherwise be slept through.
      if (__atomic_load_n(&h->write_seq, __ATOMIC_ACQUIRE) != n)
        continue;
      if (futex((uint32_t *)&h->futex_word, FUTEX_WAIT, expect, &ts) != 0 &&
          errno == EFAULT)
        r->futex_works = false; // old kernels refuse read-only mappings
    } else {
      struct timespec ts = {0, 2000000L};
      nanosleep(&ts, NULL);
    }
  }
}

void fbus_reader_close(FbusReader *r) {
  if (!r)
    return;
  munmap(r->base, r->size);
  free(r);
}

// ---- Status -------------------------------------------------------------

FbusOpenResult fbus_status(const char *name, FbusStatus *out) {
  memset(out, 0, sizeof *out);
  snprintf(out->name, sizeof out->name, "%s", name);
  out->last_frame_no = -1;
  if (!fbus_valid_name(name))
    return FBUS_OPEN_MISSING;

  char path[PATH_MAX];
  stream_path(name, path, sizeof path);
  uint8_t *base;
  size_t size;
  struct stat st;
  FbusOpenResult res = map_stream(path, &base, &size, &st);
  if (res != FBUS_OPEN_OK)
    return res;

  const FbusHeader *h = (const FbusHeader *)base;
  uint64_t now = mono_ns();
  out->width = h->width;
  out->height = h->height;
  out->writer_pid = h->writer_pid;
  out->closed =
      (__atomic_load_n(&h->flags, __ATOMIC_ACQUIRE) & fbus_flag_closed) != 0;
  out->alive = !header_stale(h, now);
  uint64_t beat = __atomic_load_n(&h->heartbeat_mono_ns, __ATOMIC_ACQUIRE);
  out->heartbeat_age_ns = now > beat ? now - beat : 0;

  uint64_t n = __atomic_load_n(&h->write_seq, __ATOMIC_ACQUIRE);
  out->frames = n;
  if (n > 0) {
    const FbusSlotHeader *s = slot_at(base, h, (n - 1) % h->slot_count);
    uint64_t s1 = __atomic_load_n(&s->seq, __ATOMIC_ACQUIRE);
    int64_t fno = s->frame_no;
    __atomic_thread_fence(__ATOMIC_ACQUIRE);
    if (__atomic_load_n(&s->seq, __ATOMIC_RELAXED) == s1 && s1 == 2 * n)
      out->last_frame_no = fno;
    uint64_t pub = __atomic_load_n(&h->last_publish_mono_ns, __ATOMIC_RELAXED);
    out->last_frame_age_ns = now > pub ? now - pub : 0;
  }
  munmap(base, size);
  return FBUS_OPEN_OK;
}

int fbus_list(void (*fn)(const char *name, void *ctx), void *ctx) {
  DIR *d = opendir(fbus_dir());
  if (!d)
    return 0;
  int count = 0;
  struct dirent *e;
  while ((e = readdir(d)) != NULL) {
    if (strncmp(e->d_name, "eeye.", 5) != 0)
      continue;
    const char *name = e->d_name + 5;
    if (!fbus_valid_name(name))
      continue;
    fn(name, ctx);
    count++;
  }
  closedir(d);
  return count;
}
