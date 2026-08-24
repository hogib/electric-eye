// _POSIX_C_SOURCE for strtol/etc under -std=c23's strict ISO mode; matches
// the same guard in video_threads.c.
#define _POSIX_C_SOURCE 200809L

#include "config.h"
#include <errno.h>
#include <linux/limits.h>
#include <poll.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/inotify.h>
#include <unistd.h>

constexpr size_t config_ring_size = 8;

struct ConfigWatcher {
  atomic_bool *is_running;

  pthread_t thread;
  bool thread_started;

  int inotify_fd; // -1 means hot-reload could not be established (static mode)

  char path[PATH_MAX];
  char watch_dir[PATH_MAX];
  char watch_name[NAME_MAX + 1];

  // Single-writer ring: only ever written from config_publish(), which is
  // called either synchronously during config_watch_start() or from the
  // one reload thread -- those two writers are ordered by pthread_create's
  // happens-before, so no lock is needed on the ring itself. `current` is
  // the only field readers touch, and it's the one under atomics.
  Config ring[8];
  size_t ring_pos;
  _Atomic(const Config *) current;
};

static const Config config_defaults = {
    .stages = {{.effect = EFFECT_SOBEL}},
    .stage_count = 1,
    .record_path = "",
    .stream_frame_interval = 0, // off by default -- no JPEG work, no socket traffic
    .stream_quality = 60,
    .capture_width = 1280,
    .capture_height = 720,
    .downscale = 1, // full resolution: what this ran at before the field existed
    .capture_source = CAPTURE_AUTO,
};

// The geometry actually in force, captured by config_load_once() before any
// thread starts and never written again. Only config_publish()'s
// "you changed this, it won't take effect" warning reads it -- the pipeline
// itself carries its geometry in the arg structs it was built with.
static Config startup_geometry;
static bool startup_geometry_valid = false;

// path="/a/b/c.json" -> dir="/a/b", name="c.json". path="c.json" (no
// directory component) -> dir=".", name="c.json". Hand-rolled rather than
// libgen's dirname()/basename(): those may modify their input buffer or
// return a pointer into static storage depending on the libc, which is
// exactly the kind of surprise not worth debugging under time pressure for
// five lines of string splitting.
static void split_path(const char *path, char *dir, size_t dir_cap,
                       char *name, size_t name_cap) {
  const char *slash = strrchr(path, '/');
  if (!slash) {
    snprintf(dir, dir_cap, ".");
    snprintf(name, name_cap, "%s", path);
    return;
  }

  size_t dir_len = (size_t)(slash - path);
  if (dir_len == 0) {
    snprintf(dir, dir_cap, "/"); // path was "/something"
  } else {
    snprintf(dir, dir_cap, "%.*s", (int)dir_len, path);
  }
  snprintf(name, name_cap, "%s", slash + 1);
}

typedef struct {
  const char *s;
  size_t len;
  size_t pos;
} Cursor;

static void skip_ws(Cursor *c) {
  while (c->pos < c->len) {
    unsigned char ch = (unsigned char)c->s[c->pos];
    if (ch == ' ' || ch == '\t' || ch == '\n' || ch == '\r') {
      c->pos++;
    } else {
      break;
    }
  }
}

static bool cur_peek(const Cursor *c, char *out) {
  if (c->pos >= c->len)
    return false;
  *out = c->s[c->pos];
  return true;
}

static bool cur_expect(Cursor *c, char expected) {
  char ch;
  if (!cur_peek(c, &ch) || ch != expected)
    return false;
  c->pos++;
  return true;
}

// No escape sequences: every key and every string value in this schema is a
// plain identifier ("effect", "sobel", ...), so a backslash in the input is
// treated as a malformed file rather than something to interpret.
static bool parse_string(Cursor *c, char *out, size_t out_cap) {
  if (!cur_expect(c, '"'))
    return false;

  size_t n = 0;
  while (c->pos < c->len) {
    char ch = c->s[c->pos];
    if (ch == '"') {
      c->pos++;
      if (n >= out_cap)
        return false;
      out[n] = '\0';
      return true;
    }
    if (ch == '\\')
      return false;
    if (n + 1 >= out_cap)
      return false;
    out[n++] = ch;
    c->pos++;
  }
  return false; // ran off the end without a closing quote
}

