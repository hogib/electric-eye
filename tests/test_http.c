// hellion's HTTP parsers decide what network input is accepted, so they
// are tested here case by case (and fuzzed by tests/fuzz_http.c); the
// server and client are then exercised over a real socket.

#include "http.c"
#include "test_harness.h"
#include <signal.h>

static HttpParse req(const char *s, HttpRequest *r) {
  return http_parse_request(s, strlen(s), r);
}

static void test_parse_simple_get(void) {
  HttpRequest r;
  CHECK_EQ_INT(req("GET /stream?src=pilot&t=1 HTTP/1.1\r\nHost: x\r\n\r\n",
                   &r),
               HTTP_PARSE_OK);
  CHECK(strcmp(r.method, "GET") == 0);
  CHECK(strcmp(r.path, "/stream") == 0);
  CHECK(strcmp(r.query, "src=pilot&t=1") == 0);
  CHECK(!r.has_content_length);
  CHECK_EQ_INT(r.head_len,
               strlen("GET /stream?src=pilot&t=1 HTTP/1.1\r\nHost: x\r\n\r\n"));

  CHECK_EQ_INT(req("GET / HTTP/1.0\r\n\r\n", &r), HTTP_PARSE_OK);
  CHECK(strcmp(r.path, "/") == 0);
  CHECK_EQ_INT(r.query[0], '\0');
}

static void test_parse_post_with_body(void) {
  HttpRequest r;
  const char *s = "POST /config HTTP/1.1\r\nContent-Type: application/json\r\n"
                  "content-length:  12 \r\n\r\n{\"chain\":[]}";
  CHECK_EQ_INT(req(s, &r), HTTP_PARSE_OK);
  CHECK(r.has_content_length);
  CHECK_EQ_INT(r.content_length, 12);
  CHECK(strcmp(s + r.head_len, "{\"chain\":[]}") == 0);
  // The same length twice is fine; two different lengths are not.
  CHECK_EQ_INT(req("POST / HTTP/1.1\r\nContent-Length: 3\r\n"
                   "Content-Length: 3\r\n\r\n",
                   &r),
               HTTP_PARSE_OK);
  CHECK_EQ_INT(req("POST / HTTP/1.1\r\nContent-Length: 3\r\n"
                   "Content-Length: 4\r\n\r\n",
                   &r),
               HTTP_PARSE_BAD);
}

static void test_parse_incomplete_and_too_large(void) {
  HttpRequest r;
  CHECK_EQ_INT(req("", &r), HTTP_PARSE_INCOMPLETE);
  CHECK_EQ_INT(req("GET / HTTP/1.1\r\n", &r), HTTP_PARSE_INCOMPLETE);
  CHECK_EQ_INT(req("GET / HTTP/1.1\r\nHost: x\r\n\r", &r),
               HTTP_PARSE_INCOMPLETE);

  static char big[http_max_head + 100];
  memset(big, 'a', sizeof big);
  memcpy(big, "GET / HTTP/1.1\r\nX: ", 19);
  CHECK_EQ_INT(http_parse_request(big, sizeof big, &r), HTTP_PARSE_TOO_LARGE);
  CHECK_EQ_INT(http_parse_request(big, http_max_head, &r),
               HTTP_PARSE_TOO_LARGE);
}

