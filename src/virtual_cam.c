// _POSIX_C_SOURCE before any header include: -std=c23 puts glibc in strict
// ISO mode, which hides nanosleep() unless a feature-test macro asks for it
// explicitly. Same guard as config.c/eeye.c/v4l2_in.c.
#define _POSIX_C_SOURCE 200809L

#include "virtual_cam.h"
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
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

// Runs argv[0] with its stdout captured into out (NUL-terminated, truncated
// silently if it doesn't fit -- these outputs are a couple lines at most)
// and its stderr discarded. Returns false if the command couldn't even be
// launched or exited nonzero; out is still filled with whatever it printed
// before that, since --list-new below is checked by its output rather than
// its exit code.
static bool capture_command(char *const argv[], char *out, size_t out_cap) {
  int pipefd[2];
  if (pipe(pipefd) != 0)
    return false;

  pid_t pid = fork();
  if (pid < 0) {
    close(pipefd[0]);
    close(pipefd[1]);
    return false;
  }
  if (pid == 0) {
    close(pipefd[0]);
    dup2(pipefd[1], STDOUT_FILENO);
    close(pipefd[1]);
    int devnull = open("/dev/null", O_WRONLY);
    if (devnull >= 0) {
      dup2(devnull, STDERR_FILENO);
      close(devnull);
    }
    execvp(argv[0], argv);
    _exit(127);
  }

  close(pipefd[1]);
  size_t total = 0;
  ssize_t n;
  while (total + 1 < out_cap &&
         (n = read(pipefd[0], out + total, out_cap - 1 - total)) > 0)
    total += (size_t)n;
  out[total] = '\0';
  close(pipefd[0]);

  int status = 0;
  if (waitpid(pid, &status, 0) < 0)
    return false;
  return WIFEXITED(status) && WEXITSTATUS(status) == 0;
}

// A failed modprobe of v4l2loopback commonly gets misread as a plain
// permissions problem ("run as root"), but on Secure Boot systems the far
// more likely cause is that v4l2loopback-dkms built and signed the module
// fine, but the signing key was never enrolled with the firmware -- the
// kernel then refuses to load *any* unenrolled out-of-tree module with the
// exact same EPERM that a missing CAP_SYS_MODULE produces. Distinguishing
// the two would ideally mean reading dmesg for the lockdown/signature
// rejection message, but kernel.dmesg_restrict (the Debian/Ubuntu default)
// blocks that for anything short of full root or CAP_SYSLOG -- exactly the
// case eeye.service runs under (only CAP_SYS_MODULE, via
// AmbientCapabilities). mokutil reads EFI variables instead, which needs no
// special capability, so it works in that same restricted context.
static bool secure_boot_mok_pending(void) {
  char sb_state[256];
  char *const sb_argv[] = {"mokutil", "--sb-state", NULL};
  // mokutil missing (non-Secure-Boot-capable firmware, or just not
  // installed) isn't an error here -- it just means this check can't say
  // anything, so the caller falls back to the generic message.
  if (!capture_command(sb_argv, sb_state, sizeof sb_state))
    return false;
  if (strstr(sb_state, "SecureBoot enabled") == NULL)
    return false;

  char pending[512] = "";
  char *const list_argv[] = {"mokutil", "--list-new", NULL};
  // --list-new exits nonzero on some mokutil versions when nothing is
  // pending, so its exit code isn't trustworthy here -- only its output is.
  capture_command(list_argv, pending, sizeof pending);
  return pending[0] != '\0';
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
      if (secure_boot_mok_pending()) {
        printf(
            "Failed to load v4l2loopback -- and this is not a permissions "
            "problem. Secure Boot is on and `mokutil --list-new` shows a "
            "pending key enrollment: v4l2loopback-dkms built and signed the "
            "module, but the signing key was never enrolled with the "
            "firmware, so the kernel refuses to load it (this shows up as "
            "the same \"Operation not permitted\" a missing CAP_SYS_MODULE "
            "would, which is why it's easy to misdiagnose).\n"
            "Reboot now -- a blue \"MOK Manager\" screen should appear "
            "during boot asking you to enroll it, using the password you "
            "set when v4l2loopback-dkms was installed. If that screen "
            "doesn't appear, or you've lost the password, run `sudo "
            "mokutil --import <path-to-the-key>.der` (commonly under "
            "/var/lib/shim-signed/mok/ or /var/lib/dkms/mok.pub) to queue a "
            "fresh enrollment, then reboot.\n");
      } else {
        printf("Failed to load v4l2loopback. Run as root (or grant CAP_SYS_"
               "MODULE), or load it manually:\n"
               "  sudo modprobe v4l2loopback video_nr=%u card_label=\"%s\" "
               "exclusive_caps=1\n",
               video_nr, card_label);
      }
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
