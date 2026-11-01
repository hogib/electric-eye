// POSIX is enough: the accepted socket gets close-on-exec and O_NONBLOCK
// through fcntl rather than accept4() (a GNU extension).
#define _POSIX_C_SOURCE 200809L

#include "http.h"
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

// How long a client gets to send its whole request.
static const int request_timeout_ms = 5000;
static const int response_timeout_ms = 5000;

// ---- Parsing ----------------------------------------------------------------

static bool is_tchar(char c) {
  // RFC 9110 token characters.
  return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
         (c >= '0' && c <= '9') || (c && strchr("!#$%&'*+-.^_`|~", c));
}

static bool ieq(const char *a, size_t alen, const char *b) {
  size_t blen = strlen(b);
  if (alen != blen)
    return false;
  for (size_t i = 0; i < alen; i++) {
    char x = a[i], y = b[i];
    if (x >= 'A' && x <= 'Z')
      x = (char)(x - 'A' + 'a');
    if (y >= 'A' && y <= 'Z')
      y = (char)(y - 'A' + 'a');
    if (x != y)
      return false;
  }
  return true;
}

// Finds the end of the head (just past "\r\n\r\n"), checking on the way
// that every line ends in CRLF and nothing is a NUL. Returns 0 while the
// head is incomplete, (size_t)-1 if it is malformed.
static size_t head_end(const char *buf, size_t len) {
  for (size_t i = 0; i < len; i++) {
    char c = buf[i];
    if (c == '\0')
      return (size_t)-1;
    if (c == '\n' && (i == 0 || buf[i - 1] != '\r'))
      return (size_t)-1; // bare LF
    if (c == '\r' && i + 1 < len && buf[i + 1] != '\n')
      return (size_t)-1; // bare CR
    if (c == '\n' && i >= 3 && buf[i - 3] == '\r' && buf[i - 2] == '\n')
      return i + 1;
  }
  return 0;
}

// Header lines from `p` up to the blank line ending at `end`. Fills the
// Content-Length fields. Returns false if any line is malformed or
// unsupported.
static bool parse_headers(const char *p, const char *end, bool *has_cl,
                          size_t *cl) {
  *has_cl = false;
  *cl = 0;
  while (p < end) {
    const char *eol = p;
    while (eol < end && *eol != '\r')
      eol++;
    if (eol >= end)
      return false;
    if (eol == p)
      return eol + 2 == end; // the blank line: must be the last thing

    // Obsolete line folding: refused, not unfolded.
    if (*p == ' ' || *p == '\t')
      return false;
    const char *colon = p;
    while (colon < eol && is_tchar(*colon))
      colon++;
    if (colon == p || colon >= eol || *colon != ':')
      return false;
    size_t name_len = (size_t)(colon - p);

    const char *v = colon + 1;
    while (v < eol && (*v == ' ' || *v == '\t'))
      v++;
    const char *vend = eol;
    while (vend > v && (vend[-1] == ' ' || vend[-1] == '\t'))
      vend--;
    for (const char *q = v; q < vend; q++)
      if ((unsigned char)*q < 0x20 && *q != '\t')
        return false;

    if (ieq(p, name_len, "transfer-encoding"))
      return false; // chunked bodies are not supported, so say so
    if (ieq(p, name_len, "content-length")) {
      size_t digits = (size_t)(vend - v);
      if (digits == 0 || digits > 9)
        return false;
      size_t value = 0;
      for (const char *q = v; q < vend; q++) {
        if (*q < '0' || *q > '9')
          return false;
        value = value * 10 + (size_t)(*q - '0');
      }
      if (*has_cl && value != *cl)
        return false; // two different lengths: which body is it?
      *has_cl = true;
      *cl = value;
    }
    p = eol + 2;
  }
  return false;
}