static bool parse_u8(Cursor *c, uint8_t *out) {
  size_t start = c->pos;
  if (c->pos < c->len && c->s[c->pos] == '-')
    c->pos++; // consumed so strtol sees the sign; range check below rejects it

  bool any_digit = false;
  while (c->pos < c->len && c->s[c->pos] >= '0' && c->s[c->pos] <= '9') {
    c->pos++;
    any_digit = true;
  }
  if (!any_digit) {
    c->pos = start;
    return false;
  }

  char buf[32];
  size_t n = c->pos - start;
  if (n >= sizeof buf) { // pathological digit run; bail before strtol sees it
    c->pos = start;
    return false;
  }
  memcpy(buf, c->s + start, n);
  buf[n] = '\0';

  char *end = NULL;
  long v = strtol(buf, &end, 10);
  if (end == buf || *end != '\0' || v < 0 || v > 255) {
    c->pos = start;
    return false;
  }

  *out = (uint8_t)v;
  return true;
}

// Same shape as parse_u8 above, widened for the capture dimensions --
// those are the only config values that don't fit in a byte. Kept as a
// separate function rather than a shared parse_uint(max) because the two
// callers' error messages and range checks read better spelled out, and
// this way parse_u8's hot path (every effect parameter) stays exactly as
// it was.
static bool parse_u16(Cursor *c, uint16_t *out) {
  size_t start = c->pos;
  if (c->pos < c->len && c->s[c->pos] == '-')
    c->pos++; // consumed so strtol sees the sign; range check below rejects it

  bool any_digit = false;
  while (c->pos < c->len && c->s[c->pos] >= '0' && c->s[c->pos] <= '9') {
    c->pos++;
    any_digit = true;
  }
  if (!any_digit) {
    c->pos = start;
    return false;
  }

  char buf[32];
  size_t n = c->pos - start;
  if (n >= sizeof buf) { // pathological digit run; bail before strtol sees it
    c->pos = start;
    return false;
  }
  memcpy(buf, c->s + start, n);
  buf[n] = '\0';

  char *end = NULL;
  long v = strtol(buf, &end, 10);
  if (end == buf || *end != '\0' || v < 0 || v > 65535) {
    c->pos = start;
    return false;
  }

  *out = (uint16_t)v;
  return true;
}

// Only for error messages -- the parser itself never needs to go this
// direction. Kept beside effect_from_string so the two can't drift.
static const char *effect_to_string(EffectType e) {
  switch (e) {
  case EFFECT_NONE:      return "none";
  case EFFECT_GRAYSCALE: return "grayscale";
  case EFFECT_INVERT:    return "invert";
  case EFFECT_THRESHOLD: return "threshold";
  case EFFECT_TINT:      return "tint";
  case EFFECT_SOBEL:     return "sobel";
  case EFFECT_BLUR:      return "blur";
  case EFFECT_CONTRAST:  return "contrast";
  case EFFECT_LIGHT:     return "light";
  }
  return "?";
}

static bool effect_from_string(const char *s, EffectType *out) {
  if (strcmp(s, "none") == 0)
    *out = EFFECT_NONE;
  else if (strcmp(s, "grayscale") == 0)
    *out = EFFECT_GRAYSCALE;
  else if (strcmp(s, "invert") == 0)
    *out = EFFECT_INVERT;
  else if (strcmp(s, "threshold") == 0)
    *out = EFFECT_THRESHOLD;
  else if (strcmp(s, "tint") == 0)
    *out = EFFECT_TINT;
  else if (strcmp(s, "sobel") == 0)
    *out = EFFECT_SOBEL;
  else if (strcmp(s, "blur") == 0)
    *out = EFFECT_BLUR;
  else if (strcmp(s, "contrast") == 0)
    *out = EFFECT_CONTRAST;
  else if (strcmp(s, "light") == 0)
    *out = EFFECT_LIGHT;
  else
    return false;
  return true;
}

