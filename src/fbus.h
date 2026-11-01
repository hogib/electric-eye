#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/*
 * The frame bus: how video moves between programs on one machine.
 *
 * eeye publishes the pilot's picture and the raw camera frame here;
 * hellion, `hellion top`, and future autonomy code read them. Each stream
 * is one shared-memory file, <dir>/eeye.<stream> (dir is $EEYE_BUS_DIR,
 * else /dev/shm), holding a small ring of frames. docs/bus.md documents
 * the layout byte by byte, so a reader in any language can mmap it.
 *
 * The rules that matter:
 *
 * - **The writer never waits for a reader.** It writes the next slot and
 *   moves on. A reader that falls behind skips to the newest frame and is
 *   told how many it missed; it can never stall the pilot's picture.
 * - **Readers never see a half-written frame.** Each slot has a sequence
 *   counter, odd while the slot is being written (a seqlock). A reader
 *   checks it before and after copying and retries if the slot changed
 *   underneath it.
 * - **Readers sleep until a frame arrives**, on a futex in the shared
 *   header, instead of polling.
 * - **Liveness is a heartbeat**, written twice a second by a thread of the
 *   writer's own, not a pid check: a reader in a container (a BlueOS
 *   extension, say) cannot see the writer's pid. A writer that restarts
 *   replaces the file atomically (rename), and readers notice the new file
 *   and report the stream as gone, so they reopen it.
 *
 * Frames are I422 planar, full range -- the layout VideoFrame uses -- with
 * the capture time and frame number of each frame alongside.
 */

enum { fbus_max_stream_name = 32 };

// Whether `name` can be a stream name: 1-31 of letters, digits, '-', '_'.
bool fbus_valid_name(const char *name);

// ---- Writer -------------------------------------------------------------

typedef struct FbusWriter FbusWriter;

/*
 * Creates stream `name` at width x height and starts its heartbeat. Any
 * existing stream of that name is replaced; its readers see it as gone.
 * `name` is letters, digits, '-' and '_' only. Returns NULL on failure,
 * having printed why.
 */
FbusWriter *fbus_writer_open(const char *name, uint32_t width,
                             uint32_t height);

/*
 * Publishes one frame. `planes` are Y, U, V with the I422 strides
 * (width, (width+1)/2, (width+1)/2) -- VideoFrame's planes or raw_planes.
 * frame_no should count up by one per frame; capture_mono_ns is the
 * frame's CLOCK_MONOTONIC capture time (0 if unknown). Never blocks.
 */
void fbus_writer_publish(FbusWriter *w, const uint8_t *const planes[3],
                         int64_t frame_no, uint64_t capture_mono_ns);

/*
 * Marks the stream closed, stops the heartbeat, and removes the file if it
 * is still ours (a newer writer may have replaced it). Readers see the
 * stream as gone at once. Safe on NULL.
 */
void fbus_writer_close(FbusWriter *w);

// ---- Reader -------------------------------------------------------------

typedef struct FbusReader FbusReader;

typedef enum {
  FBUS_OPEN_OK = 0,
  FBUS_OPEN_MISSING,    // no such stream (yet)
  FBUS_OPEN_DENIED,     // exists, but no permission to read it
  FBUS_OPEN_BAD_FORMAT, // not a frame bus file, or a version we can't read
  FBUS_OPEN_STALE,      // a file whose writer is no longer running
  FBUS_OPEN_ERROR,      // anything else
} FbusOpenResult;

const char *fbus_open_result_str(FbusOpenResult r);

/*
 * Opens stream `name` for reading. Prints nothing: an absent stream is
 * normal while its writer starts, so the caller decides what to say.
 * Returns NULL and sets *why (if non-NULL) on failure.
 */
FbusReader *fbus_reader_open(const char *name, FbusOpenResult *why);

void fbus_reader_geometry(const FbusReader *r, uint32_t *width,
                          uint32_t *height);

typedef struct {
  int64_t frame_no;
  uint64_t capture_mono_ns; // 0 if the writer did not know
  uint64_t capture_real_ns; // the same moment on the wall clock
  uint64_t publish_mono_ns; // when the writer published it
  uint64_t skipped;         // frames published since the last one read
                            // that this read did not return
} FbusFrameInfo;

typedef enum {
  FBUS_FRAME = 0, // a new frame was copied out
  FBUS_TIMEOUT,   // nothing new within the timeout; the writer is alive
  FBUS_GONE,      // closed, replaced, or its writer stopped: reopen
} FbusReadResult;

/*
 * Waits up to timeout_ms for a frame newer than the last one returned,
 * then copies the newest into `planes` (I422 strides, as for publish).
 * `info` may be NULL.
 */
FbusReadResult fbus_reader_next(FbusReader *r, uint8_t *const planes[3],
                                int timeout_ms, FbusFrameInfo *info);

void fbus_reader_close(FbusReader *r);

// ---- Status, for dashboards ---------------------------------------------

typedef struct {
  char name[fbus_max_stream_name];
  uint32_t width;
  uint32_t height;
  int32_t writer_pid;       // as the writer saw it; may mean nothing here
  bool alive;               // heartbeat fresh, not closed, not replaced
  bool closed;              // the writer closed it cleanly
  uint64_t frames;          // frames published so far
  int64_t last_frame_no;    // -1 if none yet
  uint64_t last_frame_age_ns; // since the newest publish; 0 if none
  uint64_t heartbeat_age_ns;
} FbusStatus;

/*
 * Reads stream `name`'s header without attaching as a reader. For
 * `hellion top`: fps is the change in `frames` between two calls.
 */
FbusOpenResult fbus_status(const char *name, FbusStatus *out);

/*
 * Lists the streams in the bus directory, calling fn once per name.
 * Returns how many it found.
 */
int fbus_list(void (*fn)(const char *name, void *ctx), void *ctx);

// The directory streams live in: $EEYE_BUS_DIR, else /dev/shm.
const char *fbus_dir(void);
