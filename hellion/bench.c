// `hellion bench`: what eeye and hellion cost while they run, measured from
// outside -- CPU per process and per thread, memory, the Pi's temperature,
// clock and throttling flags, the tether's traffic, and the frame rates
// these resources bought -- sampled once a second for a while and summed
// up. Plain text so it works over SSH and can be pasted into an issue;
// --csv keeps every sample for a spreadsheet.
//
// Everything comes from /proc and /sys, which Linux keeps per process and
// per thread without the programs' cooperation, so it measures the release
// binaries as installed, not an instrumented build. The processes it
// follows are every `eeye` and `hellion` (except this one) and everything
// they started: ffmpeg encoding the tether's video, rpicam-vid feeding the
// Pi camera. Each is charged to the eeye or hellion that started it.
#define _POSIX_C_SOURCE 200809L

#include "bench.h"
#include "fbus.h"
#include "hstatus.h"
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <glob.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

// ---- Limits ----------------------------------------------------------------

enum {
  max_procs = 64,      // eeye, hellion, a few ffmpegs: 5-10 in practice
  max_threads = 1024,  // ffmpeg alone spawns 10-20
  max_streams = 16,
  max_instances = 8,
  max_endpoints = 64,  // pipeline sources and sinks across all instances
  max_ifaces = 16,
  max_cores = 64,
  comm_len = 32,
};

// ---- Small readers ---------------------------------------------------------

// Reads at most cap-1 bytes of a small /proc or /sys file into buf, NUL
// terminated. false if it cannot be opened or is empty.
static bool read_small(const char *path, char *buf, size_t cap) {
  int fd = open(path, O_RDONLY);
  if (fd < 0)
    return false;
  size_t len = 0;
  for (;;) {
    ssize_t n = read(fd, buf + len, cap - 1 - len);
    if (n < 0 && errno == EINTR)
      continue;
    if (n <= 0)
      break;
    len += (size_t)n;
    if (len >= cap - 1)
      break;
  }
  close(fd);
  buf[len] = '\0';
  return len > 0;
}

static void chomp(char *s) {
  size_t n = strlen(s);
  while (n > 0 && (s[n - 1] == '\n' || s[n - 1] == '\r' || s[n - 1] == ' '))
    s[--n] = '\0';
}