// Parses one {"effect": "...", ...} chain-stage object. Mirrors
// parse_config's own key loop and its "unknown key is a hard error"
// policy (a stage carrying a key its effect doesn't use, e.g. tint_u on a
// blur stage, is rejected rather than silently ignored).
// One bit per optional stage parameter, so a stage's keys can be checked
// against its effect after the whole object is parsed.
enum {
  PARAM_THRESHOLD_VALUE = 1u << 0,
  PARAM_TINT_U = 1u << 1,
  PARAM_TINT_V = 1u << 2,
  PARAM_TINT_STRENGTH = 1u << 3,
  PARAM_SOBEL_THRESHOLD = 1u << 4,
  PARAM_BLUR_STRENGTH = 1u << 5,
  PARAM_LIGHT_LEVEL = 1u << 6,
};

// Which parameters each effect actually reads. none/grayscale/invert/
// contrast take none at all: contrast is a full-frame auto stretch with
// nothing to tune, and the others are fixed transforms.
static uint32_t params_for_effect(EffectType effect) {
  switch (effect) {
  case EFFECT_THRESHOLD:
    return PARAM_THRESHOLD_VALUE;
  case EFFECT_TINT:
    return PARAM_TINT_U | PARAM_TINT_V | PARAM_TINT_STRENGTH;
  case EFFECT_SOBEL:
    return PARAM_SOBEL_THRESHOLD;
  case EFFECT_BLUR:
    return PARAM_BLUR_STRENGTH;
  case EFFECT_LIGHT:
    return PARAM_LIGHT_LEVEL;
  case EFFECT_NONE:
  case EFFECT_GRAYSCALE:
  case EFFECT_INVERT:
  case EFFECT_CONTRAST:
    return 0;
  }
  return 0;
}

static const char *param_name(uint32_t bit) {
  switch (bit) {
  case PARAM_THRESHOLD_VALUE: return "threshold_value";
  case PARAM_TINT_U:          return "tint_u";
  case PARAM_TINT_V:          return "tint_v";
  case PARAM_TINT_STRENGTH:   return "tint_strength";
  case PARAM_SOBEL_THRESHOLD: return "sobel_threshold";
  case PARAM_BLUR_STRENGTH:   return "blur_strength";
  case PARAM_LIGHT_LEVEL:     return "light_level";
  default:                    return "?";
  }
}