HttpParse http_parse_request(const char *buf, size_t len, HttpRequest *out) {
  memset(out, 0, sizeof *out);
  size_t scan = len < http_max_head ? len : http_max_head;
  size_t end = head_end(buf, scan);
  if (end == (size_t)-1)
    return HTTP_PARSE_BAD;
  if (end == 0)
    return len >= http_max_head ? HTTP_PARSE_TOO_LARGE
                                : HTTP_PARSE_INCOMPLETE;

  // Request line: METHOD SP target SP HTTP/1.x CRLF
  const char *p = buf;
  const char *lim = buf + end;
  size_t m = 0;
  while (p < lim && *p >= 'A' && *p <= 'Z' && m < sizeof out->method - 1)
    out->method[m++] = *p++;
  if (m == 0 || p >= lim || *p != ' ')
    return HTTP_PARSE_BAD;
  p++;

  if (p >= lim || *p != '/')
    return HTTP_PARSE_BAD;
  const char *t = p;
  while (p < lim && *p > 0x20 && *p < 0x7f)
    p++;
  size_t tlen = (size_t)(p - t);
  if (tlen >= http_max_target || p >= lim || *p != ' ')
    return HTTP_PARSE_BAD;
  const char *qmark = memchr(t, '?', tlen);
  size_t plen = qmark ? (size_t)(qmark - t) : tlen;
  memcpy(out->path, t, plen);
  out->path[plen] = '\0';
  if (qmark) {
    size_t qlen = tlen - plen - 1;
    memcpy(out->query, qmark + 1, qlen);
    out->query[qlen] = '\0';
  }
  p++;

  if ((size_t)(lim - p) < 10 ||
      (memcmp(p, "HTTP/1.0\r\n", 10) != 0 && memcmp(p, "HTTP/1.1\r\n", 10) != 0))
    return HTTP_PARSE_BAD;
  p += 10;

  if (!parse_headers(p, lim, &out->has_content_length, &out->content_length))
    return HTTP_PARSE_BAD;
  out->head_len = end;
  return HTTP_PARSE_OK;
}

HttpParse http_parse_response(const char *buf, size_t len,
                              HttpResponseHead *out) {
  memset(out, 0, sizeof *out);
  size_t scan = len < http_max_head ? len : http_max_head;
  size_t end = head_end(buf, scan);
  if (end == (size_t)-1)
    return HTTP_PARSE_BAD;
  if (end == 0)
    return len >= http_max_head ? HTTP_PARSE_TOO_LARGE
                                : HTTP_PARSE_INCOMPLETE;

  // Status line: HTTP/1.x SP 3DIGIT [SP reason] CRLF
  const char *p = buf;
  const char *lim = buf + end;
  if ((size_t)(lim - p) < 14 ||
      (memcmp(p, "HTTP/1.0 ", 9) != 0 && memcmp(p, "HTTP/1.1 ", 9) != 0))
    return HTTP_PARSE_BAD;
  p += 9;
  int status = 0;
  for (int i = 0; i < 3; i++, p++) {
    if (*p < '0' || *p > '9')
      return HTTP_PARSE_BAD;
    status = status * 10 + (*p - '0');
  }
  if (status < 100)
    return HTTP_PARSE_BAD;
  while (p < lim && *p != '\r') {
    if ((unsigned char)*p < 0x20 && *p != '\t')
      return HTTP_PARSE_BAD;
    p++;
  }
  if (p + 1 >= lim)
    return HTTP_PARSE_BAD;
  p += 2;

  if (!parse_headers(p, lim, &out->has_content_length, &out->content_length))
    return HTTP_PARSE_BAD;
  out->status = status;
  out->head_len = end;
  return HTTP_PARSE_OK;
}

// ---- Sockets ----------------------------------------------------------------

