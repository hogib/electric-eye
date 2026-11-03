#pragma once
/*
 * Names the calling thread for /proc/[pid]/task/[tid]/comm, so `hellion
 * bench`, top(1) and perf show "eeye/effects" rather than four threads all
 * called "eeye". Linux only (so is everything else here: V4L2). At most
 * 15 characters are kept; the rest is cut.
 */
#include <sys/prctl.h>

static inline void thread_set_name(const char *name) {
  prctl(PR_SET_NAME, name, 0, 0, 0);
}
