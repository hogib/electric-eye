#define _POSIX_C_SOURCE 200809L

#include "hconfig.h"
#include "fbus.h"
#include "json_cursor.h"
#include <errno.h>
#include <stdio.h>
#include <string.h>

// Splits "kind:arg" at the first ':'. arg may be empty ("rpicam").
static void split_spec(const char *spec, char *kind, size_t kind_cap,
                       char *arg, size_t arg_cap) {
  const char *colon = strchr(spec, ':');
  if (!colon) {
    snprintf(kind, kind_cap, "%s", spec);
    arg[0] = '\0';
    return;
  }
  snprintf(kind, kind_cap, "%.*s", (int)(colon - spec), spec);
  snprintf(arg, arg_cap, "%s", colon + 1);
}

// 1-65535, digits only.
static bool valid_port(const char *s) {
  if (!*s || strlen(s) > 5)
    return false;
  long v = 0;
  for (const char *c = s; *c; c++) {
    if (*c < '0' || *c > '9')
      return false;
    v = v * 10 + (*c - '0');
  }
  return v >= 1 && v <= 65535;
}

// "host:port" or "[v6addr]:port", with a valid port.
static bool valid_host_port(const char *s) {
  const char *colon = strrchr(s, ':');
  if (!colon || colon == s || !valid_port(colon + 1))
    return false;
  if (s[0] == '[')
    return colon[-1] == ']' && colon - s > 2;
  return true;
}

static bool parse_source(PipelineConfig *p) {
  char kind[16];
  split_spec(p->source_spec, kind, sizeof kind, p->source_arg,
             sizeof p->source_arg);
  if (strcmp(kind, "v4l2") == 0 && p->source_arg[0]) {
    p->source = SRC_V4L2;
  } else if (strcmp(kind, "rpicam") == 0 && !p->source_arg[0]) {
    p->source = SRC_RPICAM;
  } else if (strcmp(kind, "bus") == 0 && fbus_valid_name(p->source_arg)) {
    p->source = SRC_BUS;
  } else if (strcmp(kind, "rtp") == 0 && valid_port(p->source_arg)) {
    p->source = SRC_RTP;
  } else {
    printf("hellion config: pipeline \"%s\": bad source \"%s\" (expected "
           "\"v4l2:<device>\", \"rpicam\", \"bus:<stream>\", or "
           "\"rtp:<port>\")\n",
           p->name, p->source_spec);
    return false;
  }
  return true;
}

static bool parse_sink(const PipelineConfig *p, SinkConfig *s) {
  char kind[16];
  split_spec(s->spec, kind, sizeof kind, s->arg, sizeof s->arg);
  if (strcmp(kind, "bus") == 0 && fbus_valid_name(s->arg)) {
    s->kind = SINK_BUS;
  } else if (strcmp(kind, "v4l2") == 0 && s->arg[0]) {
    s->kind = SINK_V4L2;
  } else if (strcmp(kind, "rtp") == 0 && valid_host_port(s->arg)) {
    s->kind = SINK_RTP;
  } else {
    printf("hellion config: pipeline \"%s\": bad sink \"%s\" (expected "
           "\"bus:<stream>\", \"v4l2:<device>\", or \"rtp:<host>:<port>\")\n",
           p->name, s->spec);
    return false;
  }
  return true;
}

// Moves past one ',' or the closing `close`. Returns 1 for ',', 0 for the
// close, -1 for anything else (already reported).
static int next_or_close(Cursor *c, char close, const char *where) {
  skip_ws(c);
  char ch;
  if (!cur_peek(c, &ch)) {
    printf("hellion config: unterminated %s\n", where);
    return -1;
  }
  c->pos++;
  if (ch == ',')
    return 1;
  if (ch == close)
    return 0;
  printf("hellion config: expected ',' or '%c' in %s\n", close, where);
  return -1;
}

static bool parse_sinks(Cursor *c, PipelineConfig *p) {
  if (!cur_expect(c, '[')) {
    printf("hellion config: pipeline \"%s\": \"sinks\" must be a list\n",
           p->name);
    return false;
  }
  skip_ws(c);
  char ch;
  if (cur_peek(c, &ch) && ch == ']') {
    c->pos++;
    return true;
  }
  for (;;) {
    if (p->sink_count == hcfg_max_sinks) {
      printf("hellion config: pipeline \"%s\": more than %d sinks\n",
             p->name, hcfg_max_sinks);
      return false;
    }
    SinkConfig *s = &p->sinks[p->sink_count];
    skip_ws(c);
    if (!parse_string(c, s->spec, sizeof s->spec)) {
      printf("hellion config: pipeline \"%s\": each sink must be a string\n",
             p->name);
      return false;
    }
    if (!parse_sink(p, s))
      return false;
    p->sink_count++;
    int r = next_or_close(c, ']', "\"sinks\"");
    if (r < 0)
      return false;
    if (r == 0)
      return true;
  }
}

