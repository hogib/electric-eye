#pragma once
#include "hlog.h"
#include <stdbool.h>
#include <stdint.h>

/*
 * hellion's live status, as a shared-memory file <bus dir>/hellion.<name>
 * that `hellion top` and `hellion check` map read-only.
 *
 * Shared memory rather than a socket because the reader then needs no
 * parser and no cooperation: a wedged hellion still shows its last state,
 * and a stale heartbeat says how long ago that was. The writer is one
 * thread in hellion that copies a snapshot in every 250ms under a seqlock,
 * the same scheme as the frame bus (fbus.c).
 *
 * Internal to hellion: top and the daemon are the same binary, so the
 * struct is the format. `version` and `struct_size` catch a mismatch
 * between a running hellion and a newer `hellion top`.
 */

enum {
  hs_version = 4,
  hs_max_pipelines = 8,
  hs_max_sinks = 8,
  hs_spec_len = 64,
  hs_state_len = 16,
  hs_detail_len = 112,
  hs_name_len = 32,
  hs_peer_lines = 16,
};

// One end of a pipeline: its source or one of its sinks.
typedef struct {
  char spec[hs_spec_len];     // "v4l2:/dev/video0", "bus:camera" ...
  char state[hs_state_len];   // "running", "waiting", "error", ...
  char detail[hs_detail_len]; // the latest thing worth saying about it
  uint64_t frames;            // passed through so far
  uint64_t dropped;           // frames it could not keep up with
  double fps;                 // over the last couple of seconds

  // Network endpoints (RTP) only: what the link is doing.
  bool net;
  double kbps;       // over the last second
  double loss_pct;   // packets lost, over the last second
  uint64_t packets;  // total
  uint64_t lost;     // total, from RTP sequence gaps
  double idle_s;     // since the last packet; -1 if none yet
} HsEndpoint;

typedef struct {
  char name[hs_name_len];
  uint32_t width; // 0 until the source has opened
  uint32_t height;
  HsEndpoint source;
  uint32_t sink_count;
  HsEndpoint sinks[hs_max_sinks];
} HsPipeline;

typedef struct {
  char magic[8]; // "HELLION1"
  uint32_t version;
  uint32_t struct_size;
  uint64_t seq; // seqlock: odd while being written

  int32_t pid;
  uint32_t reserved;
  uint64_t heartbeat_mono_ns;
  uint64_t started_real_ns;
  char name[hs_name_len];
  char role[16];
  char eeye_config[256];

  uint32_t pipeline_count;
  HsPipeline pipelines[hs_max_pipelines];

  // Recording, one line ("" when idle and fine).
  char recording[hs_detail_len];

  // The hellion at the other end of the tether, as it last described
  // itself -- whichever side started the exchange (see control.c).
  bool peer_configured;            // this side has "peer" and calls it
  char peer_addr[64];              // who, as configured or as seen
  uint64_t peer_contact_mono_ns;   // 0 = never
  char peer_error[hs_detail_len];  // the last failed attempt, if any
  uint32_t peer_line_count;
  char peer_lines[hs_peer_lines][hs_detail_len];
  // Discovery (discovery.h): its state as a DiscState (0 = off), whether
  // this hellion relies on it (an "auto" peer or sink), and one line.
  uint32_t discovery_state;
  bool discovery_needed;
  char discovery[hs_detail_len];

  uint64_t log_total; // lines ever logged
  uint32_t log_count; // lines below, oldest first
  char log[hlog_lines][hlog_line_len];
} HsStatus;

typedef struct HsWriter HsWriter;

// Creates <bus dir>/hellion.<name>. NULL (with a message) on failure;
// hellion runs on without a status file then.
HsWriter *hs_writer_open(const char *name);

// Publishes `snap` (heartbeat and log are filled in here).
void hs_writer_publish(HsWriter *w, HsStatus *snap);

// Removes the file (if still ours) and frees.
void hs_writer_close(HsWriter *w);

typedef enum {
  HS_READ_OK = 0,
  HS_READ_MISSING,  // no such instance
  HS_READ_MISMATCH, // written by a different build of hellion
  HS_READ_BUSY,     // kept changing while being read; try again
  HS_READ_ERROR,
} HsReadResult;

// Reads instance `name`'s status into *out.
HsReadResult hs_read(const char *name, HsStatus *out);

// Seconds since the instance's last heartbeat; it publishes every 250ms.
double hs_heartbeat_age(const HsStatus *s);

// Lists running instances' names (from <bus dir>/hellion.*).
int hs_list(char (*names)[hs_name_len], int max);
