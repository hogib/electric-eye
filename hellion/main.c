// hellion: everything that touches the tether. See docs/hellion.md.
//
//   hellion [-c config]            run (the default)
//   hellion top [-n name] [--once] live dashboard, for an SSH session
//   hellion check [-n name]        one-shot health check; exit 0/1/2
#define _POSIX_C_SOURCE 200809L

#include "control.h"
#include "hconfig.h"
#include "hellion.h"
#include "hlog.h"
#include "hstatus.h"
#include "pipeline.h"
#include "hrecord.h"
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static const char default_config[] = "/etc/hellion/hellion.json";
static const int status_period_ms = 250;

static volatile sig_atomic_t stop_requested = 0;

static void on_signal(int sig) {
  (void)sig;
  stop_requested = 1;
}

static void usage(FILE *to) {
  fprintf(to,
          "usage: hellion [-c config]            run (default config %s)\n"
          "       hellion top [-n name] [--once] live dashboard\n"
          "       hellion check [-n name]        health check: exit 0 ok, 1 "
          "degraded, 2 down\n",
          default_config);
}

static int run(const char *config_path) {
  // Before anything prints, so top's log has the whole story.
  bool capturing = hlog_start();

  HellionConfig cfg;
  if (!hconfig_load(config_path, &cfg)) {
    if (capturing)
      hlog_stop();
    return 1;
  }

  signal(SIGPIPE, SIG_IGN);
  struct sigaction sa;
  memset(&sa, 0, sizeof sa);
  sa.sa_handler = on_signal;
  sigaction(SIGINT, &sa, NULL);
  sigaction(SIGTERM, &sa, NULL);

  printf("hellion \"%s\" (%s) starting with %zu pipeline%s from %s\n",
         cfg.name, cfg.role, cfg.pipeline_count,
         cfg.pipeline_count == 1 ? "" : "s", config_path);

  HsWriter *status = hs_writer_open(cfg.name);

  // Camera controls for camera sources: what POST /camera last kept, so a
  // restart (or a crash) puts the camera back as it was. Only read when
  // there is a camera here.
  CameraControls camera;
  bool have_camera = false;
  for (size_t i = 0; i < cfg.pipeline_count; i++)
    have_camera |= cfg.pipelines[i].source == SRC_V4L2 ||
                   cfg.pipelines[i].source == SRC_RPICAM;
  if (have_camera) {
    char err[200];
    if (!control_load_camera(cfg.camera_file, &camera, err, sizeof err))
      printf("hellion: camera controls in %s ignored (%s); using defaults\n",
             cfg.camera_file, err);
  }

  Pipeline *pipes[hcfg_max_pipelines] = {0};
  for (size_t i = 0; i < cfg.pipeline_count; i++) {
    bool cam = cfg.pipelines[i].source == SRC_V4L2 ||
               cfg.pipelines[i].source == SRC_RPICAM;
    pipes[i] = pipeline_start_with_camera(&cfg.pipelines[i],
                                          cam ? &camera : NULL);
  }
  HRec *rec = hrec_create(&cfg, pipes, cfg.pipeline_count);
  Control *control = control_start(&cfg, rec, pipes, cfg.pipeline_count);

  // The status snapshot is big (the log alone is 10KB): static, and only
  // this thread touches it.
  static HsStatus snap;
  struct timespec started;
  clock_gettime(CLOCK_REALTIME, &started);

  while (!stop_requested) {
    memset(&snap, 0, sizeof snap);
    snprintf(snap.name, sizeof snap.name, "%s", cfg.name);
    snprintf(snap.role, sizeof snap.role, "%s", cfg.role);
    snprintf(snap.eeye_config, sizeof snap.eeye_config, "%s",
             cfg.eeye_config);
    snap.started_real_ns =
        (uint64_t)started.tv_sec * 1000000000ull + (uint64_t)started.tv_nsec;
    snap.pipeline_count = (uint32_t)cfg.pipeline_count;
    for (size_t i = 0; i < cfg.pipeline_count; i++) {
      if (pipes[i]) {
        pipeline_status(pipes[i], &snap.pipelines[i]);
      } else {
        snprintf(snap.pipelines[i].name, hs_name_len, "%s",
                 cfg.pipelines[i].name);
        snprintf(snap.pipelines[i].source.state, hs_state_len, "error");
        snprintf(snap.pipelines[i].source.detail, hs_detail_len,
                 "pipeline did not start (see log)");
      }
    }
    hrec_summary(rec, snap.recording, sizeof snap.recording);
    control_fill(control, &snap);
    hs_writer_publish(status, &snap);
    control_set_status(control, &snap);

    struct timespec ts = {0, (long)status_period_ms * 1000000L};
    nanosleep(&ts, NULL); // a signal cuts it short, which is fine
  }

  printf("hellion \"%s\" stopping\n", cfg.name);
  // The page first (no new requests), then the recording (finalized while
  // its pipeline still exists), then the pipelines.
  control_stop(control);
  hrec_destroy(rec);
  for (size_t i = 0; i < cfg.pipeline_count; i++)
    pipeline_stop(pipes[i]);
  hs_writer_close(status);
  printf("hellion \"%s\" stopped\n", cfg.name);
  if (capturing)
    hlog_stop();
  return 0;
}

int main(int argc, char **argv) {
  const char *config_path = default_config;
  const char *name = NULL;
  const char *command = "run";
  bool once = false;

  for (int i = 1; i < argc; i++) {
    const char *a = argv[i];
    if ((strcmp(a, "-c") == 0 || strcmp(a, "--config") == 0) && i + 1 < argc) {
      config_path = argv[++i];
    } else if ((strcmp(a, "-n") == 0 || strcmp(a, "--name") == 0) &&
               i + 1 < argc) {
      name = argv[++i];
    } else if (strcmp(a, "--once") == 0) {
      once = true;
    } else if (strcmp(a, "-h") == 0 || strcmp(a, "--help") == 0) {
      usage(stdout);
      return 0;
    } else if (strcmp(a, "run") == 0 || strcmp(a, "top") == 0 ||
               strcmp(a, "check") == 0) {
      command = a;
    } else {
      fprintf(stderr, "hellion: unexpected argument \"%s\"\n", a);
      usage(stderr);
      return 2;
    }
  }

  if (strcmp(command, "top") == 0)
    return hellion_top(name, once);
  if (strcmp(command, "check") == 0)
    return hellion_check(name);
  return run(config_path);
}