static int64_t now_ms(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static int wait_fd(int fd, short events, int64_t deadline) {
  for (;;) {
    int64_t left = deadline - now_ms();
    if (left <= 0)
      return 0;
    struct pollfd pfd = {.fd = fd, .events = events, .revents = 0};
    int r = poll(&pfd, 1, (int)left);
    if (r < 0 && errno == EINTR)
      continue;
    return r;
  }
}

static bool send_all(int fd, const void *data, size_t len, int timeout_ms) {
  const char *p = (const char *)data;
  int64_t deadline = now_ms() + timeout_ms;
  while (len > 0) {
    ssize_t n = send(fd, p, len, MSG_NOSIGNAL | MSG_DONTWAIT);
    if (n > 0) {
      p += n;
      len -= (size_t)n;
      continue;
    }
    if (n < 0 && errno == EINTR)
      continue;
    if (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK)
      return false;
    if (wait_fd(fd, POLLOUT, deadline) <= 0)
      return false;
  }
  return true;
}

// "host:port" / "[v6%iface]:port" / "port".
static bool split_host_port(const char *spec, char *host, size_t host_cap,
                            char *port, size_t port_cap) {
  host[0] = '\0';
  if (spec[0] == '[') {
    const char *close = strchr(spec, ']');
    if (!close || close[1] != ':')
      return false;
    snprintf(host, host_cap, "%.*s", (int)(close - spec - 1), spec + 1);
    snprintf(port, port_cap, "%s", close + 2);
  } else {
    const char *colon = strrchr(spec, ':');
    if (colon) {
      snprintf(host, host_cap, "%.*s", (int)(colon - spec), spec);
      snprintf(port, port_cap, "%s", colon + 1);
    } else {
      snprintf(port, port_cap, "%s", spec);
    }
  }
  if (!port[0])
    return false;
  for (const char *c = port; *c; c++)
    if (*c < '0' || *c > '9')
      return false;
  return atoi(port) <= 65535;
}

// ---- Server -----------------------------------------------------------------

struct HttpServer {
  int fd;
  int port;
  int max_conns;
  HttpHandler handler;
  void *ctx;
  pthread_t thread;
  bool stop;   // atomic (see compat.h)
  int active;  // connection threads running; atomic
};

struct HttpConn {
  HttpServer *server;
  int fd;
  char addr[64];
  bool responded;
};

static const char *reason(int status) {
  switch (status) {
  case 200: return "OK";
  case 204: return "No Content";
  case 400: return "Bad Request";
  case 404: return "Not Found";
  case 405: return "Method Not Allowed";
  case 408: return "Request Timeout";
  case 409: return "Conflict";
  case 413: return "Content Too Large";
  case 431: return "Request Header Fields Too Large";
  case 500: return "Internal Server Error";
  case 502: return "Bad Gateway";
  case 503: return "Service Unavailable";
  case 504: return "Gateway Timeout";
  }
  return "Status";
}

void http_respond(HttpConn *c, int status, const char *content_type,
                  const void *body, size_t len) {
  if (c->responded)
    return;
  c->responded = true;
  char head[512];
  bool text = strncmp(content_type, "text/", 5) == 0 &&
              !strstr(content_type, "charset");
  int n = snprintf(head, sizeof head,
                   "HTTP/1.1 %d %s\r\n"
                   "Content-Type: %s%s\r\n"
                   "Content-Length: %zu\r\n"
                   "Cache-Control: no-store\r\n"
                   "Connection: close\r\n\r\n",
                   status, reason(status), content_type,
                   text ? "; charset=utf-8" : "", len);
  if (send_all(c->fd, head, (size_t)n, response_timeout_ms) && len)
    send_all(c->fd, body, len, response_timeout_ms);
}

void http_respond_json(HttpConn *c, int status, const char *fmt, ...) {
  char body[8192];
  va_list ap;
  va_start(ap, fmt);
  int n = vsnprintf(body, sizeof body, fmt, ap);
  va_end(ap);
  if (n < 0)
    n = 0;
  if ((size_t)n >= sizeof body)
    n = (int)sizeof body - 1;
  http_respond(c, status, "application/json", body, (size_t)n);
}

bool http_write(HttpConn *c, const void *data, size_t len, int timeout_ms) {
  c->responded = true;
  return send_all(c->fd, data, len, timeout_ms);
}

bool http_stopping(const HttpConn *c) {
  return __atomic_load_n(&c->server->stop, __ATOMIC_ACQUIRE);
}

bool http_peer_gone(HttpConn *c) {
  struct pollfd pfd = {.fd = c->fd, .events = POLLIN};
  if (poll(&pfd, 1, 0) <= 0)
    return false; // nothing to read: still there, as far as anyone can tell
  if (pfd.revents & (POLLERR | POLLHUP | POLLNVAL))
    return true;
  // Readable: either the orderly close (recv returns 0) or bytes the
  // client sent anyway, which a GET with its body read never has.
  char b;
  ssize_t n = recv(c->fd, &b, 1, MSG_PEEK | MSG_DONTWAIT);
  return n == 0 || (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK &&
                    errno != EINTR);
}

const char *http_client_addr(const HttpConn *c) { return c->addr; }

static void plain_error(HttpConn *c, int status, const char *msg) {
  http_respond(c, status, "text/plain", msg, strlen(msg));
}

static void serve_connection(HttpConn *c) {
  static __thread char buf[http_max_head + http_max_body + 1];
  size_t have = 0;
  int64_t deadline = now_ms() + request_timeout_ms;
  HttpRequest req;
  HttpParse pr = HTTP_PARSE_INCOMPLETE;

  while (pr == HTTP_PARSE_INCOMPLETE) {
    if (wait_fd(c->fd, POLLIN, deadline) <= 0) {
      if (have)
        plain_error(c, 408, "request timed out\n");
      return;
    }
    ssize_t n = recv(c->fd, buf + have, http_max_head - have, MSG_DONTWAIT);
    if (n < 0 && (errno == EAGAIN || errno == EINTR))
      continue;
    if (n <= 0)
      return;
    have += (size_t)n;
    pr = http_parse_request(buf, have, &req);
    if (pr == HTTP_PARSE_INCOMPLETE && have == http_max_head)
      pr = HTTP_PARSE_TOO_LARGE;
  }
  if (pr == HTTP_PARSE_TOO_LARGE) {
    plain_error(c, 431, "request head too large\n");
    return;
  }
  if (pr != HTTP_PARSE_OK) {
    plain_error(c, 400, "malformed request\n");
    return;
  }
  if (req.content_length > http_max_body) {
    plain_error(c, 413, "request body too large\n");
    return;
  }

  // The body: whatever came with the head, then the rest.
  size_t body_have = have - req.head_len;
  if (body_have > req.content_length)
    body_have = req.content_length; // ignore anything past it
  char *body = buf + req.head_len;
  while (body_have < req.content_length) {
    if (wait_fd(c->fd, POLLIN, deadline) <= 0) {
      plain_error(c, 408, "request body timed out\n");
      return;
    }
    ssize_t n = recv(c->fd, body + body_have,
                     req.content_length - body_have, MSG_DONTWAIT);
    if (n < 0 && (errno == EAGAIN || errno == EINTR))
      continue;
    if (n <= 0)
      return;
    body_have += (size_t)n;
  }
  body[body_have] = '\0';

  c->server->handler(c, &req, req.content_length ? body : NULL,
                     req.content_length, c->server->ctx);
  if (!c->responded)
    plain_error(c, 500, "no response\n");
}

static void *conn_thread(void *arg) {
  HttpConn *c = (HttpConn *)arg;
  serve_connection(c);
  shutdown(c->fd, SHUT_WR);
  close(c->fd);
  __atomic_fetch_sub(&c->server->active, 1, __ATOMIC_ACQ_REL);
  free(c);
  return NULL;
}

static void *accept_loop(void *arg) {
  HttpServer *s = (HttpServer *)arg;
  while (!__atomic_load_n(&s->stop, __ATOMIC_ACQUIRE)) {
    struct pollfd pfd = {.fd = s->fd, .events = POLLIN, .revents = 0};
    if (poll(&pfd, 1, 250) <= 0)
      continue;
    struct sockaddr_storage ss;
    socklen_t sl = sizeof ss;
    int fd = accept(s->fd, (struct sockaddr *)&ss, &sl);
    if (fd < 0)
      continue;
    fcntl(fd, F_SETFD, FD_CLOEXEC);
    fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK);
    int one = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);

    HttpConn *c = (HttpConn *)calloc(1, sizeof *c);
    if (!c) {
      close(fd);
      continue;
    }
    c->server = s;
    c->fd = fd;
    char host[INET6_ADDRSTRLEN] = "?";
    if (ss.ss_family == AF_INET6)
      inet_ntop(AF_INET6, &((struct sockaddr_in6 *)&ss)->sin6_addr, host,
                sizeof host);
    else if (ss.ss_family == AF_INET)
      inet_ntop(AF_INET, &((struct sockaddr_in *)&ss)->sin_addr, host,
                sizeof host);
    // An IPv4 client on the dual-stack socket arrives as ::ffff:a.b.c.d;
    // show it as the IPv4 address it is.
    const char *shown = strncmp(host, "::ffff:", 7) == 0 &&
                                strchr(host + 7, '.')
                            ? host + 7
                            : host;
    snprintf(c->addr, sizeof c->addr, "%s", shown);

    if (__atomic_add_fetch(&s->active, 1, __ATOMIC_ACQ_REL) > s->max_conns) {
      plain_error(c, 503, "too many connections\n");
      close(fd);
      __atomic_fetch_sub(&s->active, 1, __ATOMIC_ACQ_REL);
      free(c);
      continue;
    }
    pthread_t t;
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    if (pthread_create(&t, &attr, conn_thread, c) != 0) {
      close(fd);
      __atomic_fetch_sub(&s->active, 1, __ATOMIC_ACQ_REL);
      free(c);
    }
    pthread_attr_destroy(&attr);
  }
  return NULL;
}

