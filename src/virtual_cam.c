#include "virtual_cam.h"
#include <stdint.h>
#include <stdio.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

static bool loaded_by_us = false;

static bool module_present(void) {
  struct stat st;
  return stat("/sys/module/v4l2loopback", &st) == 0;
}

// modprobe returning success only means the module got inserted -- the
// /dev/videoN node itself is created asynchronously by udev reacting to
// the driver's uevent, which can lag module insertion by tens of
// milliseconds on first load. Poll briefly rather than let that race show
// up as a confusing ENOENT deep inside consumer_loop's v4l2_out_open().
// 2s/50ms was picked generously -- steady-state this returns on the first
// or second check.
static bool wait_for_device(const char *device_path) {
  struct stat st;
  for (int waited_ms = 0; waited_ms < 2000; waited_ms += 50) {
    if (stat(device_path, &st) == 0)
      return true;
    struct timespec ts = {.tv_sec = 0, .tv_nsec = 50000000};
    nanosleep(&ts, NULL);
  }
  return false;
}

// fork+exec rather than system(): argv is built from fixed strings plus a
// caller-controlled card_label, and going through a shell would mean that
// label gets shell-interpreted rather than passed through verbatim.
static bool run_modprobe(char *const argv[]) {
  pid_t pid = fork();
  if (pid < 0) {
    perror("fork for modprobe failed");
    return false;
  }
  if (pid == 0) {
    execvp("modprobe", argv);
    perror("execvp modprobe failed");
    _exit(127);
  }

  int status = 0;
  if (waitpid(pid, &status, 0) < 0) {
    perror("waitpid for modprobe failed");
    return false;
  }
  return WIFEXITED(status) && WEXITSTATUS(status) == 0;
}

bool virtual_cam_ensure_loaded(uint32_t video_nr, const char *card_label,
                               const char *device_path) {
  if (module_present()) {
    printf("v4l2loopback already loaded; leaving its parameters as-is.\n");
  } else {
    char video_nr_arg[32];
    char card_label_arg[128];
    snprintf(video_nr_arg, sizeof video_nr_arg, "video_nr=%u", video_nr);
    snprintf(card_label_arg, sizeof card_label_arg, "card_label=%s",
             card_label);

    char *const argv[] = {
        "modprobe", "v4l2loopback", video_nr_arg, card_label_arg,
        "exclusive_caps=1", NULL,
    };

    printf("Loading v4l2loopback (video_nr=%u, card_label=\"%s\")...\n",
           video_nr, card_label);
    if (!run_modprobe(argv)) {
      printf("Failed to load v4l2loopback. Run as root (or grant CAP_SYS_"
             "MODULE), or load it manually:\n"
             "  sudo modprobe v4l2loopback video_nr=%u card_label=\"%s\" "
             "exclusive_caps=1\n",
             video_nr, card_label);
      return false;
    }

    // Set even though device_path may still fail to appear below: this
    // process is the one that changed the module's loaded state, so it
    // stays responsible for unloading it again regardless of how the rest
    // of this call turns out.
    loaded_by_us = true;
  }

  if (!wait_for_device(device_path)) {
    printf("v4l2loopback is loaded but %s never appeared. If it was "
           "already loaded before this run (not by this process), it may "
           "have a different video_nr than expected -- check with `sudo "
           "modprobe -r v4l2loopback` followed by a fresh run.\n",
           device_path);
    return false;
  }

  return true;
}

void virtual_cam_unload(void) {
  if (!loaded_by_us)
    return;

  char *const argv[] = {"modprobe", "-r", "v4l2loopback", NULL};
  printf("Unloading v4l2loopback...\n");
  if (!run_modprobe(argv)) {
    printf("Failed to unload v4l2loopback (a consumer may still have the "
           "device open); leaving it loaded.\n");
    return;
  }

  loaded_by_us = false;
}
