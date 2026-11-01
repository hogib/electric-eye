#pragma once
/*
 * Writing JSON by hand with snprintf: the one thing that needs care is
 * putting arbitrary text inside a string. Shared by eeye's health file and
 * hellion's HTTP responses.
 */
#include <stddef.h>
#include <stdio.h>

// Copies `in` into `out` escaped for use between JSON double quotes,
// truncating (never splitting an escape) if it does not fit.
static inline void json_escape(const char *in, char *out, size_t out_cap) {
  size_t o = 0;
  if (out_cap == 0)
    return;
  for (size_t i = 0; in[i] != '\0' && o + 2 < out_cap; i++) {
    unsigned char c = (unsigned char)in[i];
    if (c == '"' || c == '\\') {
      out[o++] = '\\';
      out[o++] = (char)c;
    } else if (c < 0x20) {
      if (o + 6 >= out_cap)
        break;
      o += (size_t)snprintf(out + o, out_cap - o, "\\u%04x", c);
    } else {
      out[o++] = (char)c;
    }
  }
  out[o] = '\0';
}