HttpServer *http_start(const char *bind_spec, int max_conns,
                       HttpHandler handler, void *ctx, char *err,
                       size_t err_cap) {
  char host[128], port[16];
  if (!split_host_port(bind_spec, host, sizeof host, port, sizeof port)) {
    snprintf(err, err_cap, "bad address \"%s\"", bind_spec);
    return NULL;
  }
  struct addrinfo hints, *res = NULL;
  memset(&hints, 0, sizeof hints);
  hints.ai_socktype = SOCK_STREAM;
  hints.ai_flags = AI_PASSIVE | AI_NUMERICHOST;
  hints.ai_family = host[0] ? AF_UNSPEC : AF_INET6;
  int rc = getaddrinfo(host[0] ? host : NULL, port, &hints, &res);
  if (rc != 0 && !host[0]) { // no IPv6 at all: IPv4 then
    hints.ai_family = AF_INET;
    rc = getaddrinfo(NULL, port, &hints, &res);
  }
  if (rc != 0 || !res) {
    snprintf(err, err_cap, "bad address \"%s\": %s", bind_spec,
             gai_strerror(rc));
    return NULL;
  }
  int fd = socket(res->ai_family, SOCK_STREAM | SOCK_CLOEXEC, 0);
  int one = 1, off = 0;
  if (fd >= 0) {
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    if (res->ai_family == AF_INET6 && !host[0])
      setsockopt(fd, IPPROTO_IPV6, IPV6_V6ONLY, &off, sizeof off);
  }
  if (fd < 0 || bind(fd, res->ai_addr, res->ai_addrlen) != 0 ||
      listen(fd, 16) != 0) {
    snprintf(err, err_cap, "cannot listen on %s: %s", bind_spec,
             strerror(errno));
    if (fd >= 0)
      close(fd);
    freeaddrinfo(res);
    return NULL;
  }
  freeaddrinfo(res);

  HttpServer *s = (HttpServer *)calloc(1, sizeof *s);
  if (!s) {
    close(fd);
    return NULL;
  }
  s->fd = fd;
  s->max_conns = max_conns;
  s->handler = handler;
  s->ctx = ctx;
  struct sockaddr_storage ss;
  socklen_t sl = sizeof ss;
  getsockname(fd, (struct sockaddr *)&ss, &sl);
  s->port = ss.ss_family == AF_INET6
                ? ntohs(((struct sockaddr_in6 *)&ss)->sin6_port)
                : ntohs(((struct sockaddr_in *)&ss)->sin_port);
  if (pthread_create(&s->thread, NULL, accept_loop, s) != 0) {
    snprintf(err, err_cap, "cannot start the server thread");
    close(fd);
    free(s);
    return NULL;
  }
  return s;
}

