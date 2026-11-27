// ee_run: eeye's effects, offline.
//
// Reads raw frames from a file, runs them through exactly what eeye's
// effects thread runs (img_stats, auto visibility, the effect chain: the
// same functions, linked from src/), writes the result and prints what
// each frame cost. For the bench: picture quality against other methods
// on image sets, and timings that can be repeated without a camera.
//
//   ee_run -c config.json -s 1280x720 [-r 30] [--still] [--repeat N]
//          in.yuv out.yuv|-
//
// Frames are planar YUV 4:2:2, full range, 8 bits: what ffmpeg calls
// yuvj422p, and what eeye holds a frame as.
//   ffmpeg -i clip.mp4 -vf scale=1280:720:out_range=pc -pix_fmt yuvj422p
//          -f rawvideo in.yuv
//   ffmpeg -f rawvideo -pix_fmt yuvj422p -s 1280x720 -i out.yuv out-%04d.png
//
// -c       an eeye config: its "chain" and "auto_visibility" are used; the
//          geometry in it is not (the size comes from -s).
// -r       frames per second the input was captured at (default 30): auto
//          visibility smooths over capture time, so this is what makes a
//          file behave like the live video it was.
// --still  every frame is its own picture: auto visibility starts afresh
//          on each, as it does on the first frame of a video. For image
//          sets.
// --repeat N  process each frame N times and time the fastest (default
//          1). Only with --still: repeating a frame of a video would move
//          its smoothing along.
// out "-"  writes nothing: timing only.
//
// One JSON object on stdout: the frames, and the time per frame in
// milliseconds for the statistics and the chain together, as eeye's own
// "effects:" log line counts it.
#define _POSIX_C_SOURCE 200809L
#include "auto_vis.h"
#include "config.h"
#include "effect_chain.h"
#include "img_stats.h"
#include "video_frame.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#ifdef _OPENMP
#include <omp.h>
#endif

static double now_ms(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return ts.tv_sec * 1e3 + ts.tv_nsec / 1e6;
}

static int cmp_double(const void *a, const void *b) {
  double x = *(const double *)a, y = *(const double *)b;
  return (x > y) - (x < y);
}

static double percentile(const double *sorted, size_t n, double f) {
  size_t i = (size_t)(f * (double)(n - 1) + 0.5);
  return sorted[i < n ? i : n - 1];
}

// One plane, row by row: the frame's stride need not be its width.
static bool read_plane(FILE *f, uint8_t *dst, size_t stride, uint32_t w,
                       uint32_t h) {
  for (uint32_t y = 0; y < h; y++)
    if (fread(dst + (size_t)y * stride, 1, w, f) != w)
      return false;
  return true;
}

static bool write_plane(FILE *f, const uint8_t *src, size_t stride,
                        uint32_t w, uint32_t h) {
  for (uint32_t y = 0; y < h; y++)
    if (fwrite(src + (size_t)y * stride, 1, w, f) != w)
      return false;
  return true;
}

static int usage(void) {
  fprintf(stderr, "usage: ee_run -c config.json -s WxH [-r fps] [--still] "
                  "[--repeat N] in.yuv out.yuv|-\n");
  return 2;
}

