// `hellion check`: one pass over everything top shows, as a verdict a
// pre-dive checklist (or a script) can act on.
//   0  healthy
//   1  degraded: running, but something is waiting, idle or failing
//   2  down: no hellion responding, or a pipeline with no video at all
#define _POSIX_C_SOURCE 200809L

#include "fbus.h"
#include "hellion.h"
#include "hstatus.h"
#include <stdio.h>
#include <string.h>
#include <time.h>

static const double heartbeat_stale_s = 2.0;

static int worst = 0;

static void say(int level, const char *fmt, const char *a, const char *b) {
  static const char *const tags[] = {"[ OK ]", "[WARN]", "[FAIL]"};
  printf("%s ", tags[level]);
  printf(fmt, a, b);
  printf("\n");
  if (level > worst)
    worst = level;
}

static bool sink_ok(const char *state) {
  return strcmp(state, "publishing") == 0 || strcmp(state, "writing") == 0 ||
         strcmp(state, "sending") == 0;
}

// Two snapshots ~1.2s apart: "frames are moving" is judged from counters
// that actually moved, not from an fps figure that may not have had a
// window to settle yet.
static void check_instance(const char *name) {
  static HsStatus a, b;
  HsReadResult r = hs_read(name, &a);
  if (r != HS_READ_OK) {
    say(2, "hellion \"%s\": %s", name,
        r == HS_READ_MISSING ? "not running" : "status unreadable");
    return;
  }
  struct timespec ts = {1, 200000000L};
  nanosleep(&ts, NULL);
  if (hs_read(name, &b) != HS_READ_OK || hs_heartbeat_age(&b) > heartbeat_stale_s) {
    say(2, "hellion \"%s\": %s", name, "not responding");
    return;
  }
  say(0, "hellion \"%s\" (%s) responding", b.name, b.role);

  // The other end of the tether: without it, config changes and the far
  // end's state go nowhere, though the video may still flow.
  if (b.peer_configured) {
    struct timespec ts_now;
    clock_gettime(CLOCK_MONOTONIC, &ts_now);
    uint64_t now = (uint64_t)ts_now.tv_sec * 1000000000ull +
                   (uint64_t)ts_now.tv_nsec;
    double age = b.peer_contact_mono_ns
                     ? (double)(now - b.peer_contact_mono_ns) / 1e9
                     : -1;
    char what[320];
    if (age >= 0 && age <= 5) {
      snprintf(what, sizeof what, "%s, last contact %.1fs ago",
               b.peer_addr, age);
      say(0, "peer: %s%s", what, "");
    } else {
      snprintf(what, sizeof what, "%s %s (%s)", b.peer_addr,
               age < 0 ? "never reached" : "not reached for a while",
               b.peer_error[0] ? b.peer_error : "no error recorded");
      say(1, "peer: %s%s", what, "");
    }
  }

  // Discovery: an "auto" peer or sink with nothing found means video or
  // config goes nowhere; several candidates means it refused to guess.
  if (b.discovery_needed && b.discovery_state != 2)
    say(1, "discovery: %s%s", b.discovery,
        b.discovery_state == 3 ? "" : " (an \"auto\" address needs it)");
  else if (b.discovery_needed)
    say(0, "discovery: %s%s", b.discovery, "");

  for (uint32_t i = 0; i < b.pipeline_count; i++) {
    const HsPipeline *pb = &b.pipelines[i];
    const HsPipeline *pa = &a.pipelines[i];
    char what[160];
    uint64_t moved = pb->source.frames - pa->source.frames;
    if (moved > 0) {
      snprintf(what, sizeof what, "%s (%ux%u, %llu frames in 1.2s)",
               pb->source.spec, pb->width, pb->height,
               (unsigned long long)moved);
      say(0, "pipeline %s: source %s", pb->name, what);
      // A receiving link that delivers frames can still be losing
      // packets: damaged frames now, a dropped link soon.
      if (pb->source.net && pb->source.loss_pct >= 1.0) {
        snprintf(what, sizeof what, "%.1f%% packet loss at %.0f kbit/s",
                 pb->source.loss_pct, pb->source.kbps);
        say(1, "pipeline %s: link in: %s", pb->name, what);
      }
    } else {
      snprintf(what, sizeof what, "%s is %s%s%s", pb->source.spec,
               pb->source.state, pb->source.detail[0] ? ": " : "",
               pb->source.detail);
      say(2, "pipeline %s: no frames -- %s", pb->name, what);
      continue;
    }
    for (uint32_t k = 0; k < pb->sink_count; k++) {
      const HsEndpoint *s = &pb->sinks[k];
      uint64_t sent = s->frames - pa->sinks[k].frames;
      if (sink_ok(s->state) && sent > 0) {
        snprintf(what, sizeof what, "%s (%llu frames)", s->spec,
                 (unsigned long long)sent);
        say(0, "pipeline %s: sink %s", pb->name, what);
      } else {
        snprintf(what, sizeof what, "%s is %s%s%s", s->spec, s->state,
                 s->detail[0] ? ": " : "", s->detail);
        say(1, "pipeline %s: sink %s", pb->name, what);
      }
    }
  }
}

static void check_stream(const char *name, void *ctx) {
  (void)ctx;
  FbusStatus st;
  FbusOpenResult r = fbus_status(name, &st);
  char what[128];
  if (r != FBUS_OPEN_OK) {
    say(1, "bus stream %s: %s", name, fbus_open_result_str(r));
    return;
  }
  if (!st.alive) {
    snprintf(what, sizeof what, "%s (writer pid %d)",
             st.closed ? "closed" : "writer gone", st.writer_pid);
    say(1, "bus stream %s: %s", name, what);
  } else if (st.frames == 0 || st.last_frame_age_ns > 2000000000ull) {
    say(1, "bus stream %s: %s", name, "alive but no recent frames");
  } else {
    snprintf(what, sizeof what, "%ux%u, last frame %.2fs ago", st.width,
             st.height, (double)st.last_frame_age_ns / 1e9);
    say(0, "bus stream %s: %s", name, what);
  }
}

int hellion_check(const char *name) {
  char names[8][hs_name_len];
  int n;
  if (name) {
    snprintf(names[0], hs_name_len, "%s", name);
    n = 1;
  } else {
    n = hs_list(names, 8);
  }
  if (n == 0)
    say(2, "no hellion running (no status files in %s)%s", fbus_dir(), "");
  for (int i = 0; i < n; i++)
    check_instance(names[i]);
  fbus_list(check_stream, NULL);

  printf("%s\n", worst == 0   ? "Healthy."
                 : worst == 1 ? "Degraded: see the WARN lines."
                              : "DOWN: see the FAIL lines.");
  return worst;
}
