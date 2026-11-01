// `hellion top`: the dashboard for an SSH session. Plain ANSI, no curses,
// so it runs over any SSH connection and in any terminal.
#define _POSIX_C_SOURCE 200809L

#include "fbus.h"
#include "hellion.h"
#include "hstatus.h"
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

static const int refresh_ms = 500;
// Older than this and an instance is shown as not responding.
static const double heartbeat_stale_s = 2.0;

// ---- Output with colors and width-limited lines ---------------------------

typedef enum { C_NONE, C_OK, C_WARN, C_BAD, C_DIM, C_HEAD } Color;

static bool use_color;
static int term_cols = 100;

// The line being built: visible characters are counted so the line can be
// cut at the terminal's width without cutting an escape sequence in half.
static char line_buf[4096];
static size_t line_len;
static int line_visible;

static void line_reset(void) {
  line_len = 0;
  line_visible = 0;
}

static void raw_append(const char *s) {
  size_t n = strlen(s);
  if (line_len + n < sizeof line_buf) {
    memcpy(line_buf + line_len, s, n);
    line_len += n;
  }
}

static void seg(Color color, const char *fmt, ...) {
  char text[512];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(text, sizeof text, fmt, ap);
  va_end(ap);

  static const char *const codes[] = {"", "\033[32m", "\033[33m", "\033[31m",
                                      "\033[2m", "\033[1m"};
  if (use_color && color != C_NONE)
    raw_append(codes[color]);
  for (const char *p = text; *p && line_visible < term_cols; p++) {
    char one[2] = {*p, 0};
    raw_append(one);
    line_visible++;
  }
  if (use_color && color != C_NONE)
    raw_append("\033[0m");
}

static void line_end(void) {
  line_buf[line_len] = '\0';
  // \033[K clears whatever an earlier, longer frame left on this row.
  printf("%s%s\n", line_buf, use_color ? "\033[K" : "");
  line_reset();
}

static Color state_color(const char *state) {
  static const char *const good[] = {"running", "publishing", "writing",
                                     "sending", "receiving", "alive", "ok"};
  static const char *const meh[] = {"waiting", "idle", "starting", "stale"};
  for (size_t i = 0; i < sizeof good / sizeof good[0]; i++)
    if (strcmp(state, good[i]) == 0)
      return C_OK;
  for (size_t i = 0; i < sizeof meh / sizeof meh[0]; i++)
    if (strcmp(state, meh[i]) == 0)
      return C_WARN;
  return C_BAD;
}

static void fmt_duration(char *out, size_t cap, double s) {
  if (s < 60)
    snprintf(out, cap, "%.0fs", s);
  else if (s < 3600)
    snprintf(out, cap, "%dm%02ds", (int)s / 60, (int)s % 60);
  else
    snprintf(out, cap, "%dh%02dm", (int)s / 3600, ((int)s % 3600) / 60);
}

// ---- Frame bus streams: fps comes from two looks at the counters --------

enum { max_streams = 32 };
typedef struct {
  char name[fbus_max_stream_name];
  uint64_t frames;
  uint64_t at_ns;
  double fps;
  bool seen;
} StreamMemo;
static StreamMemo memo[max_streams];

