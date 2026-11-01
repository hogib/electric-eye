// _POSIX_C_SOURCE before any header include: -std=c99 puts glibc in strict
// ISO mode, which hides clock_gettime() unless a feature-test macro asks
// for it explicitly. Same guard as config.c/eeye.c/v4l2_in.c.
#define _POSIX_C_SOURCE 200809L

#include "virtual_cam.h"
#include "child_proc.h"
#include "v4l2_out.h"
#include <linux/videodev2.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

static const int64_t retry_missing_ms = 2000;
static const int64_t retry_min_ms = 2000;
static const int64_t retry_max_ms = 30000;

struct VirtualCam {
  char path[128];
  uint32_t width;
  uint32_t height;

  V4l2Out *out; // NULL while closed
  bool last_write_ok;

  int64_t next_attempt_ms; // monotonic; 0 = try on the next write
  int64_t backoff_ms;      // for a node that exists but will not open
  bool explained_missing;  // the "why is it missing" note was printed
  unsigned open_attempts;  // v4l2_out_open() calls; for tests
};

static int64_t now_ms(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static bool path_exists(const char *path) {
  struct stat st;
  return stat(path, &st) == 0;
}

// Runs a short command and returns its stdout (stderr discarded). Used only
// for the diagnostics below.
static void command_output(char *const argv[], char *out, size_t cap) {
  out[0] = '\0';
  if (!child_program_exists(argv[0]))
    return;
  ChildProc proc;
  if (!child_spawn(&proc, "virtual_cam", argv,
                   CHILD_PIPE_STDOUT | CHILD_QUIET_STDERR | CHILD_NULL_STDIN))
    return;
  child_read_all(proc.out_fd, out, cap, 3000);
  child_stop(&proc, 500);
}

// A failed modprobe of v4l2loopback is commonly misread as a permissions
// problem, but on Secure Boot systems the likelier cause is that
// v4l2loopback-dkms built and signed the module but the signing key was
// never enrolled, so the kernel refuses to load it. mokutil reads EFI
// variables and needs no privileges, so it can tell.
static bool secure_boot_mok_pending(void) {
  char sb_state[256];
  char *const sb_argv[] = {"mokutil", "--sb-state", NULL};
  command_output(sb_argv, sb_state, sizeof sb_state);
  if (strstr(sb_state, "SecureBoot enabled") == NULL)
    return false;

  // --list-new exits nonzero on some mokutil versions when nothing is
  // pending, so only its output is trusted.
  char pending[512];
  char *const list_argv[] = {"mokutil", "--list-new", NULL};
  command_output(list_argv, pending, sizeof pending);
  return pending[0] != '\0';
}

static void explain_missing(const VirtualCam *vc) {
  if (!path_exists("/sys/module/v4l2loopback")) {
    printf("Virtual camera: %s does not exist because v4l2loopback is not "
           "loaded. install.sh sets it to load at boot; to load it now:\n"
           "  sudo modprobe v4l2loopback video_nr=10 card_label=VirtualCam "
           "exclusive_caps=1\n",
           vc->path);
    if (secure_boot_mok_pending())
      printf("Virtual camera: Secure Boot is on and a module signing key "
             "is waiting to be enrolled -- if modprobe says \"Operation not "
             "permitted\", that is why, not permissions. Reboot and enroll "
             "it in the blue \"MOK Manager\" screen, using the password set "
             "when v4l2loopback-dkms was installed.\n");
  } else {
    printf("Virtual camera: v4l2loopback is loaded but %s does not exist. "
           "It was probably loaded with a different video_nr; see "
           "/sys/class/video4linux/ for the devices it made, and set "
           "\"output_device\" to match.\n",
           vc->path);
  }
  printf("Virtual camera: carrying on without it, and checking for %s "
         "every %llds.\n",
         vc->path, (long long)(retry_missing_ms / 1000));
}

VirtualCam *virtual_cam_create(const char *path, uint32_t width,
                               uint32_t height) {
  if (!path || !path[0]) {
    printf("Virtual camera: off (\"output_device\" is empty).\n");
    return NULL;
  }
  VirtualCam *vc = (VirtualCam *)calloc(1, sizeof *vc);
  if (!vc)
    return NULL;
  snprintf(vc->path, sizeof vc->path, "%s", path);
  vc->width = width;
  vc->height = height;
  vc->backoff_ms = retry_min_ms;
  return vc;
}

static void try_open(VirtualCam *vc, int64_t now) {
  if (!path_exists(vc->path)) {
    if (!vc->explained_missing) {
      explain_missing(vc);
      vc->explained_missing = true;
    }
    vc->next_attempt_ms = now + retry_missing_ms;
    return;
  }

  vc->open_attempts++;
  vc->out = v4l2_out_open(vc->path, vc->width, vc->height,
                          V4L2_PIX_FMT_YUV422P);

  // The driver negotiates: if something else already fixed the device at
  // another size, every write would be refused. Treat that as a failed
  // open, so it backs off instead of reopening every two seconds.
  const size_t want = (size_t)vc->width * vc->height * 2; // I422
  if (vc->out && v4l2_out_frame_size(vc->out) != want) {
    printf("Virtual camera: %s is set to %zu-byte frames, but this pipeline "
           "makes %ux%u I422 (%zu bytes). Another program may have fixed its "
           "format; it frees up once nothing has it open.\n",
           vc->path, v4l2_out_frame_size(vc->out), vc->width, vc->height,
           want);
    v4l2_out_close(vc->out);
    vc->out = NULL;
  }

  if (!vc->out) {
    printf("Virtual camera: could not open %s (reason above); retrying in "
           "%llds.\n",
           vc->path, (long long)(vc->backoff_ms / 1000));
    vc->next_attempt_ms = now + vc->backoff_ms;
    vc->backoff_ms *= 2;
    if (vc->backoff_ms > retry_max_ms)
      vc->backoff_ms = retry_max_ms;
    return;
  }

  printf("Virtual camera: writing to %s (%ux%u)\n", vc->path, vc->width,
         vc->height);
  vc->backoff_ms = retry_min_ms;
  vc->explained_missing = false; // explain afresh if it vanishes later
}

bool virtual_cam_write(VirtualCam *vc, const VideoFrame *frame) {
  if (!vc)
    return false;

  if (!vc->out) {
    int64_t now = now_ms();
    if (now < vc->next_attempt_ms)
      return false;
    try_open(vc, now);
    if (!vc->out) {
      vc->last_write_ok = false;
      return false;
    }
  }

  vc->last_write_ok = v4l2_out_write(vc->out, frame);
  if (!vc->last_write_ok) {
    printf("Virtual camera: write to %s failed; closing it and retrying in "
           "%llds. Everything else carries on.\n",
           vc->path, (long long)(retry_min_ms / 1000));
    v4l2_out_close(vc->out);
    vc->out = NULL;
    vc->next_attempt_ms = now_ms() + retry_min_ms;
  }
  return vc->last_write_ok;
}

bool virtual_cam_connected(const VirtualCam *vc) {
  return vc && vc->out && vc->last_write_ok;
}

void virtual_cam_destroy(VirtualCam *vc) {
  if (!vc)
    return;
  v4l2_out_close(vc->out);
  free(vc);
}
