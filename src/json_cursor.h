#pragma once
/*
 * A minimal JSON reader: a cursor over a buffer, and parsers for the value
 * shapes eeye's configs use -- plain strings, small unsigned and signed
 * integers, and booleans. Shared by eeye's config parser (config.c) and
 * hellion's own config.
 *
 * Deliberately not a general JSON library. Strings take no escape
 * sequences (every key and value in these schemas is a plain identifier or
 * a path), numbers are integers only, and each parser checks its own range.
 * A caller walks the object itself, which is what lets each config give a
 * precise error for the exact key that is wrong.
 *
 * Every function is static inline so a file including this pays only for
 * what it calls, and -Wunused-function stays quiet about the rest.
 */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
  const char *s;
  size_t len;
  size_t pos;
} Cursor;

static inline void skip_ws(Cursor *c) {
  while (c->pos < c->len) {
    unsigned char ch = (unsigned char)c->s[c->pos];
    if (ch == ' ' || ch == '\t' || ch == '\n' || ch == '\r') {
      c->pos++;
    } else {
      break;
    }
  }
}

static inline bool cur_peek(const Cursor *c, char *out) {
  if (c->pos >= c->len)
    return false;
  *out = c->s[c->pos];
  return true;
}

static inline bool cur_expect(Cursor *c, char expected) {
  char ch;
  if (!cur_peek(c, &ch) || ch != expected)
    return false;
  c->pos++;
  return true;
}

// No escape sequences: every key and every string value in this schema is a
// plain identifier ("effect", "sobel", ...), so a backslash in the input is
// treated as a malformed file rather than something to interpret.
static inline bool parse_string(Cursor *c, char *out, size_t out_cap) {
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

static inline bool parse_u8(Cursor *c, uint8_t *out) {
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

// Signed, and wider than the u8/u16 parsers above: brightness is
// -100..100 and exposure is in microseconds, which passes 65535 at a
// fortieth of a second.
static inline bool parse_i32(Cursor *c, int32_t *out) {
  size_t start = c->pos;
  if (c->pos < c->len && c->s[c->pos] == '-')
    c->pos++;

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
  if (n >= sizeof buf) {
    c->pos = start;
    return false;
  }
  memcpy(buf, c->s + start, n);
  buf[n] = '\0';

  char *end = NULL;
  long v = strtol(buf, &end, 10);
  if (end == buf || *end != '\0' || v < INT32_MIN + 1 || v > INT32_MAX) {
    c->pos = start;
    return false;
  }
  *out = (int32_t)v;
  return true;
}

// The only bool-valued key so far. Accepts the two JSON literals and
// nothing else -- 0/1 would be a reasonable extension but silently
// accepting a number where a bool belongs is the kind of leniency that
// hides a typo.
static inline bool parse_bool(Cursor *c, bool *out) {
  if (c->pos + 4 <= c->len && memcmp(c->s + c->pos, "true", 4) == 0) {
    c->pos += 4;
    *out = true;
    return true;
  }
  if (c->pos + 5 <= c->len && memcmp(c->s + c->pos, "false", 5) == 0) {
    c->pos += 5;
    *out = false;
    return true;
  }
  return false;
}

// Same shape as parse_u8 above, widened for the capture dimensions --
// those are the only config values that don't fit in a byte. Kept as a
// separate function rather than a shared parse_uint(max) because the two
// callers' error messages and range checks read better spelled out, and
// this way parse_u8's hot path (every effect parameter) stays exactly as
// it was.
static inline bool parse_u16(Cursor *c, uint16_t *out) {
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