// It ends up on ffmpeg's command line (as one argument, never through a
// shell), but there is still no reason to accept anything but a name.
static bool valid_encoder(const char *s) {
  if (!*s)
    return false;
  for (const char *c = s; *c; c++)
    if (!((*c >= 'a' && *c <= 'z') || (*c >= '0' && *c <= '9') || *c == '_'))
      return false;
  return true;
}

static bool parse_relays(Cursor *c, PipelineConfig *p) {
  if (!cur_expect(c, '[')) {
    printf("hellion config: pipeline \"%s\": \"relay\" must be a list\n",
           p->name);
    return false;
  }
  skip_ws(c);
  char ch;
  if (cur_peek(c, &ch) && ch == ']') {
    c->pos++;
    return true;
  }
  for (;;) {
    if (p->relay_count == hcfg_max_relays) {
      printf("hellion config: pipeline \"%s\": more than %d relays\n",
             p->name, hcfg_max_relays);
      return false;
    }
    char *dst = p->relays[p->relay_count];
    skip_ws(c);
    if (!parse_string(c, dst, hcfg_max_spec) || !valid_host_port(dst)) {
      printf("hellion config: pipeline \"%s\": each relay must be "
             "\"host:port\"\n",
             p->name);
      return false;
    }
    p->relay_count++;
    int r = next_or_close(c, ']', "\"relay\"");
    if (r < 0)
      return false;
    if (r == 0)
      return true;
  }
}

static bool parse_format(const char *s, CaptureFormat *out) {
  if (strcmp(s, "auto") == 0)
    *out = FORMAT_AUTO;
  else if (strcmp(s, "mjpeg") == 0)
    *out = FORMAT_MJPEG;
  else if (strcmp(s, "yuyv") == 0)
    *out = FORMAT_YUYV;
  else if (strcmp(s, "yuv420") == 0)
    *out = FORMAT_YUV420;
  else
    return false;
  return true;
}

