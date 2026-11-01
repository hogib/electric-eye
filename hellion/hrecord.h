#pragma once
#include "hconfig.h"
#include "pipeline.h"
#include <stdbool.h>
#include <stddef.h>

/*
 * Topside recording: the H.264 stream exactly as it arrives over the
 * tether, copied into an MP4 by an ffmpeg child, without re-encoding.
 *
 * Starting a recording attaches a local port to the receiving pipeline's
 * UDP relay (udp_relay_attach), so the recorder gets the same packets as
 * the decoder and QGC, and takes nothing from them. The file is
 * fragmented MP4 (see ff_recorder_args): one cut off by a crash or a
 * power loss still plays up to its last keyframe.
 *
 * One recording at a time: "am I recording?" has one answer.
 */

typedef struct HRec HRec;

// Picks the pipeline to record ("record_from", else the first with an rtp
// source) from `pipes`. Never NULL: a recorder with nothing to record
// reports itself unavailable, with the reason.
HRec *hrec_create(const HellionConfig *cfg, Pipeline *const *pipes,
                          size_t pipe_count);

// Starts or stops. *detail is the file name on success, else why not.
bool hrec_start(HRec *r, char *detail, size_t cap);
bool hrec_stop(HRec *r, char *detail, size_t cap);

// The page's "recorder" object: available, why, recording, file,
// elapsed_s, bytes, disk_free_bytes, error.
void hrec_status_json(HRec *r, char *out, size_t cap);

// One line for top and the status file; "" when idle and fine.
void hrec_summary(HRec *r, char *out, size_t cap);

// Stops any recording (finalizing the file) and frees.
void hrec_destroy(HRec *r);