static bool parse_effect_stage(Cursor *c, EffectStage *out) {
  EffectStage parsed = {0};
  // Every other field's natural zero-init default happens to already mean
  // "no visible effect" for its own effect (tint_strength 0, sobel/blur's
  // documented zero-behavior, etc). light_level's neutral point is 128, not
  // 0, so it needs an explicit seed here or omitting it on a "light" stage
  // would silently mean "fully dark and desaturated" instead of "no
  // change" -- see EffectStage's own doc comment on light_level.
  parsed.light_level = 128;
  bool have_effect = false;
  // Which parameter keys this stage carried, so they can be checked
  // against the effect once it is known -- key order isn't fixed, so a
  // parameter may well be parsed before "effect" is.
  uint32_t seen_params = 0;

  if (!cur_expect(c, '{')) {
    printf("Config: expected '{' to start a chain stage\n");
    return false;
  }
  skip_ws(c);

  char ch;
  if (cur_peek(c, &ch) && ch == '}') {
    c->pos++;
  } else {
    for (;;) {
      char key[32];
      skip_ws(c);
      if (!parse_string(c, key, sizeof key)) {
        printf("Config: expected a quoted key in a chain stage\n");
        return false;
      }
      skip_ws(c);
      if (!cur_expect(c, ':')) {
        printf("Config: expected ':' after key \"%s\" in a chain stage\n",
               key);
        return false;
      }
      skip_ws(c);

      bool ok;
      if (strcmp(key, "effect") == 0) {
        char val[32];
        ok = parse_string(c, val, sizeof val) &&
             effect_from_string(val, &parsed.effect);
        have_effect = ok;
      } else if (strcmp(key, "threshold_value") == 0) {
        ok = parse_u8(c, &parsed.threshold_value);
        seen_params |= PARAM_THRESHOLD_VALUE;
      } else if (strcmp(key, "tint_u") == 0) {
        ok = parse_u8(c, &parsed.tint_u);
        seen_params |= PARAM_TINT_U;
      } else if (strcmp(key, "tint_v") == 0) {
        ok = parse_u8(c, &parsed.tint_v);
        seen_params |= PARAM_TINT_V;
      } else if (strcmp(key, "tint_strength") == 0) {
        ok = parse_u8(c, &parsed.tint_strength);
        seen_params |= PARAM_TINT_STRENGTH;
      } else if (strcmp(key, "sobel_threshold") == 0) {
        ok = parse_u8(c, &parsed.sobel_threshold);
        seen_params |= PARAM_SOBEL_THRESHOLD;
      } else if (strcmp(key, "blur_strength") == 0) {
        ok = parse_u8(c, &parsed.blur_strength);
        seen_params |= PARAM_BLUR_STRENGTH;
      } else if (strcmp(key, "light_level") == 0) {
        ok = parse_u8(c, &parsed.light_level);
        seen_params |= PARAM_LIGHT_LEVEL;
      } else {
        printf("Config: unknown key \"%s\" in a chain stage\n", key);
        return false;
      }
      if (!ok) {
        printf("Config: invalid value for \"%s\" in a chain stage\n", key);
        return false;
      }

      skip_ws(c);
      if (!cur_peek(c, &ch)) {
        printf("Config: unterminated chain stage object\n");
        return false;
      }
      if (ch == ',') {
        c->pos++;
        continue;
      }
      if (ch == '}') {
        c->pos++;
        break;
      }
      printf("Config: expected ',' or '}' after value for \"%s\" in a "
             "chain stage\n",
             key);
      return false;
    }
  }

  if (!have_effect) {
    printf("Config: chain stage missing required \"effect\" key\n");
    return false;
  }

  // A parameter that belongs to a different effect is a typo, not a
  // harmless extra -- accepting it silently means an operator sets a value
  // that never takes effect and has nothing to tell them so. Same
  // reasoning as the unknown-key error above; this is the case where the
  // key is spelled correctly but attached to the wrong stage.
  uint32_t stray = seen_params & ~params_for_effect(parsed.effect);
  if (stray) {
    for (uint32_t bit = 1; bit; bit <<= 1) {
      if (stray & bit) {
        printf("Config: \"%s\" is not a parameter of the \"%s\" effect\n",
               param_name(bit), effect_to_string(parsed.effect));
        break;
      }
    }
    return false;
  }

  *out = parsed;
  return true;
}

// Parses the "chain" array: zero or more stage objects, capped at
// max_chain_stages (a hard error past that, not silent truncation -- a
// chain quietly losing its last stages would be a confusing way to find
// out the limit exists).
static bool parse_chain(Cursor *c, EffectStage *stages, size_t *stage_count) {
  if (!cur_expect(c, '[')) {
    printf("Config: expected '[' for \"chain\"\n");
    return false;
  }
  skip_ws(c);

  size_t count = 0;
  char ch;
  if (cur_peek(c, &ch) && ch == ']') {
    c->pos++; // empty chain: a valid pass-through
  } else {
    for (;;) {
      skip_ws(c);
      if (count >= max_chain_stages) {
        printf("Config: \"chain\" has more than %zu stages\n",
               max_chain_stages);
        return false;
      }
      if (!parse_effect_stage(c, &stages[count])) {
        return false; // parse_effect_stage already explained why
      }
      count++;

      skip_ws(c);
      if (!cur_peek(c, &ch)) {
        printf("Config: unterminated \"chain\" array\n");
        return false;
      }
      if (ch == ',') {
        c->pos++;
        continue;
      }
      if (ch == ']') {
        c->pos++;
        break;
      }
      printf("Config: expected ',' or ']' in \"chain\"\n");
      return false;
    }
  }

  *stage_count = count;
  return true;
}

