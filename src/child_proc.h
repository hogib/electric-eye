#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <sys/types.h>

/*
 * Child processes: spawn, talk to over pipes, stop.
 *
 * eeye delegates to other programs where they do a job better than it
 * could: rpicam-vid for the Pi camera's ISP, mokutil for Secure Boot state,
 * and (in hellion) ffmpeg for H.264 and RTP. This is the one place that
 * knows how to run them safely, because each of the details below was a
 * real way to break things:
 *
 * - **Only stdin, stdout and stderr cross into the child.** Every other
 *   descriptor is closed before exec. A child that inherits another
 *   child's stdin pipe holds it open, so closing that pipe never reaches
 *   the process it was meant for -- an ffmpeg encoder then never sees EOF
 *   and never finalizes its MP4. A child holding a V4L2 device open keeps
 *   it busy after the parent lets go.
 * - **Signals are reset in the child.** eeye ignores SIGPIPE and handles
 *   SIGINT/SIGTERM itself; an ignored signal stays ignored across exec, so
 *   without the reset a child writing to a closed pipe would spin on EPIPE
 *   instead of dying.
 * - **Each child is its own process group.** Ctrl+C at a terminal reaches
 *   the parent alone, which then stops its children in order -- closing an
 *   encoder's stdin first so it can finish its file, rather than every
 *   process in the session being interrupted at once. child_stop()
 *   signals the whole group, so a wrapper script's own children go too.
 * - **A program that cannot be run fails here**, not later: the path is
 *   resolved before fork(), and an exec failure in the child is reported
 *   back to child_spawn() over a close-on-exec pipe. "ffmpeg is not
 *   installed" is one clear error rather than an EOF on the first read.
 *
 * The parent's ends of the pipes are non-blocking and close-on-exec. Use
 * the read/write helpers below, which wait with poll().
 */

typedef struct {
  pid_t pid;  // 0 when no child is running
  int in_fd;  // parent's end of the child's stdin, or -1
  int out_fd; // parent's end of the child's stdout, or -1
  int err_fd; // parent's end of the child's stderr, or -1
} ChildProc;

enum {
  CHILD_PIPE_STDIN = 1u << 0,  // in_fd writes to the child's stdin
  CHILD_PIPE_STDOUT = 1u << 1, // out_fd reads the child's stdout
  // The child's stderr goes into the stdout pipe too (needs
  // CHILD_PIPE_STDOUT). Without it, stderr is inherited, so a child's own
  // error messages land in the parent's log (journald, under systemd).
  CHILD_STDERR_TO_STDOUT = 1u << 2,
  CHILD_QUIET_STDERR = 1u << 3, // the child's stderr goes to /dev/null
  CHILD_NULL_STDIN = 1u << 4,   // /dev/null instead of the parent's stdin
  // err_fd reads the child's stderr separately, for a parent that wants to
  // sort its messages rather than pass them all on (hellion's decoder).
  CHILD_PIPE_STDERR = 1u << 5,
};

/*
 * Starts argv[0] (looked up in PATH unless it contains a '/'), with
 * argv as its arguments. `tag` prefixes any error message.
 *
 * Returns false, with the reason already printed, if the program could not
 * be found or executed. On success `c` describes the running child.
 */
bool child_spawn(ChildProc *c, const char *tag, char *const argv[],
                 unsigned flags);

/*
 * Whether `name` would be found and is executable, without running it or
 * printing anything. For optional helpers: probing for rpicam-vid on a
 * machine that has no Pi camera is normal, not an error.
 */
bool child_program_exists(const char *name);

// Results of the I/O helpers below.
typedef enum {
  CHILD_IO_OK = 0,
  CHILD_IO_TIMEOUT, // the deadline passed first
  CHILD_IO_EOF,     // the other end closed (child exited or closed stdout)
  CHILD_IO_ERROR,   // anything else; errno is set
} ChildIo;

/*
 * Reads up to `cap` bytes, waiting at most timeout_ms for the first of
 * them. On CHILD_IO_OK, *got is at least 1.
 */
ChildIo child_read_some(int fd, void *buf, size_t cap, int timeout_ms,
                        size_t *got);

/*
 * Reads exactly `len` bytes, or fails. timeout_ms bounds the whole read,
 * not each piece of it: "no complete frame within 4s" is what callers
 * care about. *got (if non-NULL) reports how much arrived either way.
 */
ChildIo child_read_exact(int fd, void *buf, size_t len, int timeout_ms,
                         size_t *got);

/*
 * Writes all `len` bytes, or fails. timeout_ms bounds the whole write. A
 * child that has exited shows up as CHILD_IO_EOF (EPIPE), never as a
 * SIGPIPE killing the caller.
 */
ChildIo child_write_all(int fd, const void *buf, size_t len, int timeout_ms);

/*
 * Reads until EOF into `out`, NUL-terminating it, truncating silently once
 * it is full (the rest is drained and discarded so the child is not left
 * blocked on a full pipe). For short command output -- mokutil, rpicam-vid
 * --list-cameras. Returns the bytes stored.
 */
size_t child_read_all(int fd, char *out, size_t cap, int timeout_ms);

/*
 * Raises a pipe's kernel buffer to `bytes`. Best-effort: false where
 * F_SETPIPE_SZ is unavailable or the request is over
 * /proc/sys/fs/pipe-max-size.
 */
bool child_set_pipe_size(int fd, int bytes);

/*
 * Stops the child and reaps it, escalating:
 *   1. If it has a stdin pipe, close it and wait up to grace_ms -- the
 *      polite stop. An encoder finishes its file and exits on its own.
 *   2. Close stdout and give it up to 100ms (capped at grace_ms) to exit
 *      by itself -- a child whose output just hit EOF is usually already
 *      exiting -- then SIGTERM the process group, and wait grace_ms.
 *   3. SIGKILL, and wait for it.
 * Never blocks for longer than about 2 * grace_ms + 100ms plus the reap.
 * To collect a child's natural exit status, child_wait() for it first.
 *
 * Returns the waitpid() status, or -1 if there was no child. Safe on a
 * child that has already exited, and on a zeroed or stopped ChildProc.
 */
int child_stop(ChildProc *c, int grace_ms);

/*
 * Sends `sig` to the child's process group. For a child that needs its own
 * stop: ffmpeg writing an MP4 finalizes it on SIGINT, so a recorder is
 * stopped with child_signal(SIGINT) and child_wait() before child_stop().
 * False if there is no child.
 */
bool child_signal(ChildProc *c, int sig);

/*
 * Waits up to timeout_ms for the child to exit by itself, reaping it.
 * Returns true and fills *status (if non-NULL) once it has; false if it
 * is still running. timeout_ms 0 just checks.
 */
bool child_wait(ChildProc *c, int timeout_ms, int *status);
