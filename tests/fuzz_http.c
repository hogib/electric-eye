// libFuzzer target for hellion's HTTP parsers: the only code in hellion
// that interprets bytes from the network. Run with `make fuzz-http`
// (clang; FUZZ_SECONDS=N to change how long).
//
// Beyond "no crash, no out-of-bounds access" (ASan and UBSan watch for
// those), it checks what a successful parse promises its callers.
#include "http.c"
#include <assert.h>

static void check_request(const uint8_t *data, size_t size) {
  HttpRequest r;
  HttpParse p = http_parse_request((const char *)data, size, &r);
  if (p != HTTP_PARSE_OK)
    return;
  assert(r.head_len > 0 && r.head_len <= size && r.head_len <= http_max_head);
  assert(r.method[0] >= 'A' && r.method[0] <= 'Z');
  assert(strlen(r.method) < sizeof r.method);
  assert(r.path[0] == '/');
  assert(strlen(r.path) < sizeof r.path);
  assert(strlen(r.query) < sizeof r.query);
  assert(!strchr(r.path, '?'));
  for (const char *c = r.path; *c; c++)
    assert(*c > 0x20 && *c < 0x7f);
  // The head ends in a blank line, exactly where it says.
  assert(memcmp(data + r.head_len - 4, "\r\n\r\n", 4) == 0);
  if (!r.has_content_length)
    assert(r.content_length == 0);

  // Parsing only the head gives the same answer: the parser must not have
  // looked past it.
  HttpRequest again;
  assert(http_parse_request((const char *)data, r.head_len, &again) ==
         HTTP_PARSE_OK);
  assert(again.head_len == r.head_len &&
         again.content_length == r.content_length &&
         strcmp(again.path, r.path) == 0);
}

static void check_response(const uint8_t *data, size_t size) {
  HttpResponseHead h;
  HttpParse p = http_parse_response((const char *)data, size, &h);
  if (p != HTTP_PARSE_OK)
    return;
  assert(h.status >= 100 && h.status <= 999);
  assert(h.head_len > 0 && h.head_len <= size);
  assert(memcmp(data + h.head_len - 4, "\r\n\r\n", 4) == 0);
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
  check_request(data, size);
  check_response(data, size);
  return 0;
}
