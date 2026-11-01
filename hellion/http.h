#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/*
 * A small, strict HTTP/1.x server and client: hellion's browser page and
 * its link to the hellion at the other end of the tether.
 *
 * Strict on purpose, since this is the one part of hellion that parses
 * input from the network:
 * - the request head is at most 8KB and a body at most 64KB;
 * - only Content-Length bodies: Transfer-Encoding is refused outright,
 *   rather than half-supported;
 * - one request per connection (Connection: close), so there is no
 *   request boundary to get wrong between two requests;
 * - every read and write has a deadline, so a client that stops reading
 *   or writing loses its connection, not a thread forever.
 * The parsers are pure functions, fuzzed (tests/fuzz_http.c).
 */

enum {
  http_max_head = 8192,
  http_max_body = 65536,
  http_max_target = 512,
};

typedef struct {
  char method[8];
  char path[http_max_target];  // the target up to '?'
  char query[http_max_target]; // after '?', "" if none
  bool has_content_length;
  size_t content_length;
  size_t head_len; // bytes up to and including the blank line
} HttpRequest;

typedef enum {
  HTTP_PARSE_OK = 0,
  HTTP_PARSE_INCOMPLETE, // no blank line yet: read more
  HTTP_PARSE_BAD,        // malformed or unsupported
  HTTP_PARSE_TOO_LARGE,  // head over http_max_head
} HttpParse;

// Parses a request head from buf[0..len).
HttpParse http_parse_request(const char *buf, size_t len, HttpRequest *out);

typedef struct {
  int status;
  bool has_content_length;
  size_t content_length;
  size_t head_len;
} HttpResponseHead;

// Parses a response head from buf[0..len).
HttpParse http_parse_response(const char *buf, size_t len,
                              HttpResponseHead *out);

// ---- Server ---------------------------------------------------------------

typedef struct HttpConn HttpConn;

// Called on its own thread for each request. `body` is the whole request
// body (NUL-terminated for convenience), or NULL.
typedef void (*HttpHandler)(HttpConn *c, const HttpRequest *req,
                            const char *body, size_t body_len, void *ctx);

typedef struct HttpServer HttpServer;

// Listens on `bind_spec` ("8080" for every interface, or "addr:port") and
// serves each connection on its own thread, at most `max_conns` at once
// (past that a client gets 503). NULL with *err on failure.
HttpServer *http_start(const char *bind_spec, int max_conns,
                       HttpHandler handler, void *ctx, char *err,
                       size_t err_cap);

int http_port(const HttpServer *s);

// Stops listening and waits for the connection threads to finish (a
// streaming handler must watch http_stopping()).
void http_stop(HttpServer *s);

// Whether the server is shutting down; long-running handlers poll it.
bool http_stopping(const HttpConn *c);

// Whether the client has hung up, without blocking. A long-running
// handler that has nothing to write for a while (a preview of a stream
// that is down) must ask: only a write would otherwise find out, and
// until then the connection, its thread and its slot stay taken.
bool http_peer_gone(HttpConn *c);

// A complete response. Adds Content-Length, Connection: close and
// Cache-Control: no-store; text/* gets "; charset=utf-8".
void http_respond(HttpConn *c, int status, const char *content_type,
                  const void *body, size_t len);

// Convenience: a JSON body from printf-style arguments.
void http_respond_json(HttpConn *c, int status, const char *fmt, ...)
    __attribute__((format(printf, 3, 4)));

// For streaming responses (MJPEG): send the status line and headers
// yourself with http_write. False once the client is gone or too slow
// (it could not take `len` bytes within timeout_ms).
bool http_write(HttpConn *c, const void *data, size_t len, int timeout_ms);

// The peer's address, for logs.
const char *http_client_addr(const HttpConn *c);

// ---- Client ---------------------------------------------------------------

/*
 * One request to "host:port" ("[v6addr%iface]:port" for link-local).
 * Returns the HTTP status, or -1 with *err filled. The response body goes
 * to out (NUL-terminated, truncated at out_cap - 1; *out_len is what was
 * kept). timeout_ms bounds the whole exchange.
 */
int http_call(const char *hostport, const char *method, const char *path,
              const char *body, size_t body_len, char *out, size_t out_cap,
              size_t *out_len, int timeout_ms, char *err, size_t err_cap);
