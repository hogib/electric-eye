#pragma once
#include "config.h"
#include "frame_ring_buffer.h"
#include "health.h"
#include "video_frame.h"
#include <stdbool.h>
#include <stdint.h>

/*
 * The recording tap, on its own thread.
 *
 * Recording used to be an fwrite() on consumer_loop's thread, between the
 * virtual-camera write and the next frame. Raw I422 is ~55MB/s at 720p --
 * more than most SD cards sustain -- and once the kernel's dirty-page limit
 * is hit, fwrite() blocks: the out ring backs up and the pilot's picture
 * lags behind the camera. fopen() and fclose() (which flushes) can block
 * the same way. Here the writer thread owns the file outright, and the
 * consumer only ever hands it frames it can afford to lose.
 *
 * Frame ownership: recorder_submit() takes a frame the consumer has
 * finished with. The writer returns it to `free_ring` after writing it (or
 * immediately, if nothing is recording). If the writer is already holding
 * recorder_max_queued frames, submit refuses, the consumer returns the
 * frame to the pool itself, and the recording counts a dropped frame --
 * visible in the health file -- instead of the live view stalling.
 */

typedef struct Recorder Recorder;

// Frames the writer may hold at once. Each is out of the pool while it
// waits, so this trades burst tolerance against frames available to the
// camera (see frame_pool_size in eeye.c).
constexpr unsigned recorder_max_queued = 3;

// What consumer_loop folds into its health snapshot.
typedef struct {
  RecordingState state;
  char path[256];
  char error[128];
  uint64_t bytes;          // written to the current file
  uint64_t dropped_frames; // not recorded because the writer was behind
} RecorderStatus;

// Starts the writer thread. Frames are width x height (the pipeline
// geometry); framerate is only used in the playback hint it logs.
Recorder *recorder_create(FrameRingBuffer *free_ring, uint32_t width,
                          uint32_t height, uint32_t framerate);

// Asks the writer to close whatever it is recording and, if `path` is
// non-empty, start recording there in `format`. Returns at once; the
// open/close happens on the writer thread, in order with the frames
// submitted before and after this call.
void recorder_set_target(Recorder *r, const char *path, RecordFormat format);

// Offers a finished frame for recording. Returns true if the recorder took
// it (it will reach free_ring later); false if the caller still owns it --
// either nothing is being recorded, or the writer is behind (counted as a
// dropped frame).
bool recorder_submit(Recorder *r, VideoFrame *frame);

void recorder_status(Recorder *r, RecorderStatus *out);

// Writes out everything already queued, closes the file, returns every
// frame it holds to free_ring, and stops the thread.
void recorder_destroy(Recorder *r);
