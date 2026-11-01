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
  } else {
    printf("hellion config: pipeline \"%s\": bad source \"%s\" (expected "
           "\"v4l2:<device>\", \"rpicam\", or \"bus:<stream>\")\n",
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
  } else {
    printf("hellion config: pipeline \"%s\": bad sink \"%s\" (expected "
           "\"bus:<stream>\" or \"v4l2:<device>\")\n",
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
  *p = (PipelineConfig){.width = 1280, .height = 720, .format = FORMAT_AUTO};
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

  *out = cfg;
  return true;
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
