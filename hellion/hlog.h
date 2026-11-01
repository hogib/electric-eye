#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/*
 * Keeps the last lines hellion printed, for `hellion top`.
 *
 * Everything that reaches stdout or stderr -- hellion's own messages, the
 * shared modules' (fbus, v4l2_in, virtual_cam ...), and the stderr of every
 * child process, ffmpeg included -- goes through a pipe that a thread
 * reads. The thread passes every byte on to where stdout originally went
 * (the journal, under systemd) and keeps the last hlog_lines lines,
 * timestamped. Nothing is filtered or lost on the way to the journal.
 */

enum { hlog_lines = 64, hlog_line_len = 160 };

// Starts capturing. False (and no capture) if the pipe or thread could not
// be set up; output then goes straight to the journal as before.
bool hlog_start(void);

// Copies up to `max` of the most recent lines, oldest first, and returns
// how many. *total (if non-NULL) is how many lines were ever logged.
size_t hlog_snapshot(char (*lines)[hlog_line_len], size_t max,
                     uint64_t *total);

// Restores stdout/stderr and stops the thread once everything written so
// far has been passed on. Call after child processes have stopped.
void hlog_stop(void);
