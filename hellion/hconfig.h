#pragma once
#include "config.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/*
 * hellion's own config: which pipelines to run. Separate from eeye's
 * effects config (that one belongs to eeye and is hot-reloaded; this one
 * describes the machine's role and is read once at startup).
 *
 *   {
 *     "name": "rov",
 *     "role": "rov",
 *     "eeye_config": "/var/lib/eeye/effects.json",
 *     "pipelines": [
 *       {"name": "uplink", "source": "v4l2:/dev/video0",
 *        "width": 1280, "height": 720, "format": "auto",
 *        "sinks": ["bus:camera", "v4l2:/dev/video11"]}
 *     ]
 *   }
 *
 * "name"        Instance name (default "hellion"): the status file is
 *               <bus dir>/hellion.<name>, and `hellion top -n` picks it.
 * "role"        "rov" or "topside" (default "rov"). Shown by top; later
 *               phases key behavior off it.
 * "eeye_config" eeye's config file on this machine, if eeye runs here
 *               (default ""): top shows eeye's health from beside it.
 *
 * A pipeline moves frames from one source to up to 8 sinks:
 *   sources  "v4l2:<device>"  a V4L2 camera (MJPEG or YUYV)
 *            "rpicam"         a Pi camera module, via rpicam-vid
 *            "bus:<stream>"   a frame bus stream (geometry from the stream)
 *            "rtp:<port>"     H.264 over RTP arriving on UDP <port>
 *   sinks    "bus:<stream>"   publish to the frame bus
 *            "v4l2:<device>"  a v4l2loopback device (optional, retried)
 *            "rtp:<host>:<port>"  encode to H.264, send over RTP
 *            "rtp:auto:<port>"    the same, to the topside discovery finds
 * Camera and RTP sources take "width"/"height" (default 1280x720; an RTP
 * source scales what it decodes to that) and camera sources "format"
 * ("auto", "mjpeg", "yuyv"; "yuv420" for rpicam). An RTP source takes
 * "relay": ["host:port", ...] to forward its packets unchanged (to QGC,
 * say). RTP sinks take "encoder" (default "libx264"), "bitrate_kbps"
 * (default 4000) and "gop" (default 30). "framerate" (default 30) is the
 * source's rate: asked of cameras, and what RTP sinks stamp frames at.
 *
 * Same strictness as eeye's config: an unknown key or a bad value is an
 * error with the reason printed, never ignored.
 */

enum {
  hcfg_max_pipelines = 8,
  hcfg_max_sinks = 8,
  hcfg_max_spec = 128,
  hcfg_max_name = 32,
};

typedef enum {
  SRC_V4L2 = 0,
  SRC_RPICAM,
  SRC_BUS,
  SRC_RTP, // H.264 over RTP arriving on a UDP port
} SourceKind;

typedef enum {
  SINK_BUS = 0,
  SINK_V4L2,
  SINK_RTP, // encode to H.264 and send over RTP
} SinkKind;

enum { hcfg_max_relays = 4 };

typedef struct {
  SinkKind kind;
  char spec[hcfg_max_spec]; // as written, for display
  char arg[hcfg_max_spec];  // the part after "kind:"
} SinkConfig;

typedef struct {
  char name[hcfg_max_name];
  SourceKind source;
  char source_spec[hcfg_max_spec];
  char source_arg[hcfg_max_spec];
  uint16_t width; // camera sources only
  uint16_t height;
  CaptureFormat format;
  size_t sink_count;
  SinkConfig sinks[hcfg_max_sinks];

  // RTP source: where to forward the received packets unchanged as well
  // (QGC, a recorder), "host:port".
  size_t relay_count;
  char relays[hcfg_max_relays][hcfg_max_spec];
  // RTP sinks: how to encode.
  char encoder[32];       // default "libx264"
  uint32_t bitrate_kbps;  // default 4000
  uint32_t gop;           // default 30 (one keyframe a second at 30fps)
  // The source's frame rate: asked of cameras, and what RTP sinks stamp
  // frames at (so what a recording's timing assumes). Default 30.
  uint32_t framerate;
} PipelineConfig;

typedef struct {
  char name[hcfg_max_name];
  char role[16];
  char eeye_config[256];
  // HTTP: where to serve the page and API ("" = not at all), the hellion
  // at the other end of the tether, the stream the page previews by
  // default, and where snapshots go.
  char control[hcfg_max_spec];
  char peer[hcfg_max_spec];
  char preview[hcfg_max_name];
  char snapshot_dir[256];
  // Recording (topside): the RTP source pipeline whose stream is copied to
  // MP4 ("" = the first pipeline with an rtp source), and where.
  char record_from[hcfg_max_name];
  char record_dir[256];
  // Camera controls. "peer" (topside, placement A): the camera is on the
  // ROV, under that hellion, so the "camera" object of every config the
  // page writes is also sent to the peer's /camera. "" (default): they are
  // eeye's, in its config, nothing more to do.
  char camera_controls[8];
  // Where a hellion with a camera source keeps the last controls it was
  // sent, so a restart puts them back (default
  // /var/lib/hellion/camera.json).
  char camera_file[256];
  // Plug and play (discovery.h). "discovery" (default true) announces this
  // hellion on the local network and listens for the others; "peer":
  // "auto" and RTP sinks to "rtp:auto:<port>" use what it finds. "pair"
  // (default "default") must match the other end's; "placement" ("a",
  // "b", or "" unknown) is announced, and a peer of the other placement is
  // refused.
  bool discovery;
  char pair[hcfg_max_name];
  char placement[2];
  size_t pipeline_count;
  PipelineConfig pipelines[hcfg_max_pipelines];
} HellionConfig;

// Whether a sink sends to the discovered topside ("rtp:auto:<port>"), and
// "peer": "auto".
bool hcfg_sink_is_auto(const SinkConfig *s);
bool hcfg_peer_is_auto(const HellionConfig *c);

// Parses `buf`. On failure prints why and returns false.
bool hconfig_parse(const char *buf, size_t len, HellionConfig *out);

// Reads and parses `path`. A missing file is an error: hellion with no
// pipelines has nothing to do.
bool hconfig_load(const char *path, HellionConfig *out);