// Unknown keys are a hard parse error rather than being skipped. A silently
// ignored typo ("efect" instead of "effect") is a much worse failure mode
// than the whole reload being rejected and logged -- it would look like the
// config took effect when it didn't.
static bool parse_config(const char *buf, size_t len, Config *out) {
  Cursor c = {.s = buf, .len = len, .pos = 0};
  Config parsed = config_defaults;

  skip_ws(&c);
  if (!cur_expect(&c, '{')) {
    printf("Config: expected '{' at start of file\n");
    return false;
  }
  skip_ws(&c);

  char ch;
  if (cur_peek(&c, &ch) && ch == '}') {
    c.pos++; // empty object: every field takes its default
  } else {
    for (;;) {
      char key[32];
      skip_ws(&c);
      if (!parse_string(&c, key, sizeof key)) {
        printf("Config: expected a quoted key\n");
        return false;
      }
      skip_ws(&c);
      if (!cur_expect(&c, ':')) {
        printf("Config: expected ':' after key \"%s\"\n", key);
        return false;
      }
      skip_ws(&c);

      if (strcmp(key, "chain") == 0) {
        if (!parse_chain(&c, parsed.stages, &parsed.stage_count))
          return false; // parse_chain/parse_effect_stage already explained why
      } else if (strcmp(key, "record_path") == 0) {
        if (!parse_string(&c, parsed.record_path, sizeof parsed.record_path)) {
          printf("Config: invalid value for \"record_path\"\n");
          return false;
        }
      } else if (strcmp(key, "stream_frame_interval") == 0) {
        if (!parse_u8(&c, &parsed.stream_frame_interval)) {
          printf("Config: invalid value for \"stream_frame_interval\"\n");
          return false;
        }
      } else if (strcmp(key, "stream_quality") == 0) {
        if (!parse_u8(&c, &parsed.stream_quality)) {
          printf("Config: invalid value for \"stream_quality\"\n");
          return false;
        }
      } else if (strcmp(key, "capture_width") == 0) {
        if (!parse_u16(&c, &parsed.capture_width)) {
          printf("Config: invalid value for \"capture_width\"\n");
          return false;
        }
      } else if (strcmp(key, "capture_height") == 0) {
        if (!parse_u16(&c, &parsed.capture_height)) {
          printf("Config: invalid value for \"capture_height\"\n");
          return false;
        }
      } else if (strcmp(key, "capture_source") == 0) {
        char src[16];
        if (!parse_string(&c, src, sizeof src)) {
          printf("Config: invalid value for \"capture_source\"\n");
          return false;
        }
        if (strcmp(src, "auto") == 0) {
          parsed.capture_source = CAPTURE_AUTO;
        } else if (strcmp(src, "v4l2") == 0) {
          parsed.capture_source = CAPTURE_V4L2;
        } else if (strcmp(src, "rpicam") == 0) {
          parsed.capture_source = CAPTURE_RPICAM;
        } else {
          printf("Config: unknown \"capture_source\": \"%s\" (expected "
                 "\"auto\", \"v4l2\", or \"rpicam\")\n",
                 src);
          return false;
        }
      } else if (strcmp(key, "downscale") == 0) {
        if (!parse_u8(&c, &parsed.downscale)) {
          printf("Config: invalid value for \"downscale\"\n");
          return false;
        }
      } else {
        printf("Config: unknown key \"%s\"\n", key);
        return false;
      }

      skip_ws(&c);
      if (!cur_peek(&c, &ch)) {
        printf("Config: unterminated object\n");
        return false;
      }
      if (ch == ',') {
        c.pos++;
        continue;
      }
      if (ch == '}') {
        c.pos++;
        break;
      }
      printf("Config: expected ',' or '}' after value for \"%s\"\n", key);
      return false;
    }
  }

  skip_ws(&c);
  if (c.pos != c.len) {
    printf("Config: trailing data after closing '}'\n");
    return false;
  }

  // Geometry validation happens here, once the whole object is parsed,
  // rather than at each key: downscale and the two dimensions constrain
  // each other, and the keys can appear in any order, so none of them can
  // be fully checked in isolation.
  if (parsed.downscale != 1 && parsed.downscale != 2 && parsed.downscale != 4 &&
      parsed.downscale != 8) {
    printf("Config: \"downscale\" must be 1, 2, 4, or 8 (got %u) -- see the "
           "geometry note in config.h for why it isn't an arbitrary ratio\n",
           parsed.downscale);
    return false;
  }
  if (parsed.capture_width == 0 || parsed.capture_height == 0) {
    printf("Config: \"capture_width\"/\"capture_height\" must both be "
           "non-zero (got %ux%u)\n",
           parsed.capture_width, parsed.capture_height);
    return false;
  }
  // The output frame is I422: its chroma planes are half its luma width.
  // Requiring the *output* width to be even (hence capture width divisible
  // by 2*downscale) keeps that halving exact, so the YUYV box-average and
  // libjpeg-turbo's scaled decode agree on plane sizes instead of
  // disagreeing by a half-sampled edge column.
  //
  // Height carries the same doubling for a second, independent reason: a
  // 4:2:0 source (what rpicam-vid produces) has half-height chroma that
  // jpeg_decode.c line-doubles into I422's full-height planes, which only
  // fills the plane exactly when the output height is even. Requiring
  // capture height to be a multiple of 2*downscale covers both layouts
  // with one rule rather than making the valid geometries depend on which
  // camera happens to be attached.
  uint32_t wstep = (uint32_t)parsed.downscale * 2u;
  if (parsed.capture_width % wstep != 0 ||
      parsed.capture_height % wstep != 0) {
    printf("Config: %ux%u doesn't divide evenly by \"downscale\": %u -- "
           "both width and height must be a multiple of %u\n",
           parsed.capture_width, parsed.capture_height, parsed.downscale,
           wstep);
    return false;
  }

  *out = parsed;
  return true;
}

