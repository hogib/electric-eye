// _POSIX_C_SOURCE before any header include: -std=c23 puts glibc in strict
// ISO mode otherwise. Same guard as video_threads.c/config.c.
#define _POSIX_C_SOURCE 200809L

#include "recorder.h"
#include <assert.h>
#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// One queued request: a frame to write, or (frame == NULL) a change of
// recording target. Both go through one FIFO so a frame submitted before a
// stop is written to the file it was meant for, never the next one.
typedef struct {
  VideoFrame *frame;
  char path[max_record_path_len];
  RecordFormat format;
} Entry;

// Enough for recorder_max_queued frames with a target change between
// every pair: consecutive target changes collapse into one entry (only
// the latest matters), so frames and targets can at most alternate.
constexpr unsigned recorder_queue_cap = 2 * recorder_max_queued + 2;

struct Recorder {
  pthread_t thread;
  pthread_mutex_t lock;
  pthread_cond_t wake;

  // --- Shared, under `lock` ---
  Entry queue[recorder_queue_cap];
  unsigned head;
  unsigned count;
  unsigned frames_held;    // queued + the one being written
  unsigned targets_queued; // target entries not yet processed
  // Whether submit should take frames: the latest target is non-empty and
  // has not failed. Frames offered while this is false are simply not
  // recorded, which is not a drop.
  bool accepting;
  bool quit;
  RecorderStatus status;

  // --- Fixed after create ---
  FrameRingBuffer *free_ring;
  uint32_t width;
  uint32_t height;
  uint32_t framerate;

  // --- Writer thread only ---
  FILE *file;
  RecordFormat requested;
  bool format_decided; // set by the first frame written to `file`
  bool writing_mjpeg;
};

static void return_frame(Recorder *r, VideoFrame *frame) {
  // NULL: this frame must go back to the pool regardless of is_running --
  // abandoning it would leak it out of the pool for the rest of the
  // process's life.
  ring_push_wait(r->free_ring, frame, NULL);
}

// Marks the current recording failed. `err` is the errno that explains
// it, captured before any printf could clobber it. Called by the writer
// with `lock` not held.
static void fail(Recorder *r, const char *what, int err) {
  pthread_mutex_lock(&r->lock);
  r->status.state = RECORDING_FAILED;
  snprintf(r->status.error, sizeof r->status.error, "%s: %s", what,
           strerror(err));
  // Stop taking frames for this target -- unless a newer target is
  // already queued, in which case that one decides.
  if (r->targets_queued == 0)
    r->accepting = false;
  pthread_mutex_unlock(&r->lock);
}

static void close_file(Recorder *r, const char *old_path) {
  if (!r->file)
    return;
  // fclose() flushes, and on a slow card that is where the waiting
  // happens -- the reason this runs here and not on consumer_loop.
  if (fclose(r->file) != 0)
    printf("Recording: closing %s failed (%s); its tail may be missing.\n",
           old_path, strerror(errno));
  else
    printf("Recording stopped: %s\n", old_path);
  r->file = NULL;
}

