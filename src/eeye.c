// _POSIX_C_SOURCE before any header include: -std=c23 puts glibc in strict
// ISO mode, which hides write()/STDOUT_FILENO in <unistd.h> unless a
// feature-test macro asks for them explicitly. Same guard as
// video_threads.c/config.c.
#define _POSIX_C_SOURCE 200809L

#include "config.h"
#include "frame_ring_buffer.h"
#include "rpicam_in.h"
#include "v4l2_in.h"
#include "video_frame.h"
#include "video_threads.h"
#include "virtual_cam.h"
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <threads.h>
#include <unistd.h>

// Not declared by <unistd.h> under plain -std=c23 + _POSIX_C_SOURCE (it's
// specified by POSIX as available whenever a conforming environment defines
// it, but glibc's strict-ISO mode still doesn't expose the declaration here
// the way it does write()/STDOUT_FILENO above) -- declare it directly rather
// than pull in _GNU_SOURCE for one symbol.
extern char **environ;

// Geometry now comes from the config file (capture_width/capture_height/
// downscale -- see config.h), read once by config_load_once() in main()
// before any thread starts. The arg structs below therefore leave their
// frame_* fields zero-initialized and main() fills them in; there is
// deliberately no compile-time default here to drift out of sync with
// config_defaults, which is the single source of truth for the fallback.

// virtual_cam_ensure_loaded() is what actually makes virtual_cam_device_path
// exist; cons_args.outpath below points at the same path.
constexpr uint32_t virtual_cam_video_nr = 10;
constexpr char virtual_cam_card_label[] = "VirtualCam";
constexpr char virtual_cam_device_path[] = "/dev/video10";

// Live-preview stream tap (see stream_server.h). Off by default (Config's
// stream_frame_interval starts at 0); this is only the port it listens on
// once a config turns it on.
constexpr uint16_t stream_server_port = 9000;

// Frames in circulation. Not the ring capacity (31): every frame in the
// pool can be queued in front of the virtual camera at once, so this is
// also the worst-case backlog between the camera and what the pilot sees --
// 8 frames is ~270ms at 30fps, where 31 was a full second. When the
// pipeline falls behind, the producer blocks on an empty pool and the
// camera driver drops its oldest frames instead, which is the right trade
// for a live view. Also the memory bill: each frame is three full I422
// buffers, ~5.5MB at 720p.
constexpr unsigned int frame_pool_size = 8;
static_assert(frame_pool_size <= ring_buffer_size - 1,
              "every pooled frame must fit in the free ring at once");

FrameRingBuffer ring_buffer_in;
FrameRingBuffer ring_buffer_out;
FrameRingBuffer ring_buffer_free;
atomic_bool is_running = true;

// atomic_store() on a lock-free type and write() are among the few
// operations POSIX guarantees are safe to call from a signal handler --
// printf is not one of them, since it can deadlock if the signal lands
// while the interrupted code already holds stdio's internal lock. This
// guarantee only holds because atomic_bool is required to be lock-free
// here: a lock-based atomic would mean this handler could call into a
// mutex from arbitrary interrupted context, which is not signal-safe.
static_assert(ATOMIC_BOOL_LOCK_FREE == 2,
             "is_running must be lock-free to set from a signal handler");

static void handle_shutdown_signal(int sig) {
  (void)sig;
  static const char msg[] = "\nShutting down...\n";
  write(STDOUT_FILENO, msg, sizeof(msg) - 1);
  atomic_store(&is_running, false);
}