static uint64_t now_ns(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

// ---- /proc/[pid]/stat and /proc/[pid]/task/[tid]/stat ----------------------

// The fields of a stat line this needs. The comm field is "(name)" and the
// name may itself contain spaces and parentheses ("(Web Content)"), so
// parsing starts after the LAST ')' -- the only safe way.
typedef struct {
  char comm[comm_len];
  char state;
  int ppid;
  uint64_t ticks;    // utime + stime, in clock ticks
  long num_threads;
  long rss_pages;
} ProcStat;

static bool parse_stat(const char *line, ProcStat *out) {
  memset(out, 0, sizeof *out);
  const char *open = strchr(line, '(');
  const char *close = strrchr(line, ')');
  if (!open || !close || close < open)
    return false;
  size_t n = (size_t)(close - open - 1);
  if (n >= sizeof out->comm)
    n = sizeof out->comm - 1;
  memcpy(out->comm, open + 1, n);
  out->comm[n] = '\0';

  // After ")": state ppid pgrp session tty tpgid flags minflt cminflt
  // majflt cmajflt utime stime cutime cstime priority nice num_threads
  // itrealvalue starttime vsize rss ...   (fields 3 through 24). Every
  // field is read as a signed number -- priority and nice are negative
  // sometimes -- except the state letter, which is taken as is.
  const char *p = close + 1;
  while (*p == ' ')
    p++;
  if (!*p)
    return false;
  char state = *p++;
  long long vals[25] = {0};
  int field = 4;
  while (*p && field <= 24) {
    while (*p == ' ')
      p++;
    if (!*p)
      break;
    char *end;
    vals[field] = strtoll(p, &end, 10);
    if (end == p)
      return false;
    p = end;
    field++;
  }
  if (field <= 24)
    return false;
  int ppid = (int)vals[4];
  unsigned long long utime = (unsigned long long)vals[14];
  unsigned long long stime = (unsigned long long)vals[15];
  long threads = (long)vals[20];
  long rss = (long)vals[24];

  out->state = state;
  out->ppid = ppid;
  out->ticks = utime + stime;
  out->num_threads = threads;
  out->rss_pages = rss;
  return true;
}

// ---- What is followed -------------------------------------------------------

typedef struct {
  int pid;
  int ppid;
  int root;            // the eeye/hellion pid this is charged to
  char comm[comm_len];
  char parent_comm[comm_len]; // for "ffmpeg <- hellion"
  bool seen;           // this sample
  bool exited;         // was followed, then went away
  int samples;         // samples it was seen in
  uint64_t ticks_prev; // utime+stime at the previous sample
  bool have_prev;
  double cpu_sum;      // sum of per-sample cpu% (for the average)
  double cpu_max;
  double rss_mb;       // latest
  double rss_sum;
  double rss_max;
  double hwm_mb;       // VmHWM: the most it ever held, from the kernel
  uint64_t ctxt_prev;  // voluntary + nonvoluntary context switches
  uint64_t ctxt_delta_sum;
  bool have_ctxt;
  uint64_t io_prev;    // write_bytes from /proc/pid/io
  uint64_t io_sum;
  int io_state;        // 0 untried, 1 readable, -1 permission denied
} Proc;

typedef struct {
  int tid;
  int pid;
  char name[comm_len];
  bool seen;
  int samples;
  uint64_t ticks_prev;
  bool have_prev;
  double cpu_sum;
  double cpu_max;
} Thread;

typedef struct {
  char name[fbus_max_stream_name];
  uint64_t frames_prev;
  bool have_prev;
  double fps_sum;
  double fps_min, fps_max;
  int samples;
  uint32_t width, height;
} Stream;

typedef struct {
  char instance[hs_name_len];
  char pipeline[hs_name_len];
  char spec[hs_spec_len];
  bool is_sink;
  bool net;
  uint64_t frames_prev, dropped_prev;
  bool have_prev;
  uint64_t frames_delta, dropped_delta;
  double kbps_sum, kbps_max, loss_sum, loss_max;
  int samples;
} Endpoint;

typedef struct {
  char name[32];
  uint64_t rx_prev, tx_prev;
  bool have_prev;
  double rx_sum, tx_sum, rx_max, tx_max; // kB/s
  int samples;
} Iface;

typedef struct {
  // Configuration
  int seconds;
  FILE *csv;
  int self_pid;

  // Tables
  Proc procs[max_procs];
  int proc_count;
  Thread threads[max_threads];
  int thread_count;
  Stream streams[max_streams];
  int stream_count;
  Endpoint endpoints[max_endpoints];
  int endpoint_count;
  Iface ifaces[max_ifaces];
  int iface_count;

  // System
  long clk_tck;
  int cores;
  uint64_t cpu_busy_prev, cpu_total_prev;
  uint64_t core_busy_prev[max_cores], core_total_prev[max_cores];
  bool have_cpu_prev;
  double sys_cpu_sum, sys_cpu_max;
  double core_cpu_sum[max_cores];
  double tracked_cpu_sum, tracked_cpu_max;
  double tracked_rss_sum, tracked_rss_max;
  double mem_total_mb, mem_avail_min_mb, mem_avail_sum;
  double temp_min, temp_max, temp_sum;
  bool have_temp;
  double mhz_min, mhz_max, mhz_sum;
  bool have_mhz;
  unsigned throttled_or;   // every "now" bit seen during the bench
  unsigned throttled_last; // the whole word at the end (has-occurred bits)
  bool have_throttled;
  char throttled_path[256];
  char temp_path[256];
  double load1;
  int samples;
  double elapsed_s;
  bool io_denied;
} Bench;

static volatile sig_atomic_t stop_requested = 0;

static void on_signal(int sig) {
  (void)sig;
  stop_requested = 1;
}

static Proc *find_proc(Bench *b, int pid) {
  for (int i = 0; i < b->proc_count; i++)
    if (b->procs[i].pid == pid)
      return &b->procs[i];
  return NULL;
}

static Thread *find_thread(Bench *b, int tid) {
  for (int i = 0; i < b->thread_count; i++)
    if (b->threads[i].tid == tid)
      return &b->threads[i];
  return NULL;
}

static bool is_root_comm(const char *comm) {
  return strcmp(comm, "eeye") == 0 || strcmp(comm, "eeye-d") == 0 ||
         strcmp(comm, "hellion") == 0 || strcmp(comm, "hellion-d") == 0;
}

// Everything in /proc, as (pid, ppid, comm): enough to decide who is
// followed. Kept small; a Pi has a few hundred processes.
typedef struct {
  int pid, ppid;
  char comm[comm_len];
  ProcStat st;
} Seen;

static int scan_processes(Seen *seen, int cap) {
  DIR *d = opendir("/proc");
  if (!d)
    return 0;
  int n = 0;
  struct dirent *e;
  while ((e = readdir(d)) && n < cap) {
    if (!isdigit((unsigned char)e->d_name[0]))
      continue;
    char path[320], buf[1024];
    snprintf(path, sizeof path, "/proc/%s/stat", e->d_name);
    if (!read_small(path, buf, sizeof buf))
      continue;
    ProcStat st;
    if (!parse_stat(buf, &st))
      continue;
    seen[n].pid = atoi(e->d_name);
    seen[n].ppid = st.ppid;
    snprintf(seen[n].comm, sizeof seen[n].comm, "%s", st.comm);
    seen[n].st = st;
    n++;
  }
  closedir(d);
  return n;
}

// The eeye/hellion ancestor of pid, or 0. Walks ppid up through the
// snapshot; stops at init, at a loop, or after a sane depth.
static int root_of(const Seen *seen, int n, int pid, int self_pid) {
  for (int depth = 0; depth < 32 && pid > 1; depth++) {
    const Seen *s = NULL;
    for (int i = 0; i < n; i++)
      if (seen[i].pid == pid) {
        s = &seen[i];
        break;
      }
    if (!s)
      return 0;
    if (pid != self_pid && is_root_comm(s->comm))
      return pid;
    pid = s->ppid;
  }
  return 0;
}

static double mb_from_status_line(const char *status, const char *key) {
  const char *p = strstr(status, key);
  if (!p)
    return 0;
  p += strlen(key);
  return (double)strtoull(p, NULL, 10) / 1024.0;
}

static uint64_t u64_from_status_line(const char *status, const char *key,
                                     bool *found) {
  const char *p = strstr(status, key);
  if (!p) {
    *found = false;
    return 0;
  }
  *found = true;
  return strtoull(p + strlen(key), NULL, 10);
}

static void sample_threads(Bench *b, Proc *p, double dt) {
  char dir[64];
  snprintf(dir, sizeof dir, "/proc/%d/task", p->pid);
  DIR *d = opendir(dir);
  if (!d)
    return;
  struct dirent *e;
  while ((e = readdir(d))) {
    if (!isdigit((unsigned char)e->d_name[0]))
      continue;
    char path[400], buf[1024];
    snprintf(path, sizeof path, "%s/%s/stat", dir, e->d_name);
    if (!read_small(path, buf, sizeof buf))
      continue;
    ProcStat st;
    if (!parse_stat(buf, &st))
      continue;
    int tid = atoi(e->d_name);
    Thread *t = find_thread(b, tid);
    if (!t) {
      if (b->thread_count >= max_threads)
        continue;
      t = &b->threads[b->thread_count++];
      memset(t, 0, sizeof *t);
      t->tid = tid;
      t->pid = p->pid;
    }
    // The name may change after the thread starts (prctl); keep the latest.
    snprintf(t->name, sizeof t->name, "%s", st.comm);
    t->seen = true;
    if (t->have_prev && dt > 0) {
      double cpu = 100.0 * (double)(st.ticks - t->ticks_prev) /
                   ((double)b->clk_tck * dt);
      t->cpu_sum += cpu;
      if (cpu > t->cpu_max)
        t->cpu_max = cpu;
      t->samples++;
      if (b->csv)
        fprintf(b->csv, "%.1f,thread,%s,%d,%.2f,\n", b->elapsed_s, t->name,
                tid, cpu);
    }
    t->ticks_prev = st.ticks;
    t->have_prev = true;
  }
  closedir(d);
}

static double sample_processes(Bench *b, double dt, double *rss_total) {
  static Seen seen[2048];
  int n = scan_processes(seen, 2048);
  for (int i = 0; i < b->proc_count; i++)
    b->procs[i].seen = false;
  for (int i = 0; i < b->thread_count; i++)
    b->threads[i].seen = false;

  double cpu_total = 0;
  *rss_total = 0;
  long page_kb = sysconf(_SC_PAGESIZE) / 1024;
  for (int i = 0; i < n; i++) {
    int root = root_of(seen, n, seen[i].pid, b->self_pid);
    if (!root)
      continue;
    // Kernel threads and zombies cost nothing worth charging.
    if (seen[i].st.state == 'Z')
      continue;
    Proc *p = find_proc(b, seen[i].pid);
    if (!p) {
      if (b->proc_count >= max_procs)
        continue;
      p = &b->procs[b->proc_count++];
      memset(p, 0, sizeof *p);
      p->pid = seen[i].pid;
      p->ppid = seen[i].ppid;
      p->root = root;
      snprintf(p->comm, sizeof p->comm, "%s", seen[i].comm);
      for (int k = 0; k < n; k++)
        if (seen[k].pid == seen[i].ppid)
          snprintf(p->parent_comm, sizeof p->parent_comm, "%s", seen[k].comm);
    }
    p->seen = true;
    p->rss_mb = (double)seen[i].st.rss_pages * (double)page_kb / 1024.0;

    char path[64];
    static char status[4096];
    snprintf(path, sizeof path, "/proc/%d/status", p->pid);
    if (read_small(path, status, sizeof status)) {
      double hwm = mb_from_status_line(status, "VmHWM:");
      if (hwm > p->hwm_mb)
        p->hwm_mb = hwm;
      bool f1, f2;
      uint64_t ctxt = u64_from_status_line(status, "voluntary_ctxt_switches:", &f1) +
                      u64_from_status_line(status, "nonvoluntary_ctxt_switches:", &f2);
      if (f1 && f2) {
        if (p->have_ctxt)
          p->ctxt_delta_sum += ctxt - p->ctxt_prev;
        p->ctxt_prev = ctxt;
        p->have_ctxt = true;
      }
    }

    // Bytes the process had the kernel write out (recordings). Readable
    // only by its owner or root: noted once, never fatal.
    if (p->io_state >= 0) {
      static char io[512];
      snprintf(path, sizeof path, "/proc/%d/io", p->pid);
      if (read_small(path, io, sizeof io)) {
        const char *w = strstr(io, "write_bytes:");
        if (w) {
          uint64_t bytes = strtoull(w + 12, NULL, 10);
          if (p->io_state == 1)
            p->io_sum += bytes - p->io_prev;
          p->io_prev = bytes;
          p->io_state = 1;
        }
      } else if (errno == EACCES || errno == EPERM) {
        p->io_state = -1;
        b->io_denied = true;
      }
    }

    double cpu = 0;
    if (p->have_prev && dt > 0) {
      cpu = 100.0 * (double)(seen[i].st.ticks - p->ticks_prev) /
            ((double)b->clk_tck * dt);
      p->cpu_sum += cpu;
      if (cpu > p->cpu_max)
        p->cpu_max = cpu;
      p->rss_sum += p->rss_mb;
      if (p->rss_mb > p->rss_max)
        p->rss_max = p->rss_mb;
      p->samples++;
      if (b->csv)
        fprintf(b->csv, "%.1f,process,%s,%d,%.2f,%.1f\n", b->elapsed_s,
                p->comm, p->pid, cpu, p->rss_mb);
    }
    p->ticks_prev = seen[i].st.ticks;
    p->have_prev = true;
    cpu_total += cpu;
    *rss_total += p->rss_mb;

    sample_threads(b, p, dt);
  }
  for (int i = 0; i < b->proc_count; i++)
    if (!b->procs[i].seen)
      b->procs[i].exited = true;
  return cpu_total;
}

// ---- The machine ------------------------------------------------------------

// /proc/stat's "cpu" lines: busy and total ticks, overall and per core.
static bool read_cpu_ticks(uint64_t *busy, uint64_t *total,
                           uint64_t core_busy[], uint64_t core_total[],
                           int *cores) {
  static char buf[16384];
  if (!read_small("/proc/stat", buf, sizeof buf))
    return false;
  *cores = 0;
  bool have_total = false;
  const char *line = buf;
  while (line && *line) {
    const char *next = strchr(line, '\n');
    if (strncmp(line, "cpu", 3) == 0) {
      unsigned long long v[8] = {0};
      char label[16];
      int got = sscanf(line, "%15s %llu %llu %llu %llu %llu %llu %llu %llu",
                       label, &v[0], &v[1], &v[2], &v[3], &v[4], &v[5], &v[6],
                       &v[7]);
      if (got >= 5) {
        // user nice system idle iowait irq softirq steal
        uint64_t idle = v[3] + v[4];
        uint64_t all = 0;
        for (int i = 0; i < 8; i++)
          all += v[i];
        if (strcmp(label, "cpu") == 0) {
          *busy = all - idle;
          *total = all;
          have_total = true;
        } else if (*cores < max_cores) {
          core_busy[*cores] = all - idle;
          core_total[*cores] = all;
          (*cores)++;
        }
      }
    }
    line = next ? next + 1 : NULL;
  }
  return have_total;
}

static void read_meminfo(double *total_mb, double *avail_mb) {
  static char buf[8192];
  *total_mb = *avail_mb = 0;
  if (!read_small("/proc/meminfo", buf, sizeof buf))
    return;
  *total_mb = mb_from_status_line(buf, "MemTotal:");
  *avail_mb = mb_from_status_line(buf, "MemAvailable:");
  if (*avail_mb == 0) // kernels before 3.14
    *avail_mb = mb_from_status_line(buf, "MemFree:") +
                mb_from_status_line(buf, "Cached:");
}

// The first thermal zone that looks like the CPU's, else zone 0. Pis call
// it "cpu-thermal"; x86 laptops "x86_pkg_temp" or "acpitz".
static void find_temp_path(char *out, size_t cap) {
  out[0] = '\0';
  glob_t g;
  if (glob("/sys/class/thermal/thermal_zone*/type", 0, NULL, &g) != 0)
    return;
  const char *fallback = NULL;
  for (size_t i = 0; i < g.gl_pathc; i++) {
    char type[64];
    if (!read_small(g.gl_pathv[i], type, sizeof type))
      continue;
    chomp(type);
    bool cpu = strstr(type, "cpu") || strstr(type, "x86_pkg") ||
               strstr(type, "soc");
    if (cpu) {
      snprintf(out, cap, "%s", g.gl_pathv[i]);
      break;
    }
    if (!fallback)
      fallback = g.gl_pathv[i];
  }
  if (!out[0] && fallback)
    snprintf(out, cap, "%s", fallback);
  // ".../type" -> ".../temp"
  size_t n = strlen(out);
  if (n >= 4)
    memcpy(out + n - 4, "temp", 4);
  globfree(&g);
}

// The Pi firmware's throttle word, where the kernel exposes it.
static void find_throttled_path(char *out, size_t cap) {
  out[0] = '\0';
  static const char *const patterns[] = {
      "/sys/devices/platform/soc/*firmware*/get_throttled",
      "/sys/devices/platform/*firmware*/get_throttled",
      "/sys/devices/platform/*/*firmware*/get_throttled",
  };
  for (size_t i = 0; i < sizeof patterns / sizeof patterns[0] && !out[0]; i++) {
    glob_t g;
    if (glob(patterns[i], 0, NULL, &g) == 0 && g.gl_pathc > 0)
      snprintf(out, cap, "%s", g.gl_pathv[0]);
    globfree(&g);
  }
}

static bool read_double_file(const char *path, double *out, double scale) {
  char buf[64];
  if (!path[0] || !read_small(path, buf, sizeof buf))
    return false;
  *out = strtod(buf, NULL) * scale;
  return true;
}

static void sample_system(Bench *b, double tracked_cpu, double tracked_rss) {
  uint64_t busy, total, cb[max_cores], ct[max_cores];
  int cores = 0;
  double sys_cpu = -1;
  if (read_cpu_ticks(&busy, &total, cb, ct, &cores)) {
    if (b->have_cpu_prev && total > b->cpu_total_prev) {
      // As a share of ONE core, like the per-process figures, so the
      // numbers add up: 4 busy cores read 400%.
      sys_cpu = 100.0 * (double)(busy - b->cpu_busy_prev) /
                (double)(total - b->cpu_total_prev) * cores;
      b->sys_cpu_sum += sys_cpu;
      if (sys_cpu > b->sys_cpu_max)
        b->sys_cpu_max = sys_cpu;
      for (int i = 0; i < cores && i < b->cores; i++)
        if (ct[i] > b->core_total_prev[i])
          b->core_cpu_sum[i] += 100.0 * (double)(cb[i] - b->core_busy_prev[i]) /
                                (double)(ct[i] - b->core_total_prev[i]);
    }
    b->cpu_busy_prev = busy;
    b->cpu_total_prev = total;
    memcpy(b->core_busy_prev, cb, sizeof cb);
    memcpy(b->core_total_prev, ct, sizeof ct);
    b->cores = cores;
    b->have_cpu_prev = true;
  }

  double mem_total, mem_avail;
  read_meminfo(&mem_total, &mem_avail);
  b->mem_total_mb = mem_total;

  double temp = 0, khz = 0;
  bool have_temp = read_double_file(b->temp_path, &temp, 1.0 / 1000.0);
  bool have_mhz = read_double_file(
      "/sys/devices/system/cpu/cpu0/cpufreq/scaling_cur_freq", &khz, 1.0 / 1000.0);
  unsigned throttled = 0;
  bool have_thr = false;
  {
    char buf[64];
    if (b->throttled_path[0] && read_small(b->throttled_path, buf, sizeof buf)) {
      throttled = (unsigned)strtoul(buf, NULL, 16);
      have_thr = true;
    }
  }
  char lbuf[64];
  if (read_small("/proc/loadavg", lbuf, sizeof lbuf))
    b->load1 = strtod(lbuf, NULL);

  if (sys_cpu >= 0) {
    b->samples++;
    b->tracked_cpu_sum += tracked_cpu;
    if (tracked_cpu > b->tracked_cpu_max)
      b->tracked_cpu_max = tracked_cpu;
    b->tracked_rss_sum += tracked_rss;
    if (tracked_rss > b->tracked_rss_max)
      b->tracked_rss_max = tracked_rss;
    b->mem_avail_sum += mem_avail;
    if (b->samples == 1 || mem_avail < b->mem_avail_min_mb)
      b->mem_avail_min_mb = mem_avail;
    if (have_temp) {
      if (!b->have_temp || temp < b->temp_min)
        b->temp_min = temp;
      if (!b->have_temp || temp > b->temp_max)
        b->temp_max = temp;
      b->temp_sum += temp;
      b->have_temp = true;
    }
    if (have_mhz) {
      if (!b->have_mhz || khz < b->mhz_min)
        b->mhz_min = khz;
      if (!b->have_mhz || khz > b->mhz_max)
        b->mhz_max = khz;
      b->mhz_sum += khz;
      b->have_mhz = true;
    }
    if (have_thr) {
      b->throttled_or |= throttled & 0xf;
      b->throttled_last = throttled;
      b->have_throttled = true;
    }
    if (b->csv) {
      fprintf(b->csv, "%.1f,system,cpu_pct,,%.2f,\n", b->elapsed_s, sys_cpu);
      fprintf(b->csv, "%.1f,system,tracked_cpu_pct,,%.2f,\n", b->elapsed_s,
              tracked_cpu);
      fprintf(b->csv, "%.1f,system,tracked_rss_mb,,%.1f,\n", b->elapsed_s,
              tracked_rss);
      fprintf(b->csv, "%.1f,system,mem_avail_mb,,%.1f,\n", b->elapsed_s,
              mem_avail);
      fprintf(b->csv, "%.1f,system,load1,,%.2f,\n", b->elapsed_s, b->load1);
      if (have_temp)
        fprintf(b->csv, "%.1f,system,temp_c,,%.1f,\n", b->elapsed_s, temp);
      if (have_mhz)
        fprintf(b->csv, "%.1f,system,cpu_mhz,,%.0f,\n", b->elapsed_s, khz);
      if (have_thr)
        fprintf(b->csv, "%.1f,system,throttled,,%u,\n", b->elapsed_s,
                throttled);
    }
  }
}

// /proc/net/dev: every interface but loopback, bytes in and out.
static void sample_net(Bench *b, double dt, double *busiest_rx,
                       double *busiest_tx, char *busiest_name, size_t cap) {
  static char buf[8192];
  *busiest_rx = *busiest_tx = 0;
  busiest_name[0] = '\0';
  if (!read_small("/proc/net/dev", buf, sizeof buf))
    return;
  const char *line = buf;
  while (line && *line) {
    const char *next = strchr(line, '\n');
    const char *colon = strchr(line, ':');
    if (colon && (!next || colon < next)) {
      char name[32];
      const char *s = line;
      while (*s == ' ')
        s++;
      size_t n = (size_t)(colon - s);
      if (n >= sizeof name)
        n = sizeof name - 1;
      memcpy(name, s, n);
      name[n] = '\0';
      unsigned long long rx = 0, tx = 0, skip;
      // rx: bytes packets errs drop fifo frame compressed multicast; tx: bytes...
      if (strcmp(name, "lo") != 0 &&
          sscanf(colon + 1, " %llu %llu %llu %llu %llu %llu %llu %llu %llu", &rx,
                 &skip, &skip, &skip, &skip, &skip, &skip, &skip, &tx) == 9) {
        Iface *f = NULL;
        for (int i = 0; i < b->iface_count; i++)
          if (strcmp(b->ifaces[i].name, name) == 0)
            f = &b->ifaces[i];
        if (!f && b->iface_count < max_ifaces) {
          f = &b->ifaces[b->iface_count++];
          memset(f, 0, sizeof *f);
          snprintf(f->name, sizeof f->name, "%s", name);
        }
        if (f) {
          if (f->have_prev && dt > 0) {
            double rxk = (double)(rx - f->rx_prev) / 1024.0 / dt;
            double txk = (double)(tx - f->tx_prev) / 1024.0 / dt;
            f->rx_sum += rxk;
            f->tx_sum += txk;
            if (rxk > f->rx_max)
              f->rx_max = rxk;
            if (txk > f->tx_max)
              f->tx_max = txk;
            f->samples++;
            if (rxk + txk > *busiest_rx + *busiest_tx) {
              *busiest_rx = rxk;
              *busiest_tx = txk;
              snprintf(busiest_name, cap, "%s", name);
            }
            if (b->csv && rxk + txk > 0.5)
              fprintf(b->csv, "%.1f,net,%s,,%.1f,%.1f\n", b->elapsed_s, name,
                      rxk, txk);
          }
          f->rx_prev = rx;
          f->tx_prev = tx;
          f->have_prev = true;
        }
      }
    }
    line = next ? next + 1 : NULL;
  }
}

// ---- The video: frame bus streams and hellion's pipelines -----------------

static void collect_stream(const char *name, void *ctx) {
  Bench *b = (Bench *)ctx;
  for (int i = 0; i < b->stream_count; i++)
    if (strcmp(b->streams[i].name, name) == 0)
      return;
  if (b->stream_count >= max_streams)
    return;
  Stream *s = &b->streams[b->stream_count++];
  memset(s, 0, sizeof *s);
  snprintf(s->name, sizeof s->name, "%s", name);
}

// The bus stream most worth one column: "pilot" (the operator's picture)
// if present, else the first alive one. Returns its fps this sample or -1.
static double sample_streams(Bench *b, double dt, char *headline, size_t cap) {
  fbus_list(collect_stream, b);
  double headline_fps = -1;
  headline[0] = '\0';
  for (int i = 0; i < b->stream_count; i++) {
    Stream *s = &b->streams[i];
    FbusStatus st;
    if (fbus_status(s->name, &st) != FBUS_OPEN_OK)
      continue;
    s->width = st.width;
    s->height = st.height;
    if (s->have_prev && dt > 0) {
      double fps = (double)(st.frames - s->frames_prev) / dt;
      s->fps_sum += fps;
      if (s->samples == 0 || fps < s->fps_min)
        s->fps_min = fps;
      if (fps > s->fps_max)
        s->fps_max = fps;
      s->samples++;
      if (b->csv)
        fprintf(b->csv, "%.1f,stream,%s,,%.2f,\n", b->elapsed_s, s->name, fps);
      bool prefer = strcmp(s->name, "pilot") == 0;
      if (headline_fps < 0 || prefer) {
        headline_fps = fps;
        snprintf(headline, cap, "%s", s->name);
      }
    }
    s->frames_prev = st.frames;
    s->have_prev = true;
  }
  return headline_fps;
}

static Endpoint *endpoint_for(Bench *b, const char *instance,
                              const char *pipeline, const char *spec,
                              bool is_sink) {
  for (int i = 0; i < b->endpoint_count; i++) {
    Endpoint *e = &b->endpoints[i];
    if (e->is_sink == is_sink && strcmp(e->instance, instance) == 0 &&
        strcmp(e->pipeline, pipeline) == 0 && strcmp(e->spec, spec) == 0)
      return e;
  }
  if (b->endpoint_count >= max_endpoints)
    return NULL;
  Endpoint *e = &b->endpoints[b->endpoint_count++];
  memset(e, 0, sizeof *e);
  snprintf(e->instance, sizeof e->instance, "%s", instance);
  snprintf(e->pipeline, sizeof e->pipeline, "%s", pipeline);
  snprintf(e->spec, sizeof e->spec, "%s", spec);
  e->is_sink = is_sink;
  return e;
}

static void sample_endpoint(Bench *b, const char *instance,
                            const char *pipeline, const HsEndpoint *ep,
                            bool is_sink) {
  Endpoint *e = endpoint_for(b, instance, pipeline, ep->spec, is_sink);
  if (!e)
    return;
  e->net = ep->net;
  if (e->have_prev) {
    e->frames_delta += ep->frames - e->frames_prev;
    e->dropped_delta += ep->dropped - e->dropped_prev;
    e->kbps_sum += ep->kbps;
    if (ep->kbps > e->kbps_max)
      e->kbps_max = ep->kbps;
    e->loss_sum += ep->loss_pct;
    if (ep->loss_pct > e->loss_max)
      e->loss_max = ep->loss_pct;
    e->samples++;
    if (b->csv && ep->net)
      fprintf(b->csv, "%.1f,link,%s,,%.0f,%.2f\n", b->elapsed_s, ep->spec,
              ep->kbps, ep->loss_pct);
  }
  e->frames_prev = ep->frames;
  e->dropped_prev = ep->dropped;
  e->have_prev = true;
}

static void sample_hellion(Bench *b, const char *only) {
  char names[max_instances][hs_name_len];
  int n;
  if (only) {
    snprintf(names[0], hs_name_len, "%s", only);
    n = 1;
  } else {
    n = hs_list(names, max_instances);
  }
  for (int i = 0; i < n; i++) {
    static HsStatus st;
    if (hs_read(names[i], &st) != HS_READ_OK)
      continue;
    for (uint32_t p = 0; p < st.pipeline_count && p < hs_max_pipelines; p++) {
      const HsPipeline *pl = &st.pipelines[p];
      sample_endpoint(b, st.name, pl->name, &pl->source, false);
      for (uint32_t k = 0; k < pl->sink_count && k < hs_max_sinks; k++)
        sample_endpoint(b, st.name, pl->name, &pl->sinks[k], true);
    }
  }
}

// ---- Output -----------------------------------------------------------------

static const char *throttle_words(unsigned bits, char *buf, size_t cap) {
  // The Pi firmware's bits: 0 under-voltage, 1 arm frequency capped,
  // 2 currently throttled, 3 soft temperature limit active.
  buf[0] = '\0';
  if (bits & 1)
    strncat(buf, "under-voltage ", cap - strlen(buf) - 1);
  if (bits & 2)
    strncat(buf, "freq-capped ", cap - strlen(buf) - 1);
  if (bits & 4)
    strncat(buf, "throttled ", cap - strlen(buf) - 1);
  if (bits & 8)
    strncat(buf, "soft-temp-limit ", cap - strlen(buf) - 1);
  chomp(buf);
  return buf;
}

static int cmp_proc_by_root(const void *a, const void *b) {
  const Proc *x = (const Proc *)a, *y = (const Proc *)b;
  // Roots first, then their children; by pid within that.
  int xr = x->pid == x->root ? x->pid : x->root;
  int yr = y->pid == y->root ? y->pid : y->root;
  if (xr != yr)
    return xr - yr;
  if ((x->pid == x->root) != (y->pid == y->root))
    return x->pid == x->root ? -1 : 1;
  return x->pid - y->pid;
}

typedef struct {
  char name[comm_len];
  int count;
  double cpu_sum;
  double cpu_max;
} ThreadGroup;

static int cmp_group_by_cpu(const void *a, const void *b) {
  const ThreadGroup *x = (const ThreadGroup *)a, *y = (const ThreadGroup *)b;
  if (x->cpu_sum != y->cpu_sum)
    return x->cpu_sum < y->cpu_sum ? 1 : -1;
  return strcmp(x->name, y->name);
}

static void print_threads(const Bench *b, const Proc *p, int samples) {
  // Threads of one process, grouped by name: OpenMP workers and ffmpeg's
  // pool threads share a name, and one line per worker says nothing.
  ThreadGroup groups[64];
  int ng = 0;
  for (int i = 0; i < b->thread_count; i++) {
    const Thread *t = &b->threads[i];
    if (t->pid != p->pid || t->samples == 0)
      continue;
    ThreadGroup *g = NULL;
    for (int k = 0; k < ng; k++)
      if (strcmp(groups[k].name, t->name) == 0)
        g = &groups[k];
    if (!g) {
      if (ng >= 64)
        continue;
      g = &groups[ng++];
      memset(g, 0, sizeof *g);
      snprintf(g->name, sizeof g->name, "%s", t->name);
    }
    g->count++;
    g->cpu_sum += t->cpu_sum;
    if (t->cpu_max > g->cpu_max)
      g->cpu_max = t->cpu_max;
  }
  qsort(groups, (size_t)ng, sizeof groups[0], cmp_group_by_cpu);
  for (int k = 0; k < ng; k++) {
    char label[64];
    if (groups[k].count > 1)
      snprintf(label, sizeof label, "%.31s x%d", groups[k].name,
               groups[k].count);
    else
      snprintf(label, sizeof label, "%.31s", groups[k].name);
    double avg = groups[k].cpu_sum / samples;
    if (avg < 0.05 && groups[k].cpu_max < 0.5)
      continue; // idle threads: the http accept loop, the heartbeat
    printf("      %-30s %6.1f %6.1f\n", label, avg, groups[k].cpu_max);
  }
}

static void print_summary(Bench *b, const char *hostname) {
  int samples = b->samples;
  if (samples == 0) {
    printf("\nno samples: nothing to sum up\n");
    return;
  }
  printf("\n");
  printf("BENCH  %s  %d samples over %.0fs  %d core%s (100%% = one core "
         "busy)\n",
         hostname, samples, b->elapsed_s, b->cores, b->cores == 1 ? "" : "s");

  printf("\n  %-36s %6s %6s   %7s %7s %7s  %8s %9s\n", "process / thread",
         "cpu%", "max", "rss MB", "max", "hwm", "ctxsw/s", "write kB/s");
  qsort(b->procs, (size_t)b->proc_count, sizeof b->procs[0], cmp_proc_by_root);
  for (int i = 0; i < b->proc_count; i++) {
    const Proc *p = &b->procs[i];
    if (p->samples == 0)
      continue;
    char label[64];
    if (p->pid == p->root)
      snprintf(label, sizeof label, "%s [%d]%s", p->comm, p->pid,
               p->exited ? " (exited)" : "");
    else
      snprintf(label, sizeof label, "  %s [%d] <- %s%s", p->comm, p->pid,
               p->parent_comm, p->exited ? " (exited)" : "");
    char io[16];
    if (p->io_state == 1)
      snprintf(io, sizeof io, "%9.0f",
               (double)p->io_sum / 1024.0 / (b->elapsed_s > 0 ? b->elapsed_s : 1));
    else
      snprintf(io, sizeof io, "%9s", "n/a");
    printf("  %-36s %6.1f %6.1f   %7.1f %7.1f %7.1f  %8.0f %s\n", label,
           p->cpu_sum / p->samples, p->cpu_max, p->rss_sum / p->samples,
           p->rss_max, p->hwm_mb,
           p->have_ctxt ? (double)p->ctxt_delta_sum / b->elapsed_s : 0.0, io);
    print_threads(b, p, p->samples);
  }
  printf("  %-36s %6.1f %6.1f   %7.1f %7.1f\n", "all of the above",
         b->tracked_cpu_sum / samples, b->tracked_cpu_max,
         b->tracked_rss_sum / samples, b->tracked_rss_max);
  double other = b->sys_cpu_sum / samples - b->tracked_cpu_sum / samples;
  printf("  %-36s %6.1f\n", "everything else on the machine",
         other < 0 ? 0 : other);
  printf("  %-36s %6.1f %6.1f\n", "machine total",
         b->sys_cpu_sum / samples, b->sys_cpu_max);
  if (b->cores > 1) {
    printf("  %-36s", "  per core");
    for (int i = 0; i < b->cores && i < 16; i++)
      printf(" %5.0f", b->core_cpu_sum[i] / samples);
    printf("%s\n", b->cores > 16 ? " ..." : "");
  }
  if (b->io_denied)
    printf("  (write kB/s needs the processes' owner or root: sudo hellion "
           "bench)\n");

  printf("\n  memory   %.0f MB available of %.0f (lowest %.0f)   load %.2f\n",
         b->mem_avail_sum / samples, b->mem_total_mb, b->mem_avail_min_mb,
         b->load1);
  if (b->have_temp)
    printf("  thermal  %.1f C avg, %.1f-%.1f C", b->temp_sum / samples,
           b->temp_min, b->temp_max);
  else
    printf("  thermal  no sensor found");
  if (b->have_mhz)
    printf("   clock %.0f MHz avg, %.0f-%.0f", b->mhz_sum / samples, b->mhz_min,
           b->mhz_max);
  printf("\n");
  if (b->have_throttled) {
    char words[80];
    if (b->throttled_or)
      printf("  throttle DURING THIS RUN: %s\n",
             throttle_words(b->throttled_or, words, sizeof words));
    else
      printf("  throttle none during this run\n");
    unsigned ever = (b->throttled_last >> 16) & 0xf;
    if (ever)
      printf("           since boot: %s\n",
             throttle_words(ever, words, sizeof words));
  }

  bool any_stream = false;
  for (int i = 0; i < b->stream_count; i++)
    if (b->streams[i].samples > 0) {
      if (!any_stream)
        printf("\n  frame bus\n");
      any_stream = true;
      const Stream *s = &b->streams[i];
      printf("    %-14s %ux%-5u %5.1f fps avg, %.1f-%.1f\n", s->name, s->width,
             s->height, s->fps_sum / s->samples, s->fps_min, s->fps_max);
    }
  bool any_ep = false;
  for (int i = 0; i < b->endpoint_count; i++) {
    const Endpoint *e = &b->endpoints[i];
    if (e->samples == 0)
      continue;
    if (!any_ep)
      printf("\n  hellion pipelines\n");
    any_ep = true;
    double secs = b->elapsed_s > 0 ? b->elapsed_s : 1;
    printf("    %s/%s %s %-28s %5.1f fps  %llu dropped", e->instance,
           e->pipeline, e->is_sink ? "sink  " : "source", e->spec,
           (double)e->frames_delta / secs,
           (unsigned long long)e->dropped_delta);
    if (e->net)
      printf("  %.0f kbit/s avg (max %.0f)  loss %.2f%% avg (max %.2f%%)",
             e->kbps_sum / e->samples, e->kbps_max, e->loss_sum / e->samples,
             e->loss_max);
    printf("\n");
  }
  bool any_if = false;
  for (int i = 0; i < b->iface_count; i++) {
    const Iface *f = &b->ifaces[i];
    if (f->samples == 0 || (f->rx_sum + f->tx_sum) / f->samples < 0.5)
      continue;
    if (!any_if)
      printf("\n  network\n");
    any_if = true;
    printf("    %-12s %8.0f kB/s out (max %.0f)  %8.0f kB/s in (max %.0f)\n",
           f->name, f->tx_sum / f->samples, f->tx_max, f->rx_sum / f->samples,
           f->rx_max);
  }
  printf("\n");
}

// ---- The run ----------------------------------------------------------------

int hellion_bench(const char *name, int seconds, const char *csv_path) {
  static Bench b;
  memset(&b, 0, sizeof b);
  b.seconds = seconds > 0 ? seconds : 30;
  b.self_pid = getpid();
  b.clk_tck = sysconf(_SC_CLK_TCK);
  if (b.clk_tck <= 0)
    b.clk_tck = 100;
  find_temp_path(b.temp_path, sizeof b.temp_path);
  find_throttled_path(b.throttled_path, sizeof b.throttled_path);

  if (csv_path) {
    b.csv = fopen(csv_path, "w");
    if (!b.csv) {
      fprintf(stderr, "hellion bench: cannot write %s: %s\n", csv_path,
              strerror(errno));
      return 2;
    }
    fprintf(b.csv, "t_s,kind,name,id,value,value2\n");
  }

  char hostname[64] = "?";
  gethostname(hostname, sizeof hostname);
  hostname[sizeof hostname - 1] = '\0';

  struct sigaction sa;
  memset(&sa, 0, sizeof sa);
  sa.sa_handler = on_signal;
  sigaction(SIGINT, &sa, NULL);
  sigaction(SIGTERM, &sa, NULL);

  // A first pass primes every counter; the figures start with the second.
  uint64_t t_start = now_ns();
  uint64_t t_prev = t_start;
  double rss;
  sample_processes(&b, 0, &rss);
  sample_system(&b, 0, 0);
  {
    double rx, tx;
    char n[32];
    sample_net(&b, 0, &rx, &tx, n, sizeof n);
    char h[32];
    sample_streams(&b, 0, h, sizeof h);
    sample_hellion(&b, name);
  }
  if (b.proc_count == 0)
    printf("hellion bench: no eeye or hellion running here -- measuring the "
           "machine only\n");
  else {
    printf("hellion bench: following");
    for (int i = 0; i < b.proc_count; i++)
      printf(" %s[%d]", b.procs[i].comm, b.procs[i].pid);
    printf("\n");
  }
  printf("%ds, one sample a second (Ctrl-C ends it early)%s%s\n\n", b.seconds,
         csv_path ? "; every sample to " : "", csv_path ? csv_path : "");
  printf("%5s %7s %7s %8s %7s %7s %6s %6s %9s %s\n", "t", "total%",
         "eeye%", "hellion%", "child%", "rss MB", "temp", "MHz", "fps",
         "net kB/s");

  for (int i = 0; i < b.seconds && !stop_requested; i++) {
    struct timespec ts = {1, 0};
    nanosleep(&ts, NULL);
    if (stop_requested)
      break;
    uint64_t t = now_ns();
    double dt = (double)(t - t_prev) / 1e9;
    t_prev = t;
    b.elapsed_s = (double)(t - t_start) / 1e9;

    // Per-sample CPU by group needs each process's figure for THIS
    // sample: taken as the change in its running sum.
    double cpu_before[max_procs];
    for (int k = 0; k < b.proc_count; k++)
      cpu_before[k] = b.procs[k].cpu_sum;
    int procs_before = b.proc_count;
    double rss_total;
    double tracked_cpu = sample_processes(&b, dt, &rss_total);
    double eeye_cpu = 0, hellion_cpu = 0, child_cpu = 0;
    for (int k = 0; k < b.proc_count; k++) {
      const Proc *p = &b.procs[k];
      double cpu = p->cpu_sum - (k < procs_before ? cpu_before[k] : 0);
      if (p->pid != p->root)
        child_cpu += cpu;
      else if (strncmp(p->comm, "eeye", 4) == 0)
        eeye_cpu += cpu;
      else
        hellion_cpu += cpu;
    }
    double sys_before = b.sys_cpu_sum;
    sample_system(&b, tracked_cpu, rss_total);
    double sys_cpu = b.sys_cpu_sum - sys_before;
    double rx, tx;
    char iface[32];
    sample_net(&b, dt, &rx, &tx, iface, sizeof iface);
    char stream[32];
    double fps = sample_streams(&b, dt, stream, sizeof stream);
    sample_hellion(&b, name);

    char temp_s[16] = "   -", mhz_s[16] = "   -", fps_s[48] = "        -";
    double temp;
    if (read_double_file(b.temp_path, &temp, 1.0 / 1000.0))
      snprintf(temp_s, sizeof temp_s, "%5.1f", temp);
    double mhz;
    if (read_double_file("/sys/devices/system/cpu/cpu0/cpufreq/scaling_cur_freq",
                         &mhz, 1.0 / 1000.0))
      snprintf(mhz_s, sizeof mhz_s, "%5.0f", mhz);
    if (fps >= 0)
      snprintf(fps_s, sizeof fps_s, "%4.1f %s", fps, stream);
    printf("%4.0fs %7.1f %7.1f %8.1f %7.1f %7.1f %6s %6s %9s %5.0f %s\n",
           b.elapsed_s, sys_cpu, eeye_cpu, hellion_cpu, child_cpu, rss_total,
           temp_s, mhz_s, fps_s, rx + tx, iface);
    fflush(stdout);
  }

  print_summary(&b, hostname);
  if (b.csv) {
    fclose(b.csv);
    printf("every sample is in %s\n", csv_path);
  }
  return 0;
}