static uint64_t mono_ns(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

static double stream_fps(const char *name, uint64_t frames) {
  uint64_t now = mono_ns();
  StreamMemo *m = NULL;
  for (int i = 0; i < max_streams && !m; i++)
    if (memo[i].seen && strcmp(memo[i].name, name) == 0)
      m = &memo[i];
  if (!m) {
    for (int i = 0; i < max_streams && !m; i++)
      if (!memo[i].seen)
        m = &memo[i];
    if (!m)
      return 0;
    *m = (StreamMemo){.seen = true, .frames = frames, .at_ns = now};
    snprintf(m->name, sizeof m->name, "%s", name);
    return 0;
  }
  if (frames < m->frames) { // the writer restarted
    m->frames = frames;
    m->at_ns = now;
    m->fps = 0;
  } else if (now - m->at_ns >= 400000000ull) {
    m->fps = (double)(frames - m->frames) * 1e9 / (double)(now - m->at_ns);
    m->frames = frames;
    m->at_ns = now;
  }
  return m->fps;
}

static void collect_name(const char *name, void *ctx) {
  char(*names)[fbus_max_stream_name] = ctx;
  for (int i = 0; i < max_streams; i++)
    if (!names[i][0]) {
      snprintf(names[i], fbus_max_stream_name, "%s", name);
      return;
    }
}

static int cmp_names(const void *a, const void *b) {
  return strcmp((const char *)a, (const char *)b);
}

static void draw_bus(void) {
  char names[max_streams][fbus_max_stream_name];
  memset(names, 0, sizeof names);
  fbus_list(collect_name, names);
  int n = 0;
  while (n < max_streams && names[n][0])
    n++;
  qsort(names, (size_t)n, sizeof names[0], cmp_names);

  seg(C_HEAD, "FRAME BUS");
  seg(C_DIM, "  %s", fbus_dir());
  line_end();
  if (n == 0) {
    seg(C_DIM, "  no streams");
    line_end();
  }
  for (int i = 0; i < n; i++) {
    FbusStatus st;
    FbusOpenResult r = fbus_status(names[i], &st);
    seg(C_NONE, "  %-12s ", names[i]);
    if (r != FBUS_OPEN_OK) {
      seg(C_BAD, "%s", fbus_open_result_str(r));
      line_end();
      continue;
    }
    const char *state = st.closed ? "closed" : st.alive ? "alive" : "stale";
    double fps = stream_fps(names[i], st.frames);
    seg(C_NONE, "%5ux%-5u ", st.width, st.height);
    seg(state_color(state), "%-7s", state);
    seg(C_NONE, " %5.1f fps  frame %-8lld", fps, (long long)st.last_frame_no);
    if (st.frames)
      seg(st.last_frame_age_ns > 1000000000ull ? C_WARN : C_DIM,
          "  last %.2fs ago", (double)st.last_frame_age_ns / 1e9);
    if (!st.alive && !st.closed)
      seg(C_BAD, "  writer gone %.0fs (pid %d)",
          (double)st.heartbeat_age_ns / 1e9, st.writer_pid);
    line_end();
  }
}

// ---- eeye, read from the files beside its config --------------------------

static bool read_small_file(const char *path, char *buf, size_t cap,
                            double *age_s) {
  FILE *f = fopen(path, "rbe");
  if (!f)
    return false;
  size_t n = fread(buf, 1, cap - 1, f);
  buf[n] = '\0';
  struct stat st;
  if (age_s && fstat(fileno(f), &st) == 0)
    *age_s = difftime(time(NULL), st.st_mtime);
  fclose(f);
  return true;
}

// The health file is eeye's own small JSON with unique key names, so a
// plain search is enough to pick fields out for display.
static bool json_field(const char *json, const char *key, char *out,
                       size_t cap) {
  char pat[64];
  snprintf(pat, sizeof pat, "\"%s\":", key);
  const char *p = strstr(json, pat);
  if (!p)
    return false;
  p += strlen(pat);
  if (*p == '"') {
    p++;
    size_t n = 0;
    while (*p && *p != '"' && n + 1 < cap)
      out[n++] = *p++;
    out[n] = '\0';
  } else {
    size_t n = 0;
    while (*p && *p != ',' && *p != '}' && n + 1 < cap)
      out[n++] = *p++;
    out[n] = '\0';
  }
  return true;
}

static void draw_eeye(const char *config_path) {
  seg(C_HEAD, "EEYE");
  seg(C_DIM, "  %s", config_path);
  line_end();

  char path[512], health[2048], status[256];
  double age = 0;
  snprintf(path, sizeof path, "%s.health", config_path);
  if (!read_small_file(path, health, sizeof health, &age)) {
    seg(C_BAD, "  no health file -- is eeye running with this config?");
    line_end();
  } else {
    char connected[8] = "?", frames[24] = "?", w[8] = "?", h[8] = "?",
         rec[16] = "?", rec_err[128] = "";
    json_field(health, "connected", connected, sizeof connected);
    json_field(health, "frames", frames, sizeof frames);
    json_field(health, "width", w, sizeof w);
    json_field(health, "height", h, sizeof h);
    json_field(health, "state", rec, sizeof rec);
    json_field(health, "error", rec_err, sizeof rec_err);
    seg(C_NONE, "  health ");
    // eeye republishes it every second while frames flow.
    seg(age > 3 ? C_WARN : C_OK, "%.0fs old", age);
    seg(C_NONE, "  camera ");
    seg(strcmp(connected, "true") == 0 ? C_OK : C_BAD, "%s",
        strcmp(connected, "true") == 0 ? "connected" : "disconnected");
    seg(C_NONE, "  %sx%s  %s frames  recording ", w, h, frames);
    seg(strcmp(rec, "failed") == 0 ? C_BAD : C_NONE, "%s", rec);
    if (rec_err[0])
      seg(C_BAD, " (%s)", rec_err);
    line_end();
  }

  snprintf(path, sizeof path, "%s.status", config_path);
  if (read_small_file(path, status, sizeof status, &age)) {
    bool ok = strstr(status, "true") != NULL;
    seg(C_NONE, "  last config load ");
    seg(ok ? C_OK : C_BAD, "%s", ok ? "accepted" : "REJECTED (see eeye's log)");
    char ago[16];
    fmt_duration(ago, sizeof ago, age);
    seg(C_DIM, "  %s ago", ago);
    line_end();
  }
}

// ---- One hellion instance -------------------------------------------------

static void draw_endpoint(const char *role, const HsEndpoint *e,
                          bool is_sink) {
  seg(C_DIM, "  %-6s ", role);
  seg(C_NONE, "%-26s ", e->spec);
  seg(state_color(e->state), "%-11s", e->state);
  seg(C_NONE, " %5.1f fps %9llu frames", e->fps,
      (unsigned long long)e->frames);
  if (is_sink)
    seg(e->dropped ? C_WARN : C_DIM, " %6llu dropped",
        (unsigned long long)e->dropped);
  if (e->detail[0])
    seg(C_DIM, "  %s", e->detail);
  line_end();

  // The link under an RTP endpoint, on its own line.
  if (e->net) {
    seg(C_DIM, "  %-6s ", "");
    seg(C_DIM, "%-26s ", is_sink ? "  link out" : "  link in");
    if (e->idle_s < 0 || e->idle_s > 2.0) {
      seg(C_WARN, "no packets%s",
          e->idle_s < 0 ? " yet" : "");
      if (e->idle_s > 2.0)
        seg(C_DIM, " for %.0fs", e->idle_s);
    } else {
      seg(C_NONE, "%7.0f kbit/s", e->kbps);
      seg(e->loss_pct >= 1.0   ? C_BAD
          : e->loss_pct > 0.0 ? C_WARN
                              : C_OK,
          "  %5.1f%% loss", e->loss_pct);
    }
    seg(C_DIM, "  %llu packets, %llu lost in all",
        (unsigned long long)e->packets, (unsigned long long)e->lost);
    line_end();
  }
}

static void draw_instance(const HsStatus *s) {
  double beat = hs_heartbeat_age(s);
  struct timespec now;
  clock_gettime(CLOCK_REALTIME, &now);
  double up = (double)now.tv_sec - (double)(s->started_real_ns / 1000000000ull);
  char upbuf[16];
  fmt_duration(upbuf, sizeof upbuf, up);

  seg(C_HEAD, "HELLION \"%s\"", s->name);
  seg(C_NONE, "  role %s  pid %d  up %s  ", s->role, s->pid, upbuf);
  if (beat > heartbeat_stale_s)
    seg(C_BAD, "NOT RESPONDING (last heartbeat %.0fs ago)", beat);
  else
    seg(C_OK, "responding");
  line_end();

  for (uint32_t i = 0; i < s->pipeline_count; i++) {
    const HsPipeline *p = &s->pipelines[i];
    seg(C_HEAD, " pipeline %s", p->name);
    if (p->width)
      seg(C_DIM, "  %ux%u", p->width, p->height);
    line_end();
    draw_endpoint("source", &p->source, false);
    for (uint32_t k = 0; k < p->sink_count; k++)
      draw_endpoint("sink", &p->sinks[k], true);
  }

  if (s->recording[0]) {
    seg(C_HEAD, " recording");
    seg(strstr(s->recording, "paused")                   ? C_WARN
        : strncmp(s->recording, "recording ", 10) == 0 ? C_OK
                                                         : C_BAD,
        "  %s",
        s->recording);
    line_end();
  }

  // The other end of the tether, as it last described itself.
  if (s->peer_configured || s->peer_contact_mono_ns) {
    double age = s->peer_contact_mono_ns
                     ? (double)(mono_ns() - s->peer_contact_mono_ns) / 1e9
                     : -1;
    seg(C_HEAD, " peer");
    seg(C_NONE, " %s  ", s->peer_addr);
    if (age < 0)
      seg(C_WARN, "never reached");
    else if (age > 3)
      seg(C_BAD, "last contact %.0fs ago", age);
    else
      seg(C_OK, "in contact");
    if (s->peer_error[0] && (age < 0 || age > 3))
      seg(C_BAD, "  %s", s->peer_error);
    line_end();
    for (uint32_t i = 0; i < s->peer_line_count; i++) {
      seg(C_DIM, "   | ");
      seg(age >= 0 && age <= 3 ? C_NONE : C_DIM, "%s", s->peer_lines[i]);
      line_end();
    }
  }
}

static void draw_log(const HsStatus *s, int rows) {
  if (rows <= 0)
    return;
  seg(C_HEAD, "LOG");
  seg(C_DIM, "  \"%s\", last %d of %llu lines", s->name,
      rows < (int)s->log_count ? rows : (int)s->log_count,
      (unsigned long long)s->log_total);
  line_end();
  int first = (int)s->log_count - rows;
  if (first < 0)
    first = 0;
  for (int i = first; i < (int)s->log_count; i++) {
    seg(C_NONE, "  %s", s->log[i]);
    line_end();
  }
}

// ---- The screen -----------------------------------------------------------

static HsStatus statuses[8];

static int count_lines(const HsStatus *s) {
  int n = 1;
  for (uint32_t i = 0; i < s->pipeline_count; i++) {
    const HsPipeline *p = &s->pipelines[i];
    n += 2 + (int)p->sink_count + (p->source.net ? 1 : 0);
    for (uint32_t k = 0; k < p->sink_count; k++)
      n += p->sinks[k].net ? 1 : 0;
  }
  if (s->recording[0])
    n++;
  if (s->peer_configured || s->peer_contact_mono_ns)
    n += 1 + (int)s->peer_line_count;
  return n;
}

// Draws one full frame. Returns the number of instances shown.
static int draw(const char *name, int term_rows, bool interactive) {
  char names[8][hs_name_len];
  int n;
  if (name) {
    snprintf(names[0], hs_name_len, "%s", name);
    n = 1;
  } else {
    n = hs_list(names, 8);
  }

  char clock_buf[32];
  time_t t = time(NULL);
  struct tm tm;
  localtime_r(&t, &tm);
  strftime(clock_buf, sizeof clock_buf, "%Y-%m-%d %H:%M:%S", &tm);
  seg(C_HEAD, "hellion top");
  seg(C_DIM, "  %s%s", clock_buf, interactive ? "  (q quits)" : "");
  line_end();
  line_end();

  int used = 2, shown = 0;
  const char *eeye_config = NULL;
  for (int i = 0; i < n; i++) {
    HsReadResult r = hs_read(names[i], &statuses[shown]);
    if (r != HS_READ_OK) {
      seg(C_HEAD, "HELLION \"%s\"  ", names[i]);
      seg(C_BAD, "%s",
          r == HS_READ_MISSING    ? "not running (no status file)"
          : r == HS_READ_MISMATCH ? "status written by a different hellion "
                                    "version"
                                  : "status unreadable");
      line_end();
      used++;
      continue;
    }
    draw_instance(&statuses[shown]);
    used += count_lines(&statuses[shown]);
    if (!eeye_config && statuses[shown].eeye_config[0])
      eeye_config = statuses[shown].eeye_config;
    shown++;
  }
  if (n == 0) {
    seg(C_BAD, "No hellion running here");
    seg(C_DIM, " (no status files in %s)", fbus_dir());
    line_end();
    used++;
  }
  line_end();
  used++;

  draw_bus();
  used += 2;
  line_end();
  if (eeye_config) {
    draw_eeye(eeye_config);
    line_end();
    used += 4;
  }

  // The logs share whatever room is left, so a problem in the second
  // instance is not hidden behind the first one's chatter.
  if (shown > 0) {
    int rows = interactive ? term_rows - used - 2 * shown : 20 * shown;
    int each = rows / shown;
    for (int i = 0; i < shown; i++)
      draw_log(&statuses[i], each > 3 ? each : 3);
  }
  return shown;
}

static struct termios saved_tio;
static bool tio_saved;
static volatile sig_atomic_t quit = 0;

static void on_quit(int sig) {
  (void)sig;
  quit = 1;
}

static void restore_terminal(void) {
  if (tio_saved)
    tcsetattr(STDIN_FILENO, TCSANOW, &saved_tio);
  // Cursor back on, main screen back.
  printf("\033[?25h\033[?1049l");
  fflush(stdout);
}

static void query_size(int *rows) {
  struct winsize ws;
  if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0 && ws.ws_col > 0) {
    term_cols = ws.ws_col;
    *rows = ws.ws_row;
  } else {
    term_cols = 200;
    *rows = 50;
  }
}