int main(int argc, char **argv) {
  const char *cfg_path = NULL, *in_path = NULL, *out_path = NULL;
  unsigned w = 0, h = 0, repeat = 1;
  double fps = 30;
  bool still = false;
  for (int i = 1; i < argc; i++) {
    if (!strcmp(argv[i], "-c") && i + 1 < argc)
      cfg_path = argv[++i];
    else if (!strcmp(argv[i], "-s") && i + 1 < argc) {
      if (sscanf(argv[++i], "%ux%u", &w, &h) != 2)
        return usage();
    } else if (!strcmp(argv[i], "-r") && i + 1 < argc)
      fps = atof(argv[++i]);
    else if (!strcmp(argv[i], "--still"))
      still = true;
    else if (!strcmp(argv[i], "--repeat") && i + 1 < argc)
      repeat = (unsigned)atoi(argv[++i]);
    else if (!in_path)
      in_path = argv[i];
    else if (!out_path)
      out_path = argv[i];
    else
      return usage();
  }
  if (!cfg_path || !in_path || !out_path || !w || !h || (w & 1) || fps <= 0 ||
      repeat < 1 || (repeat > 1 && !still))
    return usage();

  Config cfg;
  if (!config_load_once(cfg_path, &cfg)) {
    fprintf(stderr, "ee_run: %s is not a valid eeye config\n", cfg_path);
    return 1;
  }
  FILE *in = fopen(in_path, "rb");
  FILE *out = strcmp(out_path, "-") ? fopen(out_path, "wb") : NULL;
  if (!in || (strcmp(out_path, "-") && !out)) {
    fprintf(stderr, "ee_run: cannot open %s\n", in ? out_path : in_path);
    return 1;
  }
  VideoFrame *vf = vf_create(w, h, 0);
  AutoVis *av = auto_vis_create();
  size_t cap = 1024, n = 0;
  double *ms = malloc(cap * sizeof *ms);
  if (!vf || !av || !ms) {
    fprintf(stderr, "ee_run: out of memory\n");
    return 1;
  }

  const uint64_t period_ns = (uint64_t)(1e9 / fps);
  for (;;) {
    vf_restore_roles(vf);
    if (!read_plane(in, vf->raw_planes[0], vf->stride[0], w, h) ||
        !read_plane(in, vf->raw_planes[1], vf->stride[1], w / 2, h) ||
        !read_plane(in, vf->raw_planes[2], vf->stride[2], w / 2, h))
      break;
    vf->pts = (int64_t)n;
    // Any start but 0, which means "unknown" to auto visibility.
    vf->capture_ns = 1000000000ull + n * period_ns;

    double best = 0;
    for (unsigned r = 0; r < repeat; r++) {
      if (still) {
        auto_vis_destroy(av);
        av = auto_vis_create();
        if (!av)
          return 1;
      }
      vf_restore_roles(vf);
      double t0 = now_ms();
      ImgStats stats;
      memset(&stats, 0, sizeof stats);
      if (cfg.auto_visibility) {
        const uint8_t *raw[3] = {vf->raw_planes[0], vf->raw_planes[1],
                                 vf->raw_planes[2]};
        img_stats_compute(raw, w, h, vf->stride, &stats);
        img_stats_summarize(&stats, &vf->stats);
      }
      apply_effect_chain_auto(vf, &cfg, av, &stats);
      double t = now_ms() - t0;
      if (r == 0 || t < best)
        best = t;
    }
    if (n == cap) {
      cap *= 2;
      ms = realloc(ms, cap * sizeof *ms);
      if (!ms)
        return 1;
    }
    ms[n++] = best;
    if (out && (!write_plane(out, vf->planes[0], vf->stride[0], w, h) ||
                !write_plane(out, vf->planes[1], vf->stride[1], w / 2, h) ||
                !write_plane(out, vf->planes[2], vf->stride[2], w / 2, h))) {
      fprintf(stderr, "ee_run: cannot write %s\n", out_path);
      return 1;
    }
  }
  if (out && fclose(out) != 0) {
    fprintf(stderr, "ee_run: cannot write %s\n", out_path);
    return 1;
  }
  if (n == 0) {
    fprintf(stderr, "ee_run: %s holds no whole %ux%u frame\n", in_path, w, h);
    return 1;
  }

  // The first frame of a run pays for page faults and the thread team's
  // start; a video's figures leave it out when there are others.
  size_t skip = (!still && n > 1) ? 1 : 0;
  size_t m = n - skip;
  double sum = 0;
  for (size_t i = skip; i < n; i++)
    sum += ms[i];
  qsort(ms + skip, m, sizeof *ms, cmp_double);
  int threads = 1;
#ifdef _OPENMP
  threads = omp_get_max_threads();
#endif
  printf("{\"frames\": %zu, \"width\": %u, \"height\": %u, \"threads\": %d, "
         "\"still\": %s, \"repeat\": %u, \"ms_mean\": %.4f, \"ms_p50\": %.4f, "
         "\"ms_p95\": %.4f, \"ms_p99\": %.4f, \"ms_max\": %.4f}\n",
         n, w, h, threads, still ? "true" : "false", repeat, sum / (double)m,
         percentile(ms + skip, m, 0.50), percentile(ms + skip, m, 0.95),
         percentile(ms + skip, m, 0.99), ms[n - 1]);
  auto_vis_destroy(av);
  vf_free(vf);
  free(ms);
  fclose(in);
  return 0;
}
