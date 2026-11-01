#define _POSIX_C_SOURCE 200809L

#include "control.h"
#include "fbus.h"
#include "http.h"
#include "jpeg.h"
#include "hrecord.h"
#include "json_out.h"
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

// The page, embedded at build time from hellion/web/index.html.
extern const unsigned char page_html[];
extern const size_t page_html_len;

static const int max_connections = 32;
// Preview streams are one JPEG encode per viewer per frame, so they are
// capped separately, and so is their rate: a preview does not need 30fps,
// and the topside machine has a pilot to serve.
static const int max_previews = 4;
static const uint64_t preview_interval_ns = 66000000ull; // ~15fps
static const int preview_quality = 75;
static const int snapshot_quality = 92;
// How long POST /config waits for eeye's verdict. eeye reloads within
// ~200ms; this leaves room for a busy Pi.
static const int verdict_wait_ms = 3000;
static const int peer_timeout_ms = 3000;

// How often the side with "peer" set talks to the other side, and how
// long it waits for an answer.
static const int peer_period_ms = 1000;
static const int peer_exchange_timeout_ms = 1500;

struct Control {
  HellionConfig cfg;
  HttpServer *http;
  HRec *rec;
  int previews; // atomic (see compat.h)

  pthread_mutex_t lock; // the status snapshot and the peer view
  HsStatus status;      // the latest snapshot
  bool have_status;

  // The other end of the tether as it last described itself.
  char peer_addr[64];
  uint64_t peer_contact_ns;
  char peer_error[hs_detail_len];
  uint32_t peer_line_count;
  char peer_lines[hs_peer_lines][hs_detail_len];

  pthread_t peer_thread;
  bool peer_started;
  bool stop; // atomic (see compat.h)
};

