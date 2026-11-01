#define _POSIX_C_SOURCE 200809L

#include "ffmpeg.h"
#include "child_proc.h"
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <sys/wait.h>

static pthread_once_t probe_once = PTHREAD_ONCE_INIT;
static bool have_ffmpeg;
static bool use_fps_mode; // else -vsync

static void probe(void) {
  have_ffmpeg = child_program_exists("ffmpeg");
  if (!have_ffmpeg) {
    printf("hellion: ffmpeg is not installed; RTP sources and sinks cannot "
           "run (apt install ffmpeg)\n");
    return;
  }
  // Try the new name on a one-frame null encode; fall back to the old.
  char *const argv[] = {"ffmpeg", "-hide_banner", "-nostdin", "-loglevel",
                        "quiet", "-f", "lavfi", "-i",
                        "nullsrc=s=16x16:d=0.04", "-fps_mode", "passthrough",
                        "-f", "null", "-", NULL};
  ChildProc c;
  int status = -1;
  if (child_spawn(&c, "hellion", argv, CHILD_NULL_STDIN | CHILD_QUIET_STDERR)) {
    if (!child_wait(&c, 10000, &status))
      status = child_stop(&c, 500);
    else
      child_stop(&c, 0);
  }
  use_fps_mode = status >= 0 && WIFEXITED(status) && WEXITSTATUS(status) == 0;
  printf("hellion: using ffmpeg with %s\n",
         use_fps_mode ? "-fps_mode passthrough" : "-vsync 0");
}

bool ff_available(void) {
  pthread_once(&probe_once, probe);
  return have_ffmpeg;
}

static void add(FfArgs *a, const char *fmt, ...) {
  if (a->argc >= (int)(sizeof a->argv / sizeof a->argv[0]) - 1)
    return;
  char *dst = a->store + a->used;
  size_t room = sizeof a->store - a->used;
  va_list ap;
  va_start(ap, fmt);
  int n = vsnprintf(dst, room, fmt, ap);
  va_end(ap);
  if (n < 0 || (size_t)n >= room)
    return;
  a->argv[a->argc++] = dst;
  a->argv[a->argc] = NULL;
  a->used += (size_t)n + 1;
}

// `loglevel`: what ffmpeg itself prints (into hellion's log).
static void common(FfArgs *a, const char *loglevel) {
  memset(a, 0, sizeof *a);
  add(a, "ffmpeg");
  add(a, "-hide_banner");
  // -nostdin only stops ffmpeg reading the keyboard; "-i -" still reads
  // frames from the pipe.
  add(a, "-nostdin");
  add(a, "-loglevel");
  add(a, "%s", loglevel);
  add(a, "-nostats");
}

static void passthrough(FfArgs *a) {
  ff_available(); // ensure the probe ran
  if (use_fps_mode) {
    add(a, "-fps_mode");
    add(a, "passthrough");
  } else {
    add(a, "-vsync");
    add(a, "0");
  }
}

void ff_encoder_args(const FfEncoder *e, FfArgs *a) {
  common(a, "warning");
  // Input: raw I422 from hellion, stamped at the pipeline's nominal frame
  // rate. Arrival-time stamps were tried and rejected, measured with
  // ffmpeg 9 and libx264 at 640x480:
  // - wall clock at the rawvideo default time base (1/25s): arrivals 33ms
  //   apart round onto the same tick, and ffmpeg silently dropped one
  //   frame in six -- the link was capped at 25fps;
  // - wall clock at a 1ms time base: no drops, but x264 then budgets bits
  //   for 1000fps and sent 161 kbit/s of a 4000 kbit/s target;
  // - "-r 30" to fix that is refused alongside -fps_mode passthrough.
  // The cost of nominal stamps: a source running at another rate plays
  // back at the wrong speed in a recording. sinks.c warns when it does.
  add(a, "-framerate");
  add(a, "%u", e->framerate);
  add(a, "-f");
  add(a, "rawvideo");
  add(a, "-pix_fmt");
  add(a, "yuv422p");
  add(a, "-s");
  add(a, "%ux%u", e->width, e->height);
  add(a, "-i");
  add(a, "-");

  add(a, "-vf");
  add(a, "scale=in_range=pc:out_range=tv,format=yuv420p");
  add(a, "-c:v");
  add(a, "%s", e->encoder);
  if (strcmp(e->encoder, "libx264") == 0) {
    // No B-frames, no lookahead: each frame leaves as soon as it is
    // encoded.
    add(a, "-preset");
    add(a, "ultrafast");
    add(a, "-tune");
    add(a, "zerolatency");
    add(a, "-maxrate");
    add(a, "%uk", e->bitrate_kbps);
    add(a, "-bufsize");
    add(a, "%uk", e->bitrate_kbps / 2 ? e->bitrate_kbps / 2 : 1);
  }
  add(a, "-b:v");
  add(a, "%uk", e->bitrate_kbps);
  add(a, "-g");
  add(a, "%u", e->gop);
  // SPS/PPS before every keyframe, whatever the encoder: a receiver that
  // starts late (QGC opened mid-dive, a recording started) needs them,
  // and there is no SDP exchange to carry them out of band.
  add(a, "-bsf:v");
  add(a, "dump_extra=freq=keyframe");
  add(a, "-color_range");
  add(a, "tv");
  passthrough(a);

  add(a, "-f");
  add(a, "rtp");
  // Fits a 1500-byte MTU with room for tunnel/VPN overhead.
  add(a, "-pkt_size");
  add(a, "1200");
  // The SDP ffmpeg would otherwise print on every start.
  add(a, "-sdp_file");
  add(a, "/dev/null");
  add(a, "rtp://127.0.0.1:%d", e->rtp_port);
}