static void test_parse_rejects(void) {
  HttpRequest r;
  static const char *const bad[] = {
      "get / HTTP/1.1\r\n\r\n",                    // lowercase method
      "GET  / HTTP/1.1\r\n\r\n",                   // double space
      "GET nope HTTP/1.1\r\n\r\n",                 // target not a path
      "GET / HTTP/2.0\r\n\r\n",                    // unsupported version
      "GET / HTTP/1.1\n\n",                        // bare LF
      "GET / HTTP/1.1\r\nHost: x\n\r\n",           // bare LF in a header
      "GET / HTTP/1.1\rX\r\n\r\n",                 // bare CR
      "GET / HTTP/1.1\r\n Host: x\r\n\r\n",        // obsolete line folding
      "GET / HTTP/1.1\r\nHost x\r\n\r\n",          // no colon
      "GET / HTTP/1.1\r\n: x\r\n\r\n",             // empty name
      "GET / HTTP/1.1\r\nBad Name: x\r\n\r\n",     // space in name
      "POST / HTTP/1.1\r\nTransfer-Encoding: chunked\r\n\r\n",
      "POST / HTTP/1.1\r\nContent-Length: -1\r\n\r\n",
      "POST / HTTP/1.1\r\nContent-Length: 1x\r\n\r\n",
      "POST / HTTP/1.1\r\nContent-Length:\r\n\r\n",
      "POST / HTTP/1.1\r\nContent-Length: 9999999999\r\n\r\n",
      "GET /\x01 HTTP/1.1\r\n\r\n",                // control char in target
      "GET / HTTP/1.1\r\nX: a\x01b\r\n\r\n",       // control char in value
      "GETTTTTTT / HTTP/1.1\r\n\r\n",              // method too long
  };
  for (size_t i = 0; i < sizeof bad / sizeof bad[0]; i++) {
    HttpParse p = req(bad[i], &r);
    if (p != HTTP_PARSE_BAD)
      printf("    accepted (%d): %s\n", p, bad[i]);
    CHECK_EQ_INT(p, HTTP_PARSE_BAD);
  }
  // A NUL anywhere in the head.
  const char nul[] = "GET / HTTP/1.1\r\nX: a\0b\r\n\r\n";
  CHECK_EQ_INT(http_parse_request(nul, sizeof nul - 1, &r), HTTP_PARSE_BAD);
  // An overlong target.
  char longt[1200];
  memset(longt, 'a', sizeof longt);
  memcpy(longt, "GET /", 5);
  memcpy(longt + 700, " HTTP/1.1\r\n\r\n", 13);
  CHECK_EQ_INT(http_parse_request(longt, 713, &r), HTTP_PARSE_BAD);
}

static void test_parse_response(void) {
  HttpResponseHead h;
  const char *s = "HTTP/1.1 200 OK\r\nContent-Length: 2\r\n\r\nok";
  CHECK_EQ_INT(http_parse_response(s, strlen(s), &h), HTTP_PARSE_OK);
  CHECK_EQ_INT(h.status, 200);
  CHECK_EQ_INT(h.content_length, 2);
  CHECK(strcmp(s + h.head_len, "ok") == 0);
  CHECK_EQ_INT(http_parse_response("HTTP/1.0 502\r\n\r\n", 16, &h),
               HTTP_PARSE_OK);
  CHECK_EQ_INT(h.status, 502);
  CHECK_EQ_INT(http_parse_response("HTTP/1.1 2x0 OK\r\n\r\n", 19, &h),
               HTTP_PARSE_BAD);
  CHECK_EQ_INT(http_parse_response("HTTP/1.1 099 X\r\n\r\n", 18, &h),
               HTTP_PARSE_BAD);
  CHECK_EQ_INT(http_parse_response("ICY 200 OK\r\n\r\n", 14, &h),
               HTTP_PARSE_BAD);
  CHECK_EQ_INT(http_parse_response("HTTP/1.1 200 OK\r\n", 17, &h),
               HTTP_PARSE_INCOMPLETE);
}

// ---- Over a socket ------------------------------------------------------------

static void echo_handler(HttpConn *c, const HttpRequest *r, const char *body,
                         size_t len, void *ctx) {
  (void)ctx;
  if (strcmp(r->path, "/echo") == 0) {
    http_respond(c, 200, "text/plain", body ? body : "", len);
  } else if (strcmp(r->path, "/json") == 0) {
    http_respond_json(c, 201, "{\"method\":\"%s\",\"query\":\"%s\"}",
                      r->method, r->query);
  }
  // anything else: no response, which the server turns into a 500
}