int hellion_top(const char *name, bool once) {
  const char *term = getenv("TERM");
  bool tty = isatty(STDOUT_FILENO);
  use_color = tty && term && strcmp(term, "dumb") != 0;
  int rows = 50;
  query_size(&rows);

  if (once || !tty) {
    // Bus fps needs two looks at the counters.
    memset(memo, 0, sizeof memo);
    char names[max_streams][fbus_max_stream_name];
    memset(names, 0, sizeof names);
    fbus_list(collect_name, names);
    for (int i = 0; i < max_streams && names[i][0]; i++) {
      FbusStatus st;
      if (fbus_status(names[i], &st) == FBUS_OPEN_OK)
        stream_fps(names[i], st.frames);
    }
    struct timespec ts = {0, 500000000L};
    nanosleep(&ts, NULL);
    term_cols = tty ? term_cols : 400;
    return draw(name, rows, false) > 0 ? 0 : 1;
  }

  struct sigaction sa;
  memset(&sa, 0, sizeof sa);
  sa.sa_handler = on_quit;
  sigaction(SIGINT, &sa, NULL);
  sigaction(SIGTERM, &sa, NULL);
  sigaction(SIGHUP, &sa, NULL);

  if (tcgetattr(STDIN_FILENO, &saved_tio) == 0) {
    tio_saved = true;
    struct termios raw = saved_tio;
    raw.c_lflag &= ~(tcflag_t)(ICANON | ECHO);
    raw.c_cc[VMIN] = 0;
    raw.c_cc[VTIME] = 0;
    tcsetattr(STDIN_FILENO, TCSANOW, &raw);
  }
  // The alternate screen, so quitting leaves the shell as it was.
  printf("\033[?1049h\033[?25l");

  while (!quit) {
    query_size(&rows);
    printf("\033[H"); // home; each line clears its own tail
    draw(name, rows, true);
    printf("\033[J"); // clear below the last line
    fflush(stdout);

    struct pollfd pfd = {.fd = STDIN_FILENO, .events = POLLIN, .revents = 0};
    if (poll(&pfd, 1, refresh_ms) > 0 && (pfd.revents & POLLIN)) {
      char c;
      if (read(STDIN_FILENO, &c, 1) == 1 && (c == 'q' || c == 'Q'))
        break;
    }
  }
  restore_terminal();
  return 0;
}