int http_port(const HttpServer *s) { return s ? s->port : 0; }

void http_stop(HttpServer *s) {
  if (!s)
    return;
  __atomic_store_n(&s->stop, true, __ATOMIC_RELEASE);
  pthread_join(s->thread, NULL);
  close(s->fd);
  // Connection threads finish their request (streaming ones notice
  // http_stopping); give them a few seconds.
  for (int i = 0; i < 100 && __atomic_load_n(&s->active, __ATOMIC_ACQUIRE);
       i++) {
    struct timespec ts = {0, 50000000L};
    nanosleep(&ts, NULL);
  }
  if (__atomic_load_n(&s->active, __ATOMIC_ACQUIRE) == 0)
    free(s);
  // else: leaked deliberately -- a thread still holds it, and the process
  // is on its way out.
}

// ---- Client -----------------------------------------------------------------

int http_call(const char *hostport, const char *method, const char *path,
              const char *body, size_t body_len, char *out, size_t out_cap,
              size_t *out_len, int timeout_ms, char *err, size_t err_cap) {
  *out_len = 0;
  if (out_cap)
    out[0] = '\0';
  char host[128], port[16];
  if (!split_host_port(hostport, host, sizeof host, port, sizeof port) ||
      !host[0]) {
    snprintf(err, err_cap, "bad address \"%s\"", hostport);
    return -1;
  }
  int64_t deadline = now_ms() + timeout_ms;
  struct addrinfo hints, *res = NULL;
  memset(&hints, 0, sizeof hints);
  hints.ai_socktype = SOCK_STREAM;
  int rc = getaddrinfo(host, port, &hints, &res);
  if (rc != 0 || !res) {
    snprintf(err, err_cap, "cannot resolve %s: %s", host, gai_strerror(rc));
    return -1;
  }
  int fd = socket(res->ai_family, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK,
                  0);
  if (fd < 0) {
    snprintf(err, err_cap, "socket: %s", strerror(errno));
    freeaddrinfo(res);
    return -1;
  }
  int c = connect(fd, res->ai_addr, res->ai_addrlen);
  freeaddrinfo(res);
  if (c != 0 && errno != EINPROGRESS) {
    snprintf(err, err_cap, "cannot connect to %s: %s", hostport,
             strerror(errno));
    close(fd);
    return -1;
  }
  if (c != 0) {
    int soerr = 0;
    socklen_t sl = sizeof soerr;
    if (wait_fd(fd, POLLOUT, deadline) <= 0 ||
        getsockopt(fd, SOL_SOCKET, SO_ERROR, &soerr, &sl) != 0 || soerr) {
      snprintf(err, err_cap, "cannot connect to %s: %s", hostport,
               soerr ? strerror(soerr) : "timed out");
      close(fd);
      return -1;
    }
  }
  int one = 1;
  setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);

  char head[512];
  int hn = snprintf(head, sizeof head,
                    "%s %s HTTP/1.1\r\nHost: %s\r\nConnection: close\r\n"
                    "Content-Type: application/json\r\n"
                    "Content-Length: %zu\r\n\r\n",
                    method, path, host, body_len);
  int64_t left = deadline - now_ms();
  if (left <= 0 || !send_all(fd, head, (size_t)hn, (int)left) ||
      (body_len && !send_all(fd, body, body_len, (int)(deadline - now_ms())))) {
    snprintf(err, err_cap, "sending to %s failed", hostport);
    close(fd);
    return -1;
  }

  static __thread char rbuf[http_max_head + http_max_body];
  size_t have = 0;
  HttpResponseHead rh;
  HttpParse pr = HTTP_PARSE_INCOMPLETE;
  for (;;) {
    if (have == sizeof rbuf)
      break;
    if (wait_fd(fd, POLLIN, deadline) <= 0) {
      snprintf(err, err_cap, "%s did not answer in time", hostport);
      close(fd);
      return -1;
    }
    ssize_t n = recv(fd, rbuf + have, sizeof rbuf - have, MSG_DONTWAIT);
    if (n < 0 && (errno == EAGAIN || errno == EINTR))
      continue;
    if (n <= 0)
      break; // closed: the response is complete
    have += (size_t)n;
    pr = http_parse_response(rbuf, have, &rh);
    if (pr == HTTP_PARSE_OK && rh.has_content_length &&
        have >= rh.head_len + rh.content_length)
      break;
    if (pr == HTTP_PARSE_BAD || pr == HTTP_PARSE_TOO_LARGE)
      break;
  }
  close(fd);
  pr = http_parse_response(rbuf, have, &rh);
  if (pr != HTTP_PARSE_OK) {
    snprintf(err, err_cap, "malformed response from %s", hostport);
    return -1;
  }
  size_t blen = have - rh.head_len;
  if (rh.has_content_length && blen > rh.content_length)
    blen = rh.content_length;
  if (out_cap) {
    size_t keep = blen < out_cap - 1 ? blen : out_cap - 1;
    memcpy(out, rbuf + rh.head_len, keep);
    out[keep] = '\0';
    *out_len = keep;
  }
  return rh.status;
}
