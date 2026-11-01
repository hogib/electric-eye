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
 *   sinks    "bus:<stream>"   publish to the frame bus
 *            "v4l2:<device>"  a v4l2loopback device (optional, retried)
 * Camera sources take "width"/"height" (default 1280x720) and "format"
 * ("auto", "mjpeg", "yuyv"; "yuv420" for rpicam).
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
} SourceKind;

typedef enum {
  SINK_BUS = 0,
  SINK_V4L2,
} SinkKind;

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
} PipelineConfig;

typedef struct {
  char name[hcfg_max_name];
  char role[16];
  char eeye_config[256];
  size_t pipeline_count;
  PipelineConfig pipelines[hcfg_max_pipelines];
} HellionConfig;

// Parses `buf`. On failure prints why and returns false.
bool hconfig_parse(const char *buf, size_t len, HellionConfig *out);

// Reads and parses `path`. A missing file is an error: hellion with no
// pipelines has nothing to do.
bool hconfig_load(const char *path, HellionConfig *out);