static uint64_t mono_ns(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

// "src=NAME" from a query string; the default if absent. NULL if present
// but not a valid stream name.
static const char *query_src(const HttpRequest *req, const char *def,
                             char *buf, size_t cap) {
  const char *q = req->query;
  while (*q) {
    if (strncmp(q, "src=", 4) == 0) {
      const char *v = q + 4;
      size_t n = strcspn(v, "&");
      if (n == 0 || n >= cap)
        return NULL;
      memcpy(buf, v, n);
      buf[n] = '\0';
      return fbus_valid_name(buf) ? buf : NULL;
    }
    q += strcspn(q, "&");
    if (*q == '&')
      q++;
  }
  snprintf(buf, cap, "%s", def);
  return buf;
}

// ---- Preview and snapshots --------------------------------------------------

static void serve_stream(Control *ctl, HttpConn *conn, const char *src) {
  if (__atomic_add_fetch(&ctl->previews, 1, __ATOMIC_ACQ_REL) > max_previews) {
    __atomic_fetch_sub(&ctl->previews, 1, __ATOMIC_ACQ_REL);
    http_respond_json(conn, 503,
                      "{\"error\":\"too many previews open (%d)\"}\n",
                      max_previews);
    return;
  }
  static const char head[] =
      "HTTP/1.1 200 OK\r\n"
      "Content-Type: multipart/x-mixed-replace; boundary=frame\r\n"
      "Cache-Control: no-store\r\n"
      "Connection: close\r\n\r\n";
  JpegEnc *enc = jpeg_enc_create();
  FbusReader *r = NULL;
  uint8_t *frame = NULL;
  uint32_t w = 0, h = 0;
  uint64_t last_sent = 0;

  if (!enc || !http_write(conn, head, sizeof head - 1, 2000))
    goto done;

  // Until the viewer leaves: follow the stream through its writer
  // restarting or vanishing (the page shows the link as down meanwhile).
  while (!http_stopping(conn)) {
    if (!r) {
      r = fbus_reader_open(src, NULL);
      if (!r) {
        struct timespec ts = {0, 500000000L};
        nanosleep(&ts, NULL);
        continue;
      }
      fbus_reader_geometry(r, &w, &h);
      free(frame);
      frame = (uint8_t *)malloc((size_t)w * h * 2);
      if (!frame)
        goto done;
    }
    size_t y = (size_t)w * h, cbytes = (size_t)((w + 1) / 2) * h;
    uint8_t *planes[3] = {frame, frame + y, frame + y + cbytes};
    FbusReadResult res = fbus_reader_next(r, planes, 1000, NULL);
    if (res == FBUS_GONE) {
      fbus_reader_close(r);
      r = NULL;
      continue;
    }
    if (res != FBUS_FRAME)
      continue;
    uint64_t now = mono_ns();
    if (now - last_sent < preview_interval_ns)
      continue;
    last_sent = now;

    size_t jlen = 0;
    const uint8_t *jpg = jpeg_enc_i422(enc, (const uint8_t *const *)planes, w,
                                       h, preview_quality, &jlen);
    if (!jpg)
      continue;
    char part[128];
    int n = snprintf(part, sizeof part,
                     "--frame\r\nContent-Type: image/jpeg\r\n"
                     "Content-Length: %zu\r\n\r\n",
                     jlen);
    // A viewer that cannot take a frame in 2s is gone or hopeless.
    if (!http_write(conn, part, (size_t)n, 2000) ||
        !http_write(conn, jpg, jlen, 2000) ||
        !http_write(conn, "\r\n", 2, 2000))
      break;
  }

done:
  fbus_reader_close(r);
  free(frame);
  jpeg_enc_destroy(enc);
  __atomic_fetch_sub(&ctl->previews, 1, __ATOMIC_ACQ_REL);
}

static bool mkdirs(const char *path) {
  char buf[512];
  snprintf(buf, sizeof buf, "%s", path);
  for (char *p = buf + 1; *p; p++) {
    if (*p == '/') {
      *p = '\0';
      if (mkdir(buf, 0775) != 0 && errno != EEXIST)
        return false;
      *p = '/';
    }
  }
  return mkdir(buf, 0775) == 0 || errno == EEXIST;
}

static void serve_snapshot(Control *ctl, HttpConn *conn, const char *src) {
  FbusOpenResult why;
  FbusReader *r = fbus_reader_open(src, &why);
  if (!r) {
    http_respond_json(conn, 503, "{\"error\":\"stream %s: %s\"}\n", src,
                      fbus_open_result_str(why));
    return;
  }
  uint32_t w, h;
  fbus_reader_geometry(r, &w, &h);
  uint8_t *frame = (uint8_t *)malloc((size_t)w * h * 2);
  size_t y = (size_t)w * h, cbytes = (size_t)((w + 1) / 2) * h;
  uint8_t *planes[3] = {frame, frame + y, frame + y + cbytes};
  FbusReadResult res = frame ? fbus_reader_next(r, planes, 2000, NULL)
                             : FBUS_GONE;
  fbus_reader_close(r);
  if (res != FBUS_FRAME) {
    free(frame);
    http_respond_json(conn, 503, "{\"error\":\"no frame from %s\"}\n", src);
    return;
  }

  JpegEnc *enc = jpeg_enc_create();
  size_t jlen = 0;
  const uint8_t *jpg =
      enc ? jpeg_enc_i422(enc, (const uint8_t *const *)planes, w, h,
                          snapshot_quality, &jlen)
          : NULL;
  char name[96], path[512], esc[600];
  struct timespec ts;
  clock_gettime(CLOCK_REALTIME, &ts);
  struct tm tm;
  localtime_r(&ts.tv_sec, &tm);
  // Milliseconds in the name: two quick clicks must not overwrite.
  int n = (int)strftime(name, sizeof name, "snap-%Y%m%d-%H%M%S", &tm);
  snprintf(name + n, sizeof name - (size_t)n, "-%03ld-%s.jpg",
           ts.tv_nsec / 1000000, src);
  snprintf(path, sizeof path, "%s/%s", ctl->cfg.snapshot_dir, name);

  bool ok = false;
  if (jpg && mkdirs(ctl->cfg.snapshot_dir)) {
    FILE *f = fopen(path, "wbe");
    if (f) {
      ok = fwrite(jpg, 1, jlen, f) == jlen;
      ok = (fclose(f) == 0) && ok;
    }
  }
  int err = errno;
  jpeg_enc_destroy(enc);
  free(frame);
  if (!ok) {
    json_escape(strerror(err), esc, sizeof esc);
    http_respond_json(conn, 500, "{\"error\":\"could not save in %s: %s\"}\n",
                      ctl->cfg.snapshot_dir, esc);
    return;
  }
  printf("hellion: snapshot %s (%zu bytes)\n", path, jlen);
  json_escape(name, esc, sizeof esc);
  http_respond_json(conn, 200, "{\"file\":\"%s\",\"bytes\":%zu}\n", esc, jlen);
}

// ---- eeye's config and health -----------------------------------------------

static bool read_file(const char *path, char *buf, size_t cap, size_t *len,
                      struct stat *st) {
  FILE *f = fopen(path, "rbe");
  if (!f)
    return false;
  *len = fread(buf, 1, cap - 1, f);
  buf[*len] = '\0';
  if (st)
    fstat(fileno(f), st);
  fclose(f);
  return true;
}

static uint64_t mtime_ns(const char *path) {
  struct stat st;
  if (stat(path, &st) != 0)
    return 0;
  return (uint64_t)st.st_mtim.tv_sec * 1000000000ull +
         (uint64_t)st.st_mtim.tv_nsec;
}

// Replaces `path` the way eeye's watcher expects: a temp file in the same
// directory, renamed over, so eeye never reads half a file.
static bool replace_file(const char *path, const char *data, size_t len) {
  char tmp[300];
  snprintf(tmp, sizeof tmp, "%s.hellion.tmp", path);
  FILE *f = fopen(tmp, "wbe");
  bool ok = f && fwrite(data, 1, len, f) == len;
  if (f)
    ok = (fclose(f) == 0) && ok;
  if (!ok || rename(tmp, path) != 0) {
    int err = errno;
    unlink(tmp);
    errno = err;
    return false;
  }
  return true;
}

// Waits for eeye to rewrite its status file after a change: "true",
// "false", or "null" if it never did (eeye not running, or not watching
// this path).
static const char *wait_verdict(const char *status_path, uint64_t before) {
  for (int waited = 0; waited < verdict_wait_ms; waited += 50) {
    if (mtime_ns(status_path) != before) {
      char st[128];
      size_t n;
      if (read_file(status_path, st, sizeof st, &n, NULL))
        return strstr(st, "true") ? "true" : "false";
      return "null";
    }
    struct timespec ts = {0, 50000000L};
    nanosleep(&ts, NULL);
  }
  return "null";
}

// Writes eeye's config the way eeye's watcher expects (a temp file in the
// same directory, renamed over), then waits for eeye's verdict in
// "<config>.status". Its mtime is compared to the nanosecond: a fast
// reload within the same second would otherwise look like no reload.
static void write_eeye_config(Control *ctl, HttpConn *conn, const char *body,
                              size_t len) {
  const char *path = ctl->cfg.eeye_config;
  // Not a schema check -- eeye's own parser is the only one, and a second
  // would disagree with it sooner or later -- just enough to refuse
  // writing something that is not even a JSON object.
  size_t i = 0;
  while (i < len && (body[i] == ' ' || body[i] == '\n' || body[i] == '\r' ||
                     body[i] == '\t'))
    i++;
  if (!body || i == len || body[i] != '{') {
    http_respond_json(conn, 400, "{\"error\":\"expected a JSON object\"}\n");
    return;
  }

  char status_path[300];
  snprintf(status_path, sizeof status_path, "%s.status", path);

  // What is there now, to put back if eeye refuses the new one.
  static __thread char previous[16384];
  size_t prev_len = 0;
  bool had_previous =
      read_file(path, previous, sizeof previous, &prev_len, NULL);

  uint64_t before = mtime_ns(status_path);
  if (!replace_file(path, body, len)) {
    char esc[256];
    json_escape(strerror(errno), esc, sizeof esc);
    http_respond_json(conn, 500, "{\"error\":\"could not write %s: %s\"}\n",
                      path, esc);
    return;
  }

  const char *verdict = wait_verdict(status_path, before);
  if (strcmp(verdict, "false") == 0 && had_previous) {
    // eeye keeps running the previous config when it rejects one, but the
    // rejected file would stay on disk -- and the next time eeye starts
    // (after a crash, mid-dive) it would refuse to start at all. Put back
    // what eeye is actually running.
    uint64_t again = mtime_ns(status_path);
    bool restored = replace_file(path, previous, prev_len);
    if (restored)
      wait_verdict(status_path, again); // eeye re-reads it; let it settle
    printf("hellion: eeye rejected a new config (its log says why); %s\n",
           restored ? "restored the previous one" : "COULD NOT restore the "
                                                    "previous one");
    http_respond_json(conn, 200,
                      "{\"written\":false,\"eeye_accepted\":false,"
                      "\"restored\":%s}\n",
                      restored ? "true" : "false");
    return;
  }
  http_respond_json(conn, 200, "{\"written\":true,\"eeye_accepted\":%s}\n",
                    verdict);
}

// eeye's health file with "age_s" added: a file that stopped being
// rewritten means eeye is wedged or gone, which its contents cannot say.
static bool eeye_health(Control *ctl, char *out, size_t cap) {
  char path[300], buf[2048];
  size_t n;
  struct stat st;
  snprintf(path, sizeof path, "%s.health", ctl->cfg.eeye_config);
  if (!read_file(path, buf, sizeof buf, &n, &st))
    return false;
  while (n > 0 && (buf[n - 1] == '\n' || buf[n - 1] == ' '))
    n--;
  if (n < 2 || buf[n - 1] != '}')
    return false;
  buf[n - 1] = '\0';
  struct timespec now;
  clock_gettime(CLOCK_REALTIME, &now);
  double age = (double)(now.tv_sec - st.st_mtim.tv_sec) +
               (double)(now.tv_nsec - st.st_mtim.tv_nsec) / 1e9;
  snprintf(out, cap, "%s,\"age_s\":%.1f}", buf, age < 0 ? 0 : age);
  return true;
}

static void proxy_to_peer(Control *ctl, HttpConn *conn, const char *method,
                          const char *path, const char *body, size_t len) {
  static __thread char resp[http_max_body];
  size_t rlen = 0;
  char err[200];
  int status = http_call(ctl->cfg.peer, method, path, body, len, resp,
                         sizeof resp, &rlen, peer_timeout_ms, err, sizeof err);
  if (status < 0) {
    char esc[300];
    json_escape(err, esc, sizeof esc);
    http_respond_json(conn, 502, "{\"error\":\"peer %s: %s\"}\n",
                      ctl->cfg.peer, esc);
    return;
  }
  http_respond(conn, status, "application/json", resp, rlen);
}

static void serve_config(Control *ctl, HttpConn *conn, bool post,
                         const char *body, size_t len) {
  if (ctl->cfg.eeye_config[0]) {
    if (post) {
      write_eeye_config(ctl, conn, body, len);
      return;
    }
    static __thread char buf[16384];
    size_t n = 0;
    if (!read_file(ctl->cfg.eeye_config, buf, sizeof buf, &n, NULL))
      http_respond(conn, 200, "application/json", "{}\n", 3); // eeye's defaults
    else
      http_respond(conn, 200, "application/json", buf, n);
    return;
  }
  if (ctl->cfg.peer[0]) {
    proxy_to_peer(ctl, conn, post ? "POST" : "GET", "/config", body, len);
    return;
  }
  http_respond_json(conn, 503,
                    "{\"error\":\"eeye does not run here and no peer is "
                    "configured\"}\n");
}

static void serve_health(Control *ctl, HttpConn *conn, const char *src) {
  // Link: is the preview stream alive, and how old is its newest frame.
  FbusStatus st;
  FbusOpenResult r = fbus_status(src, &st);
  bool connected = r == FBUS_OPEN_OK && st.alive;
  bool have_frame = r == FBUS_OPEN_OK && st.frames > 0;
  char age[32] = "null";
  if (have_frame)
    snprintf(age, sizeof age, "%.2f", (double)st.last_frame_age_ns / 1e9);

  // eeye: here, or from the peer.
  char drone[2600];
  char err[200];
  if (ctl->cfg.eeye_config[0]) {
    if (!eeye_health(ctl, drone, sizeof drone))
      snprintf(drone, sizeof drone,
               "{\"error\":\"no health file -- is eeye running?\"}");
  } else if (ctl->cfg.peer[0]) {
    size_t rlen = 0;
    int status = http_call(ctl->cfg.peer, "GET", "/api/drone", NULL, 0, drone,
                           sizeof drone, &rlen, peer_timeout_ms, err,
                           sizeof err);
    if (status != 200 || rlen == 0 || drone[0] != '{') {
      char esc[300];
      json_escape(status < 0 ? err : "peer has no eeye health", esc,
                  sizeof esc);
      snprintf(drone, sizeof drone, "{\"error\":\"%s\"}", esc);
    }
  } else {
    snprintf(drone, sizeof drone, "{\"error\":\"eeye does not run here\"}");
  }

  char rec[1200];
  hrec_status_json(ctl->rec, rec, sizeof rec);
  http_respond_json(conn, 200,
                    "{\"link\":{\"stream\":\"%s\",\"connected\":%s,"
                    "\"have_frame\":%s,\"age_s\":%s,\"frames\":%llu},"
                    "\"recorder\":%s,\"drone\":%s}\n",
                    src, connected ? "true" : "false",
                    have_frame ? "true" : "false", age,
                    (unsigned long long)(r == FBUS_OPEN_OK ? st.frames : 0),
                    rec, drone);
}

typedef struct {
  char *out;
  size_t cap, len;
  int n;
} StreamList;

static void add_stream(const char *name, void *ctx) {
  StreamList *l = (StreamList *)ctx;
  int w = snprintf(l->out + l->len, l->cap - l->len, "%s\"%s\"",
                   l->n ? "," : "", name);
  if (w > 0 && (size_t)w < l->cap - l->len) {
    l->len += (size_t)w;
    l->n++;
  }
}

static void serve_status(Control *ctl, HttpConn *conn) {
  static __thread HsStatus s;
  pthread_mutex_lock(&ctl->lock);
  bool have = ctl->have_status;
  if (have)
    s = ctl->status;
  pthread_mutex_unlock(&ctl->lock);
  if (!have) {
    http_respond_json(conn, 503, "{\"error\":\"starting\"}\n");
    return;
  }

  static __thread char out[16384];
  size_t n = 0;
  n += (size_t)snprintf(out + n, sizeof out - n,
                        "{\"name\":\"%s\",\"role\":\"%s\",\"pid\":%d,"
                        "\"pipelines\":[",
                        s.name, s.role, s.pid);
  for (uint32_t i = 0; i < s.pipeline_count && n < sizeof out - 1024; i++) {
    const HsPipeline *p = &s.pipelines[i];
    for (uint32_t e = 0; e <= p->sink_count && n < sizeof out - 512; e++) {
      const HsEndpoint *ep = e == 0 ? &p->source : &p->sinks[e - 1];
      char spec[2 * hs_spec_len], detail[2 * hs_detail_len];
      json_escape(ep->spec, spec, sizeof spec);
      json_escape(ep->detail, detail, sizeof detail);
      if (e == 0)
        n += (size_t)snprintf(out + n, sizeof out - n,
                              "%s{\"name\":\"%s\",\"width\":%u,"
                              "\"height\":%u,\"endpoints\":[",
                              i ? "," : "", p->name, p->width, p->height);
      n += (size_t)snprintf(
          out + n, sizeof out - n,
          "%s{\"role\":\"%s\",\"spec\":\"%s\",\"state\":\"%s\","
          "\"detail\":\"%s\",\"frames\":%llu,\"dropped\":%llu,"
          "\"fps\":%.1f",
          e ? "," : "", e ? "sink" : "source", spec, ep->state, detail,
          (unsigned long long)ep->frames, (unsigned long long)ep->dropped,
          ep->fps);
      if (ep->net)
        n += (size_t)snprintf(out + n, sizeof out - n,
                              ",\"kbps\":%.0f,\"loss_pct\":%.2f,"
                              "\"packets\":%llu,\"lost\":%llu",
                              ep->kbps, ep->loss_pct,
                              (unsigned long long)ep->packets,
                              (unsigned long long)ep->lost);
      n += (size_t)snprintf(out + n, sizeof out - n, "}");
    }
    n += (size_t)snprintf(out + n, sizeof out - n, "]}");
  }
  n += (size_t)snprintf(out + n, sizeof out - n, "]}\n");
  http_respond(conn, 200, "application/json", out, n < sizeof out ? n : 0);
}

// ---- Recording ----------------------------------------------------------------

static void serve_record(Control *ctl, HttpConn *conn, const char *body) {
  // The page sends {"action":"start"} or {"action":"stop"}.
  bool start = body && strstr(body, "\"start\"");
  bool stop = body && strstr(body, "\"stop\"");
  if (start == stop) {
    http_respond_json(conn, 400,
                      "{\"ok\":false,\"detail\":\"action must be start or "
                      "stop\"}\n");
    return;
  }
  char detail[256], esc[512];
  bool ok = start ? hrec_start(ctl->rec, detail, sizeof detail)
                  : hrec_stop(ctl->rec, detail, sizeof detail);
  json_escape(detail, esc, sizeof esc);
  http_respond_json(conn, ok ? 200 : 409, "{\"ok\":%s,\"detail\":\"%s\"}\n",
                    ok ? "true" : "false", esc);
}

// ---- The other end of the tether ------------------------------------------
//
// Once a second the side with "peer" set POSTs a few lines describing
// itself to /api/peer on the other side, and gets the other side's lines
// back. Both keep what they received, so `hellion top` on either machine
// shows the far end -- on the ROV that includes what the topside actually
// receives, which the ROV cannot otherwise know. Plain text lines, not
// JSON: they are for people, and nothing has to parse them.

// This side in a few lines, from the latest status snapshot.
static size_t own_summary(Control *ctl, char *out, size_t cap) {
  static __thread HsStatus s;
  pthread_mutex_lock(&ctl->lock);
  bool have = ctl->have_status;
  if (have)
    s = ctl->status;
  pthread_mutex_unlock(&ctl->lock);
  if (!have)
    return (size_t)snprintf(out, cap, "%s (%s): starting\n", ctl->cfg.name,
                            ctl->cfg.role);

  size_t n = (size_t)snprintf(out, cap, "%s (%s), pid %d\n", s.name, s.role,
                              s.pid);
  for (uint32_t i = 0; i < s.pipeline_count && n < cap; i++) {
    const HsPipeline *p = &s.pipelines[i];
    for (uint32_t e = 0; e <= p->sink_count && n < cap; e++) {
      const HsEndpoint *ep = e == 0 ? &p->source : &p->sinks[e - 1];
      char link[64] = "";
      if (ep->net)
        snprintf(link, sizeof link, ", %.0f kbit/s %.1f%% loss", ep->kbps,
                 ep->loss_pct);
      n += (size_t)snprintf(out + n, cap - n, "%s%s %s %s %.0f fps%s\n",
                            e == 0 ? p->name : "  ", e == 0 ? ":" : "->",
                            ep->spec, ep->state, ep->fps, link);
    }
  }
  if (s.recording[0] && n < cap)
    n += (size_t)snprintf(out + n, cap - n, "%s\n", s.recording);
  return n < cap ? n : cap - 1;
}

// Stores what the other side said about itself. Untrusted text: only
// printable ASCII is kept, a line at most hs_detail_len - 1 long, at most
// hs_peer_lines of them.
static void store_peer_view(Control *ctl, const char *addr, const char *text,
                            size_t len) {
  char lines[hs_peer_lines][hs_detail_len];
  uint32_t count = 0;
  size_t col = 0;
  memset(lines, 0, sizeof lines);
  for (size_t i = 0; i < len && count < hs_peer_lines; i++) {
    char ch = text[i];
    if (ch == '\n') {
      count++;
      col = 0;
    } else if (ch >= 0x20 && ch < 0x7f && col < hs_detail_len - 1) {
      lines[count][col++] = ch;
    }
  }
  if (col && count < hs_peer_lines)
    count++;

  pthread_mutex_lock(&ctl->lock);
  snprintf(ctl->peer_addr, sizeof ctl->peer_addr, "%.63s", addr);
  ctl->peer_contact_ns = mono_ns();
  ctl->peer_error[0] = '\0';
  memcpy(ctl->peer_lines, lines, sizeof lines);
  ctl->peer_line_count = count;
  pthread_mutex_unlock(&ctl->lock);
}

// POST /api/peer: the caller's lines in, ours out.
static void serve_peer(Control *ctl, HttpConn *conn, const char *body,
                       size_t len) {
  if (body)
    store_peer_view(ctl, http_client_addr(conn), body, len);
  char out[4096];
  size_t n = own_summary(ctl, out, sizeof out);
  http_respond(conn, 200, "text/plain", out, n);
}

static void *peer_loop(void *arg) {
  Control *ctl = (Control *)arg;
  char mine[4096];
  static __thread char theirs[8192];
  while (!__atomic_load_n(&ctl->stop, __ATOMIC_ACQUIRE)) {
    size_t n = own_summary(ctl, mine, sizeof mine);
    size_t got = 0;
    char err[200];
    int status = http_call(ctl->cfg.peer, "POST", "/api/peer", mine, n,
                           theirs, sizeof theirs, &got,
                           peer_exchange_timeout_ms, err, sizeof err);
    if (status == 200) {
      store_peer_view(ctl, ctl->cfg.peer, theirs, got);
    } else {
      pthread_mutex_lock(&ctl->lock);
      if (status > 0)
        snprintf(ctl->peer_error, sizeof ctl->peer_error,
                 "%.60s answered %d (an older hellion?)", ctl->cfg.peer,
                 status);
      else
        snprintf(ctl->peer_error, sizeof ctl->peer_error, "%.*s",
                 hs_detail_len - 1, err);
      pthread_mutex_unlock(&ctl->lock);
    }
    for (int waited = 0; waited < peer_period_ms &&
                         !__atomic_load_n(&ctl->stop, __ATOMIC_ACQUIRE);
         waited += 50) {
      struct timespec ts = {0, 50000000L};
      nanosleep(&ts, NULL);
    }
  }
  return NULL;
}

void control_fill(Control *c, HsStatus *s) {
  if (!c)
    return;
  pthread_mutex_lock(&c->lock);
  s->peer_configured = c->cfg.peer[0] != '\0';
  snprintf(s->peer_addr, sizeof s->peer_addr, "%.63s",
           c->peer_addr[0] ? c->peer_addr : c->cfg.peer);
  s->peer_contact_mono_ns = c->peer_contact_ns;
  snprintf(s->peer_error, sizeof s->peer_error, "%s", c->peer_error);
  s->peer_line_count = c->peer_line_count;
  memcpy(s->peer_lines, c->peer_lines, sizeof s->peer_lines);
  pthread_mutex_unlock(&c->lock);
}

// ---- Dispatch -----------------------------------------------------------------

static void handle(HttpConn *conn, const HttpRequest *req, const char *body,
                   size_t len, void *arg) {
  Control *ctl = (Control *)arg;
  bool get = strcmp(req->method, "GET") == 0;
  bool post = strcmp(req->method, "POST") == 0;
  char src_buf[fbus_max_stream_name];
  const char *src = query_src(req, ctl->cfg.preview, src_buf, sizeof src_buf);

  if (get && strcmp(req->path, "/") == 0) {
    http_respond(conn, 200, "text/html", page_html, page_html_len);
  } else if (get && strcmp(req->path, "/stream") == 0) {
    if (!src)
      http_respond_json(conn, 400, "{\"error\":\"bad src\"}\n");
    else
      serve_stream(ctl, conn, src);
  } else if (post && strcmp(req->path, "/snapshot") == 0) {
    if (!src)
      http_respond_json(conn, 400, "{\"error\":\"bad src\"}\n");
    else
      serve_snapshot(ctl, conn, src);
  } else if ((get || post) && strcmp(req->path, "/config") == 0) {
    serve_config(ctl, conn, post, body, len);
  } else if (get && strcmp(req->path, "/health") == 0) {
    if (!src)
      http_respond_json(conn, 400, "{\"error\":\"bad src\"}\n");
    else
      serve_health(ctl, conn, src);
  } else if (post && strcmp(req->path, "/record") == 0) {
    serve_record(ctl, conn, body);
  } else if (post && strcmp(req->path, "/api/peer") == 0) {
    serve_peer(ctl, conn, body, len);
  } else if (get && strcmp(req->path, "/api/streams") == 0) {
    char list[4096];
    StreamList l = {list, sizeof list, 0, 0};
    fbus_list(add_stream, &l);
    list[l.len] = '\0';
    http_respond_json(conn, 200, "{\"streams\":[%s]}\n", list);
  } else if (get && strcmp(req->path, "/api/drone") == 0) {
    char drone[2600];
    if (ctl->cfg.eeye_config[0] && eeye_health(ctl, drone, sizeof drone))
      http_respond(conn, 200, "application/json", drone, strlen(drone));
    else
      http_respond_json(conn, 404, "{\"error\":\"no eeye health here\"}\n");
  } else if (get && strcmp(req->path, "/api/status") == 0) {
    serve_status(ctl, conn);
  } else if (!get && !post) {
    http_respond_json(conn, 405, "{\"error\":\"method not allowed\"}\n");
  } else {
    http_respond_json(conn, 404, "{\"error\":\"no such page\"}\n");
  }
}

Control *control_start(const HellionConfig *cfg, HRec *rec) {
  if (!cfg->control[0])
    return NULL;
  Control *ctl = (Control *)calloc(1, sizeof *ctl);
  if (!ctl)
    return NULL;
  ctl->cfg = *cfg;
  ctl->rec = rec;
  pthread_mutex_init(&ctl->lock, NULL);
  char err[200];
  ctl->http = http_start(cfg->control, max_connections, handle, ctl, err,
                         sizeof err);
  if (!ctl->http) {
    printf("hellion: web page and API disabled: %s\n", err);
    pthread_mutex_destroy(&ctl->lock);
    free(ctl);
    return NULL;
  }
  printf("hellion: web page and API on port %d%s%s%s\n", http_port(ctl->http),
         cfg->peer[0] ? "; peer " : "", cfg->peer,
         cfg->eeye_config[0] ? "; eeye config here" : "");
  if (cfg->peer[0])
    ctl->peer_started =
        pthread_create(&ctl->peer_thread, NULL, peer_loop, ctl) == 0;
  return ctl;
}

void control_set_status(Control *c, const HsStatus *s) {
  if (!c)
    return;
  pthread_mutex_lock(&c->lock);
  c->status = *s;
  c->have_status = true;
  pthread_mutex_unlock(&c->lock);
}

void control_stop(Control *c) {
  if (!c)
    return;
  __atomic_store_n(&c->stop, true, __ATOMIC_RELEASE);
  if (c->peer_started)
    pthread_join(c->peer_thread, NULL);
  http_stop(c->http);
  pthread_mutex_destroy(&c->lock);
  free(c);
}