void config_output_geometry(const Config *cfg, uint32_t *out_width,
                            uint32_t *out_height) {
  uint32_t d = cfg->downscale ? cfg->downscale : 1u;
  *out_width = (uint32_t)cfg->capture_width / d;
  *out_height = (uint32_t)cfg->capture_height / d;
}

static bool config_try_load(const char *path, Config *out) {
  FILE *f = fopen(path, "rb");
  if (!f) {
    printf("Config: could not open %s: %s\n", path, strerror(errno));
    return false;
  }

  char buf[4096];
  size_t n = fread(buf, 1, sizeof buf, f);

  // fread() filling the buffer exactly doesn't by itself mean the file is
  // truncated -- a file of precisely sizeof(buf) bytes reads fully without
  // ever setting feof() (that only happens once a read attempts to go past
  // the end and comes up short). So a full read is ambiguous on its own;
  // resolve it with one more byte: EOF here means the file was exactly
  // sizeof(buf), not truncated, and fgetc() is cheap enough that doing this
  // unconditionally would be fine too -- it's guarded here only to skip it
  // on the far more common case of a small config file.
  bool hit_limit = false;
  if (n == sizeof buf) {
    hit_limit = fgetc(f) != EOF;
  }
  fclose(f);

  if (hit_limit) {
    printf("Config: %s exceeds the %zu byte limit; refusing to guess at a "
           "possibly-truncated read\n",
           path, sizeof buf);
    return false;
  }

  return parse_config(buf, n, out);
}