static bool parse_pipeline(Cursor *c, PipelineConfig *p) {
  *p = (PipelineConfig){.width = 1280,
                        .height = 720,
                        .format = FORMAT_AUTO,
                        .encoder = "libx264",
                        .bitrate_kbps = 4000,
                        .gop = 30,
                        .framerate = 30};
  bool have_source = false;

  if (!cur_expect(c, '{')) {
    printf("hellion config: each pipeline must be an object\n");
    return false;
  }
  skip_ws(c);
  char ch;
  if (cur_peek(c, &ch) && ch == '}') {
    printf("hellion config: empty pipeline\n");
    return false;
  }
  for (;;) {
    char key[32];
    skip_ws(c);
    if (!parse_string(c, key, sizeof key)) {
      printf("hellion config: expected a quoted key in a pipeline\n");
      return false;
    }
    skip_ws(c);
    if (!cur_expect(c, ':')) {
      printf("hellion config: expected ':' after \"%s\"\n", key);
      return false;
    }
    skip_ws(c);

    if (strcmp(key, "name") == 0) {
      if (!parse_string(c, p->name, sizeof p->name) ||
          !fbus_valid_name(p->name)) {
        printf("hellion config: bad pipeline \"name\" (letters, digits, "
               "'-' and '_')\n");
        return false;
      }
    } else if (strcmp(key, "source") == 0) {
      if (!parse_string(c, p->source_spec, sizeof p->source_spec)) {
        printf("hellion config: \"source\" must be a string\n");
        return false;
      }
      have_source = true;
    } else if (strcmp(key, "sinks") == 0) {
      if (!parse_sinks(c, p))
        return false;
    } else if (strcmp(key, "width") == 0) {
      if (!parse_u16(c, &p->width)) {
        printf("hellion config: bad \"width\"\n");
        return false;
      }
    } else if (strcmp(key, "height") == 0) {
      if (!parse_u16(c, &p->height)) {
        printf("hellion config: bad \"height\"\n");
        return false;
      }
    } else if (strcmp(key, "relay") == 0) {
      if (!parse_relays(c, p))
        return false;
    } else if (strcmp(key, "encoder") == 0) {
      if (!parse_string(c, p->encoder, sizeof p->encoder) ||
          !valid_encoder(p->encoder)) {
        printf("hellion config: bad \"encoder\" (an ffmpeg encoder name, "
               "e.g. \"libx264\" or \"h264_v4l2m2m\")\n");
        return false;
      }
    } else if (strcmp(key, "bitrate_kbps") == 0) {
      int32_t v;
      if (!parse_i32(c, &v) || v < 100 || v > 100000) {
        printf("hellion config: \"bitrate_kbps\" must be 100-100000\n");
        return false;
      }
      p->bitrate_kbps = (uint32_t)v;
    } else if (strcmp(key, "framerate") == 0) {
      uint16_t v;
      if (!parse_u16(c, &v) || v < 1 || v > 240) {
        printf("hellion config: \"framerate\" must be 1-240\n");
        return false;
      }
      p->framerate = v;
    } else if (strcmp(key, "gop") == 0) {
      uint16_t v;
      if (!parse_u16(c, &v) || v < 1 || v > 600) {
        printf("hellion config: \"gop\" must be 1-600 frames\n");
        return false;
      }
      p->gop = v;
    } else if (strcmp(key, "format") == 0) {
      char fmt[16];
      if (!parse_string(c, fmt, sizeof fmt) || !parse_format(fmt, &p->format)) {
        printf("hellion config: bad \"format\" (expected \"auto\", "
               "\"mjpeg\", \"yuyv\" or \"yuv420\")\n");
        return false;
      }
    } else {
      printf("hellion config: unknown pipeline key \"%s\"\n", key);
      return false;
    }

    int r = next_or_close(c, '}', "a pipeline");
    if (r < 0)
      return false;
    if (r == 0)
      break;
  }

  if (!p->name[0]) {
    printf("hellion config: a pipeline has no \"name\"\n");
    return false;
  }
  if (!have_source) {
    printf("hellion config: pipeline \"%s\" has no \"source\"\n", p->name);
    return false;
  }
  if (!parse_source(p))
    return false;
  if (p->sink_count == 0) {
    printf("hellion config: pipeline \"%s\" has no sinks\n", p->name);
    return false;
  }
  if (p->width == 0 || p->height == 0 || p->width % 2 || p->height % 2) {
    printf("hellion config: pipeline \"%s\": %ux%u -- width and height must "
           "be even and non-zero\n",
           p->name, p->width, p->height);
    return false;
  }
  if (p->relay_count && p->source != SRC_RTP) {
    printf("hellion config: pipeline \"%s\": \"relay\" only applies to an "
           "\"rtp:<port>\" source\n",
           p->name);
    return false;
  }
  if (p->format == FORMAT_YUV420 && p->source != SRC_RPICAM) {
    printf("hellion config: pipeline \"%s\": \"yuv420\" is only for "
           "\"rpicam\"\n",
           p->name);
    return false;
  }
  // A pipeline reading the stream it publishes would feed itself.
  for (size_t i = 0; i < p->sink_count; i++) {
    if (p->source == SRC_BUS && p->sinks[i].kind == SINK_BUS &&
        strcmp(p->sinks[i].arg, p->source_arg) == 0) {
      printf("hellion config: pipeline \"%s\" reads and publishes bus "
             "stream \"%s\"\n",
             p->name, p->source_arg);
      return false;
    }
  }
  return true;
}

static bool parse_pipelines(Cursor *c, HellionConfig *cfg) {
  if (!cur_expect(c, '[')) {
    printf("hellion config: \"pipelines\" must be a list\n");
    return false;
  }
  skip_ws(c);
  char ch;
  if (cur_peek(c, &ch) && ch == ']') {
    c->pos++;
    return true;
  }
  for (;;) {
    if (cfg->pipeline_count == hcfg_max_pipelines) {
      printf("hellion config: more than %d pipelines\n", hcfg_max_pipelines);
      return false;
    }
    skip_ws(c);
    if (!parse_pipeline(c, &cfg->pipelines[cfg->pipeline_count]))
      return false;
    cfg->pipeline_count++;
    int r = next_or_close(c, ']', "\"pipelines\"");
    if (r < 0)
      return false;
    if (r == 0)
      return true;
  }
}