void ff_decoder_args(const FfDecoder *d, FfArgs *a) {
  // error, not warning: the rawvideo muxer warns about timestamps on every
  // frame, and timestamps mean nothing on a raw pipe.
  common(a, "error");
  add(a, "-protocol_whitelist");
  add(a, "file,udp,rtp");
  // Latency over smoothness: no input buffering, no reordering wait. A
  // late packet costs a damaged frame, never a delayed picture.
  add(a, "-fflags");
  add(a, "nobuffer");
  add(a, "-flags");
  add(a, "low_delay");
  add(a, "-max_delay");
  add(a, "0");
  add(a, "-reorder_queue_size");
  add(a, "0");
  add(a, "-i");
  add(a, "%s", d->sdp_path);

  // Out at a fixed size and full range, so every frame is exactly
  // width x height x 2 bytes for hellion to read.
  add(a, "-vf");
  add(a, "scale=w=%u:h=%u:in_range=tv:out_range=pc,format=yuv422p", d->width,
      d->height);
  passthrough(a);
  // The rawvideo *encoder* (a plain copy) runs frame-threaded on ffmpeg
  // 5.1 and later, which hands each frame back only once the next one is
  // in: one frame period of delay (33ms at 30fps) for nothing. Measured:
  // a raw -> scale -> raw ffmpeg holds 34ms at 30fps, 1ms with this. Slice
  // threading rather than -threads 1: on an output, -threads also sets the
  // filter graph's threads, which would make the scale single-threaded.
  add(a, "-thread_type");
  add(a, "slice");
  add(a, "-f");
  add(a, "rawvideo");
  add(a, "-flush_packets");
  add(a, "1");
  add(a, "-");
}

void ff_recorder_args(const char *sdp_path, const char *out_path, FfArgs *a) {
  common(a, "warning");
  add(a, "-protocol_whitelist");
  add(a, "file,udp,rtp");
  add(a, "-i");
  add(a, "%s", sdp_path);
  add(a, "-map");
  add(a, "0:v");
  // The tether's H.264 as it arrived: no decode, no re-encode, no quality
  // lost, almost no CPU.
  add(a, "-c");
  add(a, "copy");
  // Fragmented MP4: every keyframe closes a fragment that plays on its own.
  // A recording cut off by a crash or a power loss plays up to its last
  // keyframe, and stopping is instant. (+faststart, which an earlier
  // recorder used "so the file still plays if the boat loses power", does
  // the opposite: it writes the index only at the end, so a recording that
  // never reaches the end has none and will not open at all.)
  add(a, "-movflags");
  add(a, "+frag_keyframe+empty_moov+default_base_moof");
  add(a, "-f");
  add(a, "mp4");
  add(a, "-y");
  add(a, "%s", out_path);
}

bool ff_write_sdp(const char *path, int port) {
  FILE *f = fopen(path, "we");
  if (!f)
    return false;
  fprintf(f,
          "v=0\n"
          "o=- 0 0 IN IP4 127.0.0.1\n"
          "s=hellion\n"
          "c=IN IP4 127.0.0.1\n"
          "t=0 0\n"
          "m=video %d RTP/AVP 96\n"
          "a=rtpmap:96 H264/90000\n"
          "a=fmtp:96 packetization-mode=1\n",
          port);
  return fclose(f) == 0;
}

void ff_describe(const FfArgs *a, char *out, size_t cap) {
  size_t n = 0;
  out[0] = '\0';
  for (int i = 0; i < a->argc && n + 1 < cap; i++) {
    int w = snprintf(out + n, cap - n, "%s%s", i ? " " : "", a->argv[i]);
    if (w < 0)
      break;
    n += (size_t)w;
  }
}