bool config_load_once(const char *path, Config *out) {
  *out = config_defaults;

  // A missing file is the documented "just use the defaults" case, matching
  // config_watch_start; anything else (present but unreadable, malformed,
  // over the size limit) is a real error worth refusing to start on, since
  // there is no earlier geometry to keep running with.
  FILE *probe = fopen(path, "rb");
  if (!probe) {
    printf("Config: %s not found; using default geometry (%ux%u, downscale "
           "%u)\n",
           path, out->capture_width, out->capture_height, out->downscale);
  } else {
    fclose(probe);
    if (!config_try_load(path, out))
      return false; // config_try_load/parse_config already explained why
  }

  startup_geometry = *out;
  startup_geometry_valid = true;
  return true;
}

static void config_publish(ConfigWatcher *w, const Config *parsed) {
  // Geometry is startup-only (see Config's own note): warn rather than
  // silently ignore, since a config field that appears to change but does
  // nothing is exactly the kind of thing that costs an hour to notice.
  // Only the *pipeline* can't follow a geometry change -- the parsed value
  // is still published as-is, so config_current() keeps reporting whatever
  // is actually in the file.
  if (startup_geometry_valid &&
      (parsed->capture_width != startup_geometry.capture_width ||
       parsed->capture_height != startup_geometry.capture_height ||
       parsed->downscale != startup_geometry.downscale)) {
    printf("Config: geometry changed (%ux%u/downscale %u -> %ux%u/downscale "
           "%u) but it only takes effect at startup -- still running at the "
           "original size. Restart eeye to apply it.\n",
           startup_geometry.capture_width, startup_geometry.capture_height,
           startup_geometry.downscale, parsed->capture_width,
           parsed->capture_height, parsed->downscale);
  }

  // Same reasoning for capture_source: startup-only, so a change that looks
  // applied but isn't gets said out loud. Kept as its own check rather than
  // folded into the geometry one above so the message names the field that
  // actually changed instead of printing sizes that didn't.
  if (startup_geometry_valid &&
      parsed->capture_source != startup_geometry.capture_source) {
    static const char *const names[] = {"auto", "v4l2", "rpicam"};
    printf("Config: \"capture_source\" changed (%s -> %s) but it only takes "
           "effect at startup -- still capturing from the original source. "
           "Restart eeye to apply it.\n",
           names[startup_geometry.capture_source],
           names[parsed->capture_source]);
  }

  size_t slot = w->ring_pos % config_ring_size;
  w->ring_pos++;
  w->ring[slot] = *parsed;
  atomic_store_explicit(&w->current, &w->ring[slot], memory_order_release);
}

// Writes "<config_path>.status" with a one-line {"ok":true|false} verdict
// after every load/reload attempt -- config_try_load() (called just before
// this, both here and in config_watch_start()) already printed the full
// reason on failure, so this file only needs to carry the yes/no a remote
// writer (e.g. pi/config_agent.py) can't otherwise get: whether the config
// it just wrote actually took effect, as opposed to being silently
// rejected in favor of whatever was running before. Same writer contract
// as the config file itself (temp file in the same directory, then
// rename()) for the same reason -- a reader must never observe a
// half-written status file.
//
// Best-effort: a failure to write this must never affect the pipeline
// itself, so this only logs and returns, exactly like the printf-only
// reporting it's layered on top of.
static void write_reload_status(const char *config_path, bool ok) {
  char status_path[PATH_MAX + 8];
  char tmp_path[PATH_MAX + 16];
  snprintf(status_path, sizeof status_path, "%s.status", config_path);
  snprintf(tmp_path, sizeof tmp_path, "%s.status.tmp", config_path);

  FILE *f = fopen(tmp_path, "wb");
  if (!f) {
    printf("Config: could not write reload status to %s: %s\n", tmp_path,
           strerror(errno));
    return;
  }
  fprintf(f, "{\"ok\":%s}\n", ok ? "true" : "false");
  fclose(f);

  if (rename(tmp_path, status_path) < 0) {
    printf("Config: could not publish reload status to %s: %s\n",
           status_path, strerror(errno));
  }
}

