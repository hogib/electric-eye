#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/*
 * Runtime health, written periodically to "<config_path>.health" for a
 * remote operator to read (pi/config_agent.py relays it; see its /health
 * endpoint).
 *
 * This exists because the drone knows things topside cannot infer. Chief
 * among them: whether recording is actually still running. When a disk
 * fills mid-dive, consumer_loop stops recording and clears its path -- but
 * the operator's UI still shows the path they typed, so they carry on
 * believing they are recording and only discover the gap after surfacing.
 * At 640x480 that is ~18 MB/s, so a disk fills fast enough for this to be
 * a real way to lose a dive's footage.
 *
 * Same writer contract as the config status file (temp file + rename), so
 * a reader never observes a half-written one. Written on a timer rather
 * than on every frame -- this is diagnostics, not a hot path.
 */

typedef enum {
  RECORDING_OFF = 0,   // no record_path configured
  RECORDING_ACTIVE,    // writing frames right now
  RECORDING_FAILED,    // was asked to record but couldn't, or write failed
} RecordingState;

typedef struct {
  RecordingState recording_state;
  char recording_path[256];
  // Why recording stopped, when state is RECORDING_FAILED. Empty
  // otherwise. Carried as text because the operator-facing answer to "why
  // isn't it recording" is a sentence, not an errno.
  char recording_error[128];
  uint64_t recording_bytes;   // written this session
  uint64_t disk_free_bytes;   // on the recording path's filesystem, 0 if unknown

  bool camera_connected;
  uint64_t frames_captured;

  uint32_t frame_width;
  uint32_t frame_height;
} HealthSnapshot;

/*
 * Publishes `snap` to "<config_path>.health". Best-effort: a failure here
 * is logged once and never fatal, since health reporting must not be able
 * to take down the pipeline it reports on.
 */
void health_publish(const char *config_path, const HealthSnapshot *snap);

/*
 * Free bytes on the filesystem containing `path`, or 0 if that can't be
 * determined. Uses the path's parent directory when the file itself does
 * not exist yet, which is the normal case when checking before starting a
 * recording.
 */
uint64_t health_disk_free(const char *path);