/*
 * .filename is the V4L2 camera device (MJPEG capture -- see v4l2_in.h).
 * .outpath is the v4l2loopback virtual camera device.
 *
 * ffmpeg is no longer required at build or run time; both ends of the
 * pipeline talk directly to their /dev/videoN devices now.
 *
 * One-time host setup required:
 *   sudo apt install v4l2loopback-dkms libturbojpeg0-dev
 *
 * v4l2loopback itself no longer needs to be modprobed by hand -- see
 * virtual_cam.c, called from main() below -- but loading a kernel module
 * needs CAP_SYS_MODULE, so this binary must run as root (e.g. `sudo
 * ./eeye`) or, under eeye.service, with the AmbientCapabilities=
 * CAP_SYS_MODULE line already set there. Without either, startup fails
 * fast with the equivalent manual `sudo modprobe` command printed.
 *
 * Confirm your real camera's device number with:
 *   v4l2-ctl --list-devices
 */
ProducerArgs prod_args = {.filename = "/dev/video0",
                          .is_running = &is_running,
                          .ring_buffer_in = &ring_buffer_in,
                          .ring_buffer_free = &ring_buffer_free};

// .config is filled in by main() once config_watch_start() has run --
// building the watcher needs is_running, which isn't available until then.
static EffectsTiming effects_timing;

WorkerArgs work_args = {
    .is_running = &is_running,
    .timing = &effects_timing,
    .ring_buffer_in = &ring_buffer_in,
    .ring_buffer_out = &ring_buffer_out,
};

ConsumerArgs cons_args = {
    .outpath = virtual_cam_device_path,
    .is_running = &is_running,
    .ring_buffer_out = &ring_buffer_out,
    .ring_buffer_free = &ring_buffer_free,
    .stream_port = stream_server_port,
};

StatsArgs stats_args = {
    .is_running = &is_running,
    .effects_timing = &effects_timing,
    .ring_buffer_in = &ring_buffer_in,
    .ring_buffer_out = &ring_buffer_out,
    .ring_buffer_free = &ring_buffer_free,
};

// The path main() re-executes. /proc/self/exe works as an execve() target
// too, but the kernel names the new process after the last component of the
// path it was exec'd from, so the daemon would run as "exe": invisible to
// `pgrep eeye` / `pkill eeye` and mislabelled in ps and top. Its readlink()
// target is the binary's real path, which keeps the name ("eeye", or
// "eeye-d" for a debug build). Falls back to /proc/self/exe itself if the
// link can't be read, or if the binary was replaced on disk while running
// (it then reads back as "<path> (deleted)", which names nothing).
static const char *self_exe_path(char *buf, size_t cap) {
  ssize_t n = readlink("/proc/self/exe", buf, cap - 1);
  if (n <= 0 || (size_t)n >= cap - 1)
    return "/proc/self/exe";
  buf[n] = '\0';
  if (strstr(buf, " (deleted)"))
    return "/proc/self/exe";
  return buf;
}

