#define _POSIX_C_SOURCE 200809L

#include "health.h"
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <unistd.h>

uint64_t health_disk_free(const char *path) {
  if (!path || path[0] == '\0')
    return 0;

  struct statvfs vfs;
  if (statvfs(path, &vfs) == 0)
    return (uint64_t)vfs.f_bavail * vfs.f_frsize;

  // The file itself may not exist yet -- which is the normal case when
  // checking whether there is room *before* starting a recording. Fall
  // back to its parent directory.
  char dir[512];
  snprintf(dir, sizeof dir, "%s", path);
  char *slash = strrchr(dir, '/');
  if (!slash)
    return statvfs(".", &vfs) == 0
               ? (uint64_t)vfs.f_bavail * vfs.f_frsize
               : 0;
  if (slash == dir)
    slash[1] = '\0'; // path was "/file": the directory is "/"
  else
    *slash = '\0';
  if (statvfs(dir, &vfs) == 0)
    return (uint64_t)vfs.f_bavail * vfs.f_frsize;
  return 0;
}

// Escapes a string for embedding in a JSON string literal. Paths and
// errno strings are the only things written here, but a path may legally
// contain a quote or a backslash, and one unescaped byte would make the
// whole file unparseable to the reader that needs it most.
static void json_escape(const char *in, char *out, size_t out_cap) {
  size_t o = 0;
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

static const char *state_name(RecordingState s) {
  switch (s) {
  case RECORDING_ACTIVE: return "active";
  case RECORDING_FAILED: return "failed";
  case RECORDING_OFF:
  default:               return "off";
  }
}

void health_publish(const char *config_path, const HealthSnapshot *snap) {
  char path[512];
  char tmp[544];
  if (snprintf(path, sizeof path, "%s.health", config_path) >=
      (int)sizeof path)
    return; // pathologically long config path; nothing useful to do

  // Temp file in the same directory, then rename -- same contract as the
  // config status file, so a reader can never catch this half-written.
  if (snprintf(tmp, sizeof tmp, "%s.tmp", path) >= (int)sizeof tmp)
    return;

  FILE *f = fopen(tmp, "wb");
  if (!f)
    return; // best-effort: health reporting must never break the pipeline

  char esc_path[512], esc_err[256];
  json_escape(snap->recording_path, esc_path, sizeof esc_path);
  json_escape(snap->recording_error, esc_err, sizeof esc_err);

  fprintf(f,
          "{\"recording\":{\"state\":\"%s\",\"path\":\"%s\",\"error\":\"%s\","
          "\"bytes\":%llu,\"disk_free_bytes\":%llu},"
          "\"camera\":{\"connected\":%s,\"frames\":%llu},"
          "\"geometry\":{\"width\":%u,\"height\":%u}}\n",
          state_name(snap->recording_state), esc_path, esc_err,
          (unsigned long long)snap->recording_bytes,
          (unsigned long long)snap->disk_free_bytes,
          snap->camera_connected ? "true" : "false",
          (unsigned long long)snap->frames_captured, snap->frame_width,
          snap->frame_height);

  if (fclose(f) != 0 || rename(tmp, path) != 0)
    unlink(tmp);
}