bool hconfig_parse(const char *buf, size_t len, HellionConfig *out) {
  HellionConfig cfg;
  memset(&cfg, 0, sizeof cfg);
  snprintf(cfg.name, sizeof cfg.name, "hellion");
  snprintf(cfg.role, sizeof cfg.role, "rov");
  snprintf(cfg.preview, sizeof cfg.preview, "pilot");
  snprintf(cfg.snapshot_dir, sizeof cfg.snapshot_dir,
           "/var/lib/hellion/snapshots");
  snprintf(cfg.record_dir, sizeof cfg.record_dir,
           "/var/lib/hellion/recordings");
  snprintf(cfg.camera_file, sizeof cfg.camera_file,
           "/var/lib/hellion/camera.json");
  cfg.discovery = true;
  snprintf(cfg.pair, sizeof cfg.pair, "default");

  Cursor c = {.s = buf, .len = len, .pos = 0};
  skip_ws(&c);
  if (!cur_expect(&c, '{')) {
    printf("hellion config: expected '{' at start\n");
    return false;
  }
  skip_ws(&c);
  char ch;
  if (!(cur_peek(&c, &ch) && ch == '}')) {
    for (;;) {
      char key[32];
      skip_ws(&c);
      if (!parse_string(&c, key, sizeof key)) {
        printf("hellion config: expected a quoted key\n");
        return false;
      }
      skip_ws(&c);
      if (!cur_expect(&c, ':')) {
        printf("hellion config: expected ':' after \"%s\"\n", key);
        return false;
      }
      skip_ws(&c);

      if (strcmp(key, "name") == 0) {
        if (!parse_string(&c, cfg.name, sizeof cfg.name) ||
            !fbus_valid_name(cfg.name)) {
          printf("hellion config: bad \"name\" (letters, digits, '-' and "
                 "'_')\n");
          return false;
        }
      } else if (strcmp(key, "role") == 0) {
        if (!parse_string(&c, cfg.role, sizeof cfg.role) ||
            (strcmp(cfg.role, "rov") != 0 &&
             strcmp(cfg.role, "topside") != 0)) {
          printf("hellion config: \"role\" must be \"rov\" or \"topside\"\n");
          return false;
        }
      } else if (strcmp(key, "control") == 0) {
        if (!parse_string(&c, cfg.control, sizeof cfg.control) ||
            (cfg.control[0] && !valid_port(cfg.control) &&
             !valid_host_port(cfg.control))) {
          printf("hellion config: \"control\" must be a port (\"8080\"), "
                 "\"addr:port\", or \"\" for no web page\n");
          return false;
        }
      } else if (strcmp(key, "peer") == 0) {
        if (!parse_string(&c, cfg.peer, sizeof cfg.peer) ||
            (cfg.peer[0] && strcmp(cfg.peer, "auto") != 0 &&
             !valid_host_port(cfg.peer))) {
          printf("hellion config: \"peer\" must be \"host:port\" (the "
                 "other hellion's \"control\"), \"auto\" (found on the "
                 "network), or \"\"\n");
          return false;
        }
      } else if (strcmp(key, "discovery") == 0) {
        if (!parse_bool(&c, &cfg.discovery)) {
          printf("hellion config: \"discovery\" must be true or false\n");
          return false;
        }
      } else if (strcmp(key, "pair") == 0) {
        if (!parse_string(&c, cfg.pair, sizeof cfg.pair) ||
            !fbus_valid_name(cfg.pair)) {
          printf("hellion config: \"pair\" must be a name (letters, "
                 "digits, '-' and '_')\n");
          return false;
        }
      } else if (strcmp(key, "placement") == 0) {
        if (!parse_string(&c, cfg.placement, sizeof cfg.placement) ||
            (cfg.placement[0] && strcmp(cfg.placement, "a") != 0 &&
             strcmp(cfg.placement, "b") != 0)) {
          printf("hellion config: \"placement\" must be \"a\", \"b\" or "
                 "\"\"\n");
          return false;
        }
      } else if (strcmp(key, "preview") == 0) {
        if (!parse_string(&c, cfg.preview, sizeof cfg.preview) ||
            !fbus_valid_name(cfg.preview)) {
          printf("hellion config: \"preview\" must be a stream name\n");
          return false;
        }
      } else if (strcmp(key, "snapshot_dir") == 0) {
        if (!parse_string(&c, cfg.snapshot_dir, sizeof cfg.snapshot_dir) ||
            cfg.snapshot_dir[0] != '/') {
          printf("hellion config: \"snapshot_dir\" must be an absolute "
                 "path\n");
          return false;
        }
      } else if (strcmp(key, "record_dir") == 0) {
        if (!parse_string(&c, cfg.record_dir, sizeof cfg.record_dir) ||
            cfg.record_dir[0] != '/') {
          printf("hellion config: \"record_dir\" must be an absolute path\n");
          return false;
        }
      } else if (strcmp(key, "record_from") == 0) {
        if (!parse_string(&c, cfg.record_from, sizeof cfg.record_from) ||
            (cfg.record_from[0] && !fbus_valid_name(cfg.record_from))) {
          printf("hellion config: \"record_from\" must be a pipeline name\n");
          return false;
        }
      } else if (strcmp(key, "camera_controls") == 0) {
        if (!parse_string(&c, cfg.camera_controls, sizeof cfg.camera_controls) ||
            (cfg.camera_controls[0] && strcmp(cfg.camera_controls, "peer") != 0)) {
          printf("hellion config: \"camera_controls\" must be \"peer\" (the "
                 "camera is on the other end of the tether) or \"\"\n");
          return false;
        }
      } else if (strcmp(key, "camera_file") == 0) {
        if (!parse_string(&c, cfg.camera_file, sizeof cfg.camera_file) ||
            cfg.camera_file[0] != '/') {
          printf("hellion config: \"camera_file\" must be an absolute path\n");
          return false;
        }
      } else if (strcmp(key, "eeye_config") == 0) {
        if (!parse_string(&c, cfg.eeye_config, sizeof cfg.eeye_config)) {
          printf("hellion config: bad \"eeye_config\"\n");
          return false;
        }
      } else if (strcmp(key, "pipelines") == 0) {
        if (!parse_pipelines(&c, &cfg))
          return false;
      } else {
        printf("hellion config: unknown key \"%s\"\n", key);
        return false;
      }

      int r = next_or_close(&c, '}', "the config");
      if (r < 0)
        return false;
      if (r == 0)
        break;
    }
  } else {
    c.pos++;
  }
  skip_ws(&c);
  if (c.pos != c.len) {
    printf("hellion config: trailing data after the closing '}'\n");
    return false;
  }

  if (cfg.pipeline_count == 0) {
    printf("hellion config: no pipelines -- nothing to do\n");
    return false;
  }
  for (size_t i = 0; i < cfg.pipeline_count; i++)
    for (size_t j = i + 1; j < cfg.pipeline_count; j++)
      if (strcmp(cfg.pipelines[i].name, cfg.pipelines[j].name) == 0) {
        printf("hellion config: two pipelines named \"%s\"\n",
               cfg.pipelines[i].name);
        return false;
      }
  if (cfg.record_from[0]) {
    bool found = false;
    for (size_t i = 0; i < cfg.pipeline_count; i++)
      if (strcmp(cfg.pipelines[i].name, cfg.record_from) == 0) {
        found = true;
        if (cfg.pipelines[i].source != SRC_RTP) {
          printf("hellion config: \"record_from\": pipeline \"%s\" has no "
                 "rtp source to record\n",
                 cfg.record_from);
          return false;
        }
      }
    if (!found) {
      printf("hellion config: \"record_from\": no pipeline named \"%s\"\n",
             cfg.record_from);
      return false;
    }
  }

  bool any_auto = hcfg_peer_is_auto(&cfg);
  for (size_t i = 0; i < cfg.pipeline_count; i++)
    for (size_t j = 0; j < cfg.pipelines[i].sink_count; j++)
      any_auto |= hcfg_sink_is_auto(&cfg.pipelines[i].sinks[j]);
  if (any_auto && !cfg.discovery) {
    printf("hellion config: \"auto\" addresses need \"discovery\": true\n");
    return false;
  }

  if (strcmp(cfg.camera_controls, "peer") == 0 && !cfg.peer[0]) {
    printf("hellion config: \"camera_controls\": \"peer\" needs \"peer\" "
           "set\n");
    return false;
  }

  *out = cfg;
  return true;
}

bool hcfg_sink_is_auto(const SinkConfig *s) {
  return s->kind == SINK_RTP && strncmp(s->arg, "auto:", 5) == 0;
}

bool hcfg_peer_is_auto(const HellionConfig *c) {
  return strcmp(c->peer, "auto") == 0;
}

bool hconfig_load(const char *path, HellionConfig *out) {
  FILE *f = fopen(path, "rbe");
  if (!f) {
    printf("hellion config: cannot open %s: %s\n", path, strerror(errno));
    return false;
  }
  static char buf[16384];
  size_t n = fread(buf, 1, sizeof buf, f);
  bool too_big = n == sizeof buf && fgetc(f) != EOF;
  fclose(f);
  if (too_big) {
    printf("hellion config: %s is over %zu bytes\n", path, sizeof buf);
    return false;
  }
  return hconfig_parse(buf, n, out);
}