static void *config_watch_thread(void *arg) {
  ConfigWatcher *w = (ConfigWatcher *)arg;
  // inotify_event has a flexible-array-member `name`; alignment matters
  // because the buffer is walked as a sequence of these structs packed
  // back-to-back, not just read as raw bytes.
  char buf[4096] __attribute__((aligned(__alignof__(struct inotify_event))));

  while (atomic_load(w->is_running)) {
    struct pollfd pfd = {.fd = w->inotify_fd, .events = POLLIN, .revents = 0};
    // Short timeout so a shutdown request is noticed within 200ms instead
    // of blocking on read() until the next file change, which might never
    // come during the time the caller is waiting to join this thread.
    int pr = poll(&pfd, 1, 200);
    if (pr <= 0)
      continue; // timeout or EINTR either way -- loop back and recheck

    ssize_t n = read(w->inotify_fd, buf, sizeof buf);
    if (n <= 0)
      continue;

    for (char *p = buf; p < buf + n;) {
      struct inotify_event *ev = (struct inotify_event *)p;
      if (ev->len > 0 && strcmp(ev->name, w->watch_name) == 0) {
        Config parsed;
        bool ok = config_try_load(w->path, &parsed);
        if (ok) {
          config_publish(w, &parsed);
          printf("Config: reloaded %s\n", w->path);
        } else {
          printf("Config: reload of %s failed; keeping previous config\n",
                 w->path);
        }
        write_reload_status(w->path, ok);
      }
      p += (ptrdiff_t)(sizeof(struct inotify_event) + ev->len);
    }
  }

  return NULL;
}

ConfigWatcher *config_watch_start(const char *path, atomic_bool *is_running) {
  ConfigWatcher *w = (ConfigWatcher *)calloc(1, sizeof(ConfigWatcher));
  if (!w)
    return NULL;

  w->is_running = is_running;
  w->inotify_fd = -1;
  snprintf(w->path, sizeof w->path, "%s", path);

  Config initial;
  bool initial_ok = config_try_load(path, &initial);
  if (initial_ok) {
    printf("Config: loaded %s\n", path);
  } else {
    printf("Config: %s not usable at startup, using built-in defaults\n",
           path);
    initial = config_defaults;
  }
  config_publish(w, &initial);
  write_reload_status(path, initial_ok);

  split_path(path, w->watch_dir, sizeof w->watch_dir, w->watch_name,
            sizeof w->watch_name);

  int fd = inotify_init1(IN_CLOEXEC);
  if (fd < 0) {
    printf("Config: inotify_init1 failed (%s); hot-reload disabled, running "
           "with the config loaded at startup\n",
           strerror(errno));
    return w;
  }

  // Watch the directory, not the file: a watch on the file's inode goes
  // stale the instant an atomic writer's rename() replaces it, and a plain
  // in-place overwrite could be observed mid-write. IN_MOVED_TO catches
  // write-temp-then-rename; IN_CLOSE_WRITE catches an editor that
  // truncates and rewrites the file in place instead.
  if (inotify_add_watch(fd, w->watch_dir, IN_CLOSE_WRITE | IN_MOVED_TO) < 0) {
    printf("Config: could not watch %s (%s); hot-reload disabled, running "
           "with the config loaded at startup\n",
           w->watch_dir, strerror(errno));
    close(fd);
    return w;
  }

  w->inotify_fd = fd;

  if (pthread_create(&w->thread, NULL, config_watch_thread, w) != 0) {
    printf("Config: failed to start reload thread; hot-reload disabled\n");
    close(w->inotify_fd);
    w->inotify_fd = -1;
    return w;
  }
  w->thread_started = true;

  return w;
}

void config_watch_stop(ConfigWatcher *watcher) {
  if (!watcher)
    return;

  if (watcher->thread_started)
    pthread_join(watcher->thread, NULL);

  if (watcher->inotify_fd >= 0)
    close(watcher->inotify_fd);

  free(watcher);
}

const Config *config_current(const ConfigWatcher *watcher) {
  return atomic_load_explicit(&watcher->current, memory_order_acquire);
}