static void switch_target(Recorder *r, const char *path, RecordFormat format) {
  char old_path[sizeof r->status.path];
  pthread_mutex_lock(&r->lock);
  snprintf(old_path, sizeof old_path, "%s", r->status.path);
  pthread_mutex_unlock(&r->lock);
  close_file(r, old_path);

  pthread_mutex_lock(&r->lock);
  snprintf(r->status.path, sizeof r->status.path, "%s", path);
  r->status.error[0] = '\0';
  r->status.bytes = 0;
  r->status.dropped_frames = 0;
  r->status.state = RECORDING_OFF;
  pthread_mutex_unlock(&r->lock);
  if (path[0] == '\0')
    return;

  // Check for room before opening, not just after failing to write. Raw
  // I422 at 30fps is ~55MB/s at 720p, so "there was space when I hit
  // record" is worth knowing before the dive rather than after -- this is
  // reported, not enforced, since how much footage an operator intends to
  // take is theirs to decide.
  uint64_t free_bytes = health_disk_free(path);
  size_t raw_frame_bytes = (size_t)r->width * r->height * 2;
  if (free_bytes > 0 && raw_frame_bytes > 0) {
    printf("Recording: %llu MB free at %s",
           (unsigned long long)(free_bytes / (1024 * 1024)), path);
    if (format == RECORD_RAW)
      printf(" (~%llu seconds of raw video at this geometry)",
             (unsigned long long)(free_bytes /
                                  (raw_frame_bytes * r->framerate)));
    printf("\n");
  }

  r->file = fopen(path, "wb");
  if (!r->file) {
    int err = errno;
    printf("Recording failed to start: could not open %s: %s\n", path,
           strerror(err));
    fail(r, "could not open", err);
    return;
  }
  // Larger than stdio's default, so MJPEG's ~100KB frames go out in a
  // handful of write()s rather than dozens.
  setvbuf(r->file, NULL, _IOFBF, 1u << 20);
  r->requested = format;
  r->format_decided = false;

  pthread_mutex_lock(&r->lock);
  r->status.state = RECORDING_ACTIVE;
  pthread_mutex_unlock(&r->lock);
  printf("Recording started: %s\n", path);
}

// The first frame decides what this file actually holds: "mjpeg" needs the
// camera's own JPEG bytes, which a YUYV or yuv420 source never had.
static void decide_format(Recorder *r, const VideoFrame *frame,
                          const char *path) {
  r->format_decided = true;
  r->writing_mjpeg =
      r->requested == RECORD_MJPEG && frame->source_jpeg_len > 0;
  if (r->requested == RECORD_MJPEG && !r->writing_mjpeg)
    printf("Recording: \"record_format\": \"mjpeg\" needs an MJPEG camera "
           "stream, and this one is not; recording raw I422 instead.\n");
  if (r->writing_mjpeg)
    printf("Recording %s as MJPEG (the camera's own frames) -- play back "
           "with: ffplay -f mjpeg -framerate %u -i %s\n",
           path, r->framerate, path);
  else
    printf("Recording %s as raw I422 %ux%u -- play back with: ffplay -f "
           "rawvideo -pix_fmt yuv422p -s %ux%u -r %u -i %s\n",
           path, r->width, r->height, r->width, r->height, r->framerate,
           path);
}

static void write_frame(Recorder *r, const VideoFrame *frame) {
  if (!r->file)
    return; // stopped or failed since this frame was queued

  if (!r->format_decided) {
    char path[sizeof r->status.path];
    pthread_mutex_lock(&r->lock);
    snprintf(path, sizeof path, "%s", r->status.path);
    pthread_mutex_unlock(&r->lock);
    decide_format(r, frame, path);
  }

  const uint8_t *data;
  size_t len;
  if (r->writing_mjpeg) {
    if (frame->source_jpeg_len == 0) {
      // A frame that didn't come with its JPEG (shouldn't happen with a
      // fixed source); mixing a raw frame into an MJPEG file would make
      // it unplayable, so skip it and say so in the drop count.
      pthread_mutex_lock(&r->lock);
      r->status.dropped_frames++;
      pthread_mutex_unlock(&r->lock);
      return;
    }
    data = frame->source_jpeg;
    len = frame->source_jpeg_len;
  } else {
    data = frame->raw_data;
    len = frame->plane_sizes[0] + frame->plane_sizes[1] + frame->plane_sizes[2];
  }

  if (fwrite(data, 1, len, r->file) != len) {
    // Almost always a full disk. This is the case that most needs to
    // reach topside: the operator asked to record, believes they are
    // recording, and nothing on their screen would otherwise change.
    int err = errno;
    printf("Recording write failed (%s); stopping recording.\n",
           strerror(err));
    fail(r, "write failed", err);
    fclose(r->file);
    r->file = NULL;
    return;
  }
  pthread_mutex_lock(&r->lock);
  r->status.bytes += len;
  pthread_mutex_unlock(&r->lock);
}

