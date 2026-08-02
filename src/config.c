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
    .effect = EFFECT_SOBEL,
    .threshold_value = 128,
    .tint_u = 90,
    .tint_v = 150,
    .tint_strength = 180,
};

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
  else
    return false;
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

      bool ok;
      if (strcmp(key, "effect") == 0) {
        char val[32];
        ok = parse_string(&c, val, sizeof val) &&
             effect_from_string(val, &parsed.effect);
      } else if (strcmp(key, "threshold_value") == 0) {
        ok = parse_u8(&c, &parsed.threshold_value);
      } else if (strcmp(key, "tint_u") == 0) {
        ok = parse_u8(&c, &parsed.tint_u);
      } else if (strcmp(key, "tint_v") == 0) {
        ok = parse_u8(&c, &parsed.tint_v);
      } else if (strcmp(key, "tint_strength") == 0) {
        ok = parse_u8(&c, &parsed.tint_strength);
      } else {
        printf("Config: unknown key \"%s\"\n", key);
        return false;
      }
      if (!ok) {
        printf("Config: invalid value for \"%s\"\n", key);
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

  *out = parsed;
  return true;
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

static void config_publish(ConfigWatcher *w, const Config *parsed) {
  size_t slot = w->ring_pos % config_ring_size;
  w->ring_pos++;
  w->ring[slot] = *parsed;
  atomic_store_explicit(&w->current, &w->ring[slot], memory_order_release);
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
        if (config_try_load(w->path, &parsed)) {
          config_publish(w, &parsed);
          printf("Config: reloaded %s\n", w->path);
        } else {
          printf("Config: reload of %s failed; keeping previous config\n",
                 w->path);
        }
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
  if (config_try_load(path, &initial)) {
    printf("Config: loaded %s\n", path);
  } else {
    printf("Config: %s not usable at startup, using built-in defaults\n",
           path);
    initial = config_defaults;
  }
  config_publish(w, &initial);

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