// Sends raw bytes and returns the status line's code (0 if none).
static int raw_status(int port, const char *data, size_t len) {
  int fd = socket(AF_INET, SOCK_STREAM, 0);
  struct sockaddr_in a;
  memset(&a, 0, sizeof a);
  a.sin_family = AF_INET;
  a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  a.sin_port = htons((uint16_t)port);
  if (connect(fd, (struct sockaddr *)&a, sizeof a) != 0) {
    close(fd);
    return 0;
  }
  send(fd, data, len, MSG_NOSIGNAL);
  char buf[512];
  ssize_t n = recv(fd, buf, sizeof buf - 1, 0);
  close(fd);
  if (n < 12)
    return 0;
  buf[n] = '\0';
  return atoi(buf + 9);
}

static void test_server_and_client(void) {
  char err[200];
  HttpServer *s = http_start("127.0.0.1:0", 8, echo_handler, NULL, err,
                             sizeof err);
  CHECK(s != NULL);
  if (!s)
    return;
  char addr[64];
  snprintf(addr, sizeof addr, "127.0.0.1:%d", http_port(s));

  char out[256];
  size_t out_len;
  CHECK_EQ_INT(http_call(addr, "POST", "/echo", "hello", 5, out, sizeof out,
                         &out_len, 2000, err, sizeof err),
               200);
  CHECK_EQ_INT(out_len, 5);
  CHECK(strcmp(out, "hello") == 0);

  CHECK_EQ_INT(http_call(addr, "GET", "/json?x=1", NULL, 0, out, sizeof out,
                         &out_len, 2000, err, sizeof err),
               201);
  CHECK(strcmp(out, "{\"method\":\"GET\",\"query\":\"x=1\"}") == 0);

  CHECK_EQ_INT(http_call(addr, "GET", "/silent", NULL, 0, out, sizeof out,
                         &out_len, 2000, err, sizeof err),
               500);

  // A body bigger than the caller's buffer is truncated, not overrun.
  char small[4];
  CHECK_EQ_INT(http_call(addr, "POST", "/echo", "hello", 5, small,
                         sizeof small, &out_len, 2000, err, sizeof err),
               200);
  CHECK_EQ_INT(out_len, 3);
  CHECK(strcmp(small, "hel") == 0);

  // Malformed and oversized requests get the right refusal.
  const char *bad = "GET / HTTP/1.1\nHost: x\n\n";
  CHECK_EQ_INT(raw_status(http_port(s), bad, strlen(bad)), 400);
  const char *te = "POST /echo HTTP/1.1\r\nTransfer-Encoding: chunked\r\n\r\n";
  CHECK_EQ_INT(raw_status(http_port(s), te, strlen(te)), 400);
  char big[256];
  int n = snprintf(big, sizeof big,
                   "POST /echo HTTP/1.1\r\nContent-Length: %d\r\n\r\n",
                   http_max_body + 1);
  CHECK_EQ_INT(raw_status(http_port(s), big, (size_t)n), 413);
  static char huge[http_max_head + 10];
  memset(huge, 'a', sizeof huge);
  memcpy(huge, "GET / HTTP/1.1\r\nX: ", 19);
  CHECK_EQ_INT(raw_status(http_port(s), huge, sizeof huge), 431);

  // Nobody listening: an error, not a hang.
  CHECK_EQ_INT(http_call("127.0.0.1:1", "GET", "/", NULL, 0, out, sizeof out,
                         &out_len, 1000, err, sizeof err),
               -1);
  CHECK(strstr(err, "cannot connect") != NULL);

  http_stop(s);
}

int main(void) {
  signal(SIGPIPE, SIG_IGN);
  RUN_TEST(test_parse_simple_get);
  RUN_TEST(test_parse_post_with_body);
  RUN_TEST(test_parse_incomplete_and_too_large);
  RUN_TEST(test_parse_rejects);
  RUN_TEST(test_parse_response);
  RUN_TEST(test_server_and_client);
  TEST_MAIN_END();
}