static void *writer_loop(void *arg) {
  Recorder *r = arg;
  pthread_mutex_lock(&r->lock);
  for (;;) {
    while (r->count == 0 && !r->quit)
      pthread_cond_wait(&r->wake, &r->lock);
    if (r->count == 0)
      break; // quit, and everything queued has been handled

    Entry e = r->queue[r->head];
    r->head = (r->head + 1) % recorder_queue_cap;
    r->count--;
    if (!e.frame)
      r->targets_queued--;
    pthread_mutex_unlock(&r->lock);

    if (e.frame) {
      write_frame(r, e.frame);
      return_frame(r, e.frame);
      pthread_mutex_lock(&r->lock);
      r->frames_held--;
    } else {
      switch_target(r, e.path, e.format);
      pthread_mutex_lock(&r->lock);
    }
  }
  pthread_mutex_unlock(&r->lock);

  char path[sizeof r->status.path];
  snprintf(path, sizeof path, "%s", r->status.path);
  close_file(r, path);
  return NULL;
}

Recorder *recorder_create(FrameRingBuffer *free_ring, uint32_t width,
                          uint32_t height, uint32_t framerate) {
  Recorder *r = calloc(1, sizeof *r);
  if (!r)
    return NULL;
  r->free_ring = free_ring;
  r->width = width;
  r->height = height;
  r->framerate = framerate;
  r->status.state = RECORDING_OFF;
  pthread_mutex_init(&r->lock, NULL);
  pthread_cond_init(&r->wake, NULL);
  if (pthread_create(&r->thread, NULL, writer_loop, r) != 0) {
    pthread_cond_destroy(&r->wake);
    pthread_mutex_destroy(&r->lock);
    free(r);
    return NULL;
  }
  return r;
}

void recorder_set_target(Recorder *r, const char *path, RecordFormat format) {
  pthread_mutex_lock(&r->lock);
  Entry *e;
  unsigned last = (r->head + r->count + recorder_queue_cap - 1) %
                  recorder_queue_cap;
  if (r->count > 0 && r->queue[last].frame == NULL) {
    e = &r->queue[last]; // an unprocessed target change: replace it
  } else {
    // Cannot overflow: see recorder_queue_cap.
    assert(r->count < recorder_queue_cap);
    e = &r->queue[(r->head + r->count) % recorder_queue_cap];
    r->count++;
    r->targets_queued++;
  }
  e->frame = NULL;
  snprintf(e->path, sizeof e->path, "%s", path);
  e->format = format;
  r->accepting = path[0] != '\0';
  pthread_cond_signal(&r->wake);
  pthread_mutex_unlock(&r->lock);
}

bool recorder_submit(Recorder *r, VideoFrame *frame) {
  pthread_mutex_lock(&r->lock);
  if (!r->accepting) {
    pthread_mutex_unlock(&r->lock);
    return false;
  }
  if (r->frames_held >= recorder_max_queued) {
    r->status.dropped_frames++;
    pthread_mutex_unlock(&r->lock);
    return false;
  }
  Entry *e = &r->queue[(r->head + r->count) % recorder_queue_cap];
  e->frame = frame;
  r->count++;
  r->frames_held++;
  pthread_cond_signal(&r->wake);
  pthread_mutex_unlock(&r->lock);
  return true;
}

void recorder_status(Recorder *r, RecorderStatus *out) {
  pthread_mutex_lock(&r->lock);
  *out = r->status;
  pthread_mutex_unlock(&r->lock);
}

void recorder_destroy(Recorder *r) {
  if (!r)
    return;
  pthread_mutex_lock(&r->lock);
  r->quit = true;
  pthread_cond_signal(&r->wake);
  pthread_mutex_unlock(&r->lock);
  pthread_join(r->thread, NULL);
  pthread_cond_destroy(&r->wake);
  pthread_mutex_destroy(&r->lock);
  free(r);
}
