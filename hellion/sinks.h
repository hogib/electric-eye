#pragma once
#include "hconfig.h"
#include "hstatus.h"
#include "udp_relay.h"
#include <stddef.h>
#include <stdint.h>

/*
 * The things a pipeline can send frames to. Each runs on its own sink
 * thread (pipeline.c); these functions are only ever called from it.
 */

// One I422 frame, planes contiguous (Y, then U, then V).
typedef struct {
  const uint8_t *planes[3];
  size_t plane_bytes[3];
  uint32_t width;
  uint32_t height;
  int64_t frame_no;
  uint64_t capture_ns;
} SinkFrame;

// What a sink says about itself for the status file.
typedef struct {
  char state[hs_state_len];
  char detail[hs_detail_len];
} SinkReport;

typedef struct SinkImpl SinkImpl;

/*
 * Opens a sink at width x height. Returns NULL only for an outright
 * failure (reported in *rep); a sink whose destination is merely missing
 * for now (a v4l2 device not there yet) opens anyway and keeps retrying.
 */
SinkImpl *sink_impl_open(const SinkConfig *cfg, const PipelineConfig *pc,
                         uint32_t width, uint32_t height, SinkReport *rep);

// Sends one frame. Returns false if it was not delivered; *rep says why.
bool sink_impl_write(SinkImpl *s, const SinkFrame *f, SinkReport *rep);

/*
 * For an "rtp:" sink of a pipeline whose source is H.264 already
 * (hcfg_passthrough): packetizes one access unit (Annex B) and sends it.
 * `capture_ns` (CLOCK_MONOTONIC) becomes the RTP timestamp. Returns false
 * if it was not sent; *rep says why. Called from the pipeline's source
 * thread, in place of sink_impl_write.
 */
bool sink_impl_write_h264(SinkImpl *s, const uint8_t *au, size_t len,
                          uint64_t capture_ns, SinkReport *rep);

// Whether a receiver has joined since the last call, and so needs a
// keyframe to start from. Only ever true for a sink written with
// sink_impl_write_h264.
bool sink_impl_wants_keyframe(SinkImpl *s);

// Network sinks: what the link is doing. False for the others. Safe to
// call from another thread while the sink thread writes.
bool sink_impl_net(SinkImpl *s, RelayStats *out);

void sink_impl_close(SinkImpl *s);