int main(int argc, char **argv) {
  // libgomp's default OMP_WAIT_POLICY has each worker thread in a #pragma
  // omp parallel team (conv.c, point_opps.c) busy-spin for a few ms after
  // every region, on the assumption regions run back-to-back. Here frames
  // are ~33ms apart, so every worker spent most of each frame interval
  // spinning -- measured at ~178% CPU idling down to ~15-19% once
  // OMP_WAIT_POLICY=passive is set, no other code change.
  //
  // A bounded spin (GOMP_SPINCOUNT) looked like it should do better still,
  // by keeping workers awake across one frame's back-to-back regions. It
  // measured worse on every count: with the default blur -> sobel chain on
  // a 12-core host, GOMP_SPINCOUNT=10000 cost 55% CPU and 2.0-2.3ms per
  // frame of effects time, against 27% and 1.3ms for passive. Spinning
  // workers compete for cores with the capture and output threads, and the
  // more the host has going on the worse that gets. So: passive. An
  // operator's own OMP_WAIT_POLICY or GOMP_SPINCOUNT is left alone, which
  // makes either easy to try on real hardware.
  //
  // It cannot be set from inside this process after it has started:
  // verified empirically that neither setenv() here nor an early
  // __attribute__((constructor(101))) -- about as soon as user code is
  // allowed to run -- changes libgomp's behavior. Whatever decides this is
  // settled before any code in this process gets a chance to run.
  //
  // So: re-exec once, with the variable set, before doing anything else.
  // execve() replaces the process image outright, which re-runs every
  // constructor (including libgomp's) against the now-updated environment.
  // The target is this binary's real path, from /proc/self/exe -- the
  // kernel's own resolved path, which sidesteps the usual argv[0]-may-be-
  // relative-or-bare problem that execve() (unlike execvp()) can't otherwise
  // handle. See self_exe_path() for why not /proc/self/exe itself. Skipped
  // when the
  // operator set either variable themselves (e.g. eeye.service's
  // Environment= line), and the re-exec'd image inherits the variable it
  // just set, so this only ever happens once.
  if (!getenv("GOMP_SPINCOUNT") && !getenv("OMP_WAIT_POLICY")) {
    setenv("OMP_WAIT_POLICY", "passive", 1);
    printf("Re-executing with OMP_WAIT_POLICY=passive (see eeye.c main() "
           "for why this can't be done in-process)...\n");
    // execve() discards this process image, stdio buffer included, before
    // anything forces a buffered line out to the fd -- without this flush
    // the message above is silently lost every single time, on a redirected
    // file exactly as much as under journald's own pipe-backed capture.
    fflush(stdout);
    char exe_buf[4096];
    execve(self_exe_path(exe_buf, sizeof exe_buf), argv, environ);
    // Only reached if execve() itself failed to start (e.g. /proc not
    // mounted) -- exec never returns on success. Degrade rather than abort:
    // the pipeline is still correct, it will just idle at a higher CPU cost
    // than it should.
    perror("Re-exec for OMP_WAIT_POLICY failed; continuing without it");
  }

  // Nothing in this pipeline talks to a pipe or socket anymore (both
  // producer_loop and consumer_loop are direct V4L2 device I/O now), but
  // stdout can still be one if the user runs `./eeye | something` and that
  // reader exits -- including the signal handler's own write() below.
  // Ignoring SIGPIPE means that shows up as a normal write() failure
  // instead of killing the process outright.
  signal(SIGPIPE, SIG_IGN);

  // SIGINT: Ctrl+C from an interactive terminal. SIGTERM: what a service
  // manager (systemd, etc.) sends on stop/restart -- handling both the same
  // way means "graceful exit" holds whether this runs at a terminal or as a
  // deployed service. Both threads and the config watcher already poll
  // is_running on a bounded interval, so flipping it is the entire job; the
  // existing join/close/free sequence below is unchanged and runs exactly
  // as it does on any other shutdown path.
  signal(SIGINT, handle_shutdown_signal);
  signal(SIGTERM, handle_shutdown_signal);

  const char *config_path = (argc > 1) ? argv[1] : "eeye_config.json";

  // Geometry has to be settled before anything is sized from it -- the
  // frame pool below, both V4L2 format negotiations, and every arg struct.
  // config_watch_start() further down re-reads the same file for the
  // hot-reloadable fields; this earlier read exists purely because the
  // watcher's first publish happens after the pool already needs a size.
  Config startup_config;
  if (!config_load_once(config_path, &startup_config)) {
    printf("Error: %s is not usable (see the reason above); refusing to "
           "start on a guessed geometry.\n",
           config_path);
    return 1;
  }

  uint32_t capture_width = startup_config.capture_width;
  uint32_t capture_height = startup_config.capture_height;

  // Settle CAPTURE_AUTO here, once, rather than in producer_loop: the
  // reconnect loop there runs repeatedly, and re-probing on every retry
  // would let the backend silently change mid-session (e.g. a USB webcam
  // unplugged, then a CSI camera picked up on the next retry) with the
  // frame pool already sized for the first one.
  CaptureSource capture_source = startup_config.capture_source;
  if (capture_source == CAPTURE_AUTO) {
    capture_source = rpicam_in_available() ? CAPTURE_RPICAM : CAPTURE_V4L2;
    printf("Capture source: auto-detected %s\n",
           capture_source == CAPTURE_RPICAM
               ? "Pi camera module (rpicam-vid)"
               : "V4L2 device");
  }
  // config.c already refuses an explicit v4l2 source with yuv420; this
  // catches "auto" resolving to V4L2.
  if (capture_source == CAPTURE_V4L2 &&
      startup_config.capture_format == FORMAT_YUV420) {
    printf("Error: \"capture_format\": \"yuv420\" is only for the Pi camera "
           "(rpicam) backend, but no Pi camera was found. Use \"auto\", "
           "\"mjpeg\" or \"yuyv\" for a V4L2 camera.\n");
    return 1;
  }

  // Ask the camera what it can actually do before anything is sized from
  // the answer. Advisory only: if the camera isn't plugged in yet this
  // leaves the configured size alone and producer_loop's reconnect loop
  // takes over, so startup without a camera keeps working. Done here
  // rather than inside v4l2_in_open because the frame pool below is
  // allocated once, up front, and every frame in it has to match whatever
  // size the camera is eventually opened at -- by the time v4l2_in_open
  // runs (on the producer thread) that size is already committed.
  //
  // V4L2 only: the rpicam backend has no device node to enumerate against
  // (see rpicam_in.h), and rpicam-vid's ISP scales to whatever geometry it
  // is asked for rather than offering a fixed mode list, so there is
  // nothing to negotiate.
  if (capture_source == CAPTURE_V4L2) {
    v4l2_in_negotiate_size(prod_args.filename, &capture_width, &capture_height,
                           startup_config.downscale,
                           startup_config.capture_format);
  }

  uint32_t out_width = capture_width / startup_config.downscale;
  uint32_t out_height = capture_height / startup_config.downscale;

  prod_args.capture_source = capture_source;
  prod_args.capture_format = startup_config.capture_format;
  prod_args.capture_width = capture_width;
  prod_args.capture_height = capture_height;
  prod_args.downscale = startup_config.downscale;
  prod_args.frame_width = out_width;
  prod_args.frame_height = out_height;
  work_args.frame_width = out_width;
  work_args.frame_height = out_height;
  cons_args.frame_width = out_width;
  cons_args.frame_height = out_height;

  // Reports the negotiated capture size, not the configured one -- they
  // differ whenever v4l2_in_negotiate_size() substituted a mode, and this
  // line is the one place an operator checks to see what the run is
  // actually doing.
  if (startup_config.downscale > 1) {
    printf("Geometry: capturing %ux%u, downscaling %ux -> pipeline runs at "
           "%ux%u\n",
           capture_width, capture_height, startup_config.downscale, out_width,
           out_height);
  } else {
    printf("Geometry: %ux%u, no downscaling\n", out_width, out_height);
  }

  // Loads v4l2loopback (if not already loaded) so the one-time `sudo
  // modprobe ...` from this file's header comment no longer has to be run
  // by hand before every launch. atexit() -- rather than a call at the
  // bottom of main() -- covers every return path below uniformly,
  // including the early failure returns that follow. Registered before
  // the call it guards (not after): virtual_cam_ensure_loaded() can load
  // the module and then still fail (device node never appeared) -- in
  // which case it has already set the "we loaded it" state that
  // virtual_cam_unload() checks, so cleanup must still run on the
  // immediate `return 1` below. virtual_cam_unload() itself is a no-op
  // whenever that state wasn't set, so registering it unconditionally
  // here is always safe.
  atexit(virtual_cam_unload);
  if (!virtual_cam_ensure_loaded(virtual_cam_video_nr, virtual_cam_card_label,
                                 virtual_cam_device_path))
    return 1;

  printf("Initializing pipeline...\n");
  ring_init(&ring_buffer_in);
  ring_init(&ring_buffer_out);
  ring_init(&ring_buffer_free);

  const unsigned int pool_size = frame_pool_size;
  VideoFrame **pool = vf_pool_create(pool_size, out_width, out_height);
  if (!pool) {
    printf("Error: Failed to allocate frame pool.\n");
    return 1;
  }

  // ring_push_wait, not the plain ring_push: the latter only updates the
  // lock-free head/tail queue and never touches the filled/empty
  // semaphores ring_pop_wait actually waits on, which left every one of
  // these frames invisible to producer_loop -- the ring held them, but
  // nothing was ever posted to say so. NULL is safe here (never give up
  // rather than abandon a frame) since is_running is already true and
  // there is always enough room: the static_assert on frame_pool_size
  // keeps it within empty's initial capacity.
  for (unsigned int i = 0; i < pool_size; ++i) {
    ring_push_wait(&ring_buffer_free, pool[i], NULL);
  }

  atomic_store(&is_running, true);

  ConfigWatcher *config_watcher = config_watch_start(config_path, &is_running);
  if (!config_watcher) {
    printf("Error: Failed to start config watcher.\n");
    return 1;
  }
  work_args.config = config_watcher;
  prod_args.config = config_watcher;
  cons_args.config = config_watcher;
  cons_args.config_path = config_path;

  pthread_t producer, worker, consumer, stats;
  if (pthread_create(&producer, NULL, producer_loop, &prod_args) != 0) {
    printf("Error: Failed to create producer thread.\n");
    return 1;
  }
  if (pthread_create(&worker, NULL, effects_loop, &work_args) != 0) {
    printf("Error: Failed to create worker thread.\n");
    return 1;
  }
  if (pthread_create(&consumer, NULL, consumer_loop, &cons_args) != 0) {
    printf("Error: Failed to create consumer thread.\n");
    return 1;
  }
  // Diagnostic only: a failure here doesn't affect the actual pipeline, so
  // it logs and carries on running without stats rather than exiting.
  bool stats_started = pthread_create(&stats, NULL, stats_loop, &stats_args) == 0;
  if (!stats_started) {
    printf("Warning: failed to create stats thread; ring diagnostics "
           "disabled.\n");
  }

  printf("Pipeline running. Capturing from camera -> virtual cam...\n");
  printf("Point another app (browser, Zoom, etc.) at the virtual camera "
         "device to view the live feed.\n");

  pthread_join(producer, NULL);
  pthread_join(worker, NULL);
  pthread_join(consumer, NULL);
  if (stats_started)
    pthread_join(stats, NULL);

  printf("Processing complete. Pipeline shut down cleanly.\n");

  // One definitive snapshot of each ring's lifetime peak occupancy and
  // stall counts, now that every producer/consumer of them has stopped --
  // the real numbers the ring-tuning question needed, not an estimate.
  printf("Final ring stats:\n");
  RingStats s;
  ring_get_stats(&ring_buffer_in, &s);
  printf("  in   (producer->effects) peak %zu/%zu  full_stalls %llu  "
        "empty_stalls %llu\n",
        s.high_water, s.capacity, (unsigned long long)s.full_stalls,
        (unsigned long long)s.empty_stalls);
  ring_get_stats(&ring_buffer_out, &s);
  printf("  out  (effects->consumer) peak %zu/%zu  full_stalls %llu  "
        "empty_stalls %llu\n",
        s.high_water, s.capacity, (unsigned long long)s.full_stalls,
        (unsigned long long)s.empty_stalls);
  ring_get_stats(&ring_buffer_free, &s);
  printf("  free (pool)              peak %zu/%zu  full_stalls %llu  "
        "empty_stalls %llu\n",
        s.high_water, s.capacity, (unsigned long long)s.full_stalls,
        (unsigned long long)s.empty_stalls);

  config_watch_stop(config_watcher);
  vf_pool_free(pool, pool_size);
  ring_destroy(&ring_buffer_in);
  ring_destroy(&ring_buffer_out);
  ring_destroy(&ring_buffer_free);
  return 0;
}
