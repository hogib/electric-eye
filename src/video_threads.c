#include "video_threads.h"
#include "frame_ring_buffer.h"
#include "point_opps.h"
#include "video_frame.h"
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <threads.h>

/*
 * Camera capture / virtual-cam output are done by shelling out to ffmpeg via
 * popen(). This keeps producer_loop/consumer_loop nearly unchanged: fread()
 * and fwrite() work identically whether the FILE* came from fopen() or
 * popen(), since popen() just hands back one end of a pipe to the child
 * process's stdout/stdin.
 *
 * args->filename (ProducerArgs) is now the camera device, e.g. "/dev/video0"
 * args->outpath   (ConsumerArgs) is now the loopback device, e.g. "/dev/video10"
 *
 * Tune these two to match your webcam's supported capture mode:
 *   - Most USB webcams can do 1080p only via MJPEG; raw yuyv422 is often
 *     capped at lower resolutions/framerates. If the capture ffmpeg fails,
 *     try switching CAMERA_INPUT_FORMAT to "yuyv422" and/or lowering
 *     CAMERA_FRAMERATE, or check `ffmpeg -f v4l2 -list_formats all -i
 *     /dev/video0` for what your device actually supports.
 */
#define CAMERA_INPUT_FORMAT "mjpeg"
#define CAMERA_FRAMERATE 30

void *producer_loop(void *arg) {
  ProducerArgs *args = (ProducerArgs *)arg;

  char cmd[512];
  snprintf(cmd, sizeof(cmd),
           "ffmpeg -hide_banner -loglevel error "
           "-f v4l2 -input_format %s -framerate %d -video_size %ux%u -i %s "
           "-pix_fmt yuv422p -f rawvideo -",
           CAMERA_INPUT_FORMAT, CAMERA_FRAMERATE, args->frame_width,
           args->frame_height, args->filename);

  FILE *infile = popen(cmd, "r");
  if (!infile) {
    printf("Failed to start camera capture (ffmpeg): %s\n", cmd);
    atomic_store(args->is_running, false);
    return NULL;
  }

  int64_t pts = 0;

  while (atomic_load(args->is_running)) {
    VideoFrame *frame = NULL;

    while (!ring_pop(args->ring_buffer_free, (void **)&frame) &&
           atomic_load(args->is_running)) {
      sleep_us(100);
    }

    if (!frame)
      break;

    frame->pts = pts++;

    size_t bytes_read = 0;
    bytes_read += fread(frame->planes[0], 1, frame->plane_sizes[0], infile);
    bytes_read += fread(frame->planes[1], 1, frame->plane_sizes[1], infile);
    bytes_read += fread(frame->planes[2], 1, frame->plane_sizes[2], infile);

    if (bytes_read < (frame->plane_sizes[0] + frame->plane_sizes[1] +
                      frame->plane_sizes[2])) {
      printf("Camera stream ended or ffmpeg capture died.\n");
      while (!ring_push(args->ring_buffer_free, frame))
        sleep_us(10);
      atomic_store(args->is_running, false);
      break;
    }

    while (!ring_push(args->ring_buffer_in, frame) &&
           atomic_load(args->is_running)) {
      sleep_us(100);
    }
  }

  pclose(infile);
  return NULL;
}

void *effects_loop(void *arg) {
  WorkerArgs *args = (WorkerArgs *)arg;
  while (
      atomic_load(args->is_running) ||
      atomic_load_explicit(&args->ring_buffer_in->head, memory_order_relaxed) !=
          atomic_load_explicit(&args->ring_buffer_in->tail,
                               memory_order_relaxed)) {

    VideoFrame *frame = NULL;

    if (!ring_pop(args->ring_buffer_in, (void **)&frame)) {
      sleep_us(100);
      continue;
    }

    grayscale(frame);
    gs_invert(frame);

    while (!ring_push(args->ring_buffer_out, frame)) {
      sleep_us(100);
    }
  }
  return NULL;
}

void *consumer_loop(void *arg) {
  ConsumerArgs *args = (ConsumerArgs *)arg;

  char cmd[512];
  snprintf(cmd, sizeof(cmd),
           "ffmpeg -hide_banner -loglevel error "
           "-f rawvideo -pix_fmt yuv422p -s %ux%u -r %d -i - "
           "-f v4l2 %s",
           args->frame_width, args->frame_height, CAMERA_FRAMERATE,
           args->outpath);

  FILE *outfile = popen(cmd, "w");
  if (!outfile) {
    printf("Failed to start virtual camera output (ffmpeg): %s\n", cmd);
    return NULL;
  }

  while (atomic_load(args->is_running) ||
         atomic_load_explicit(&args->ring_buffer_out->head,
                              memory_order_relaxed) !=
             atomic_load_explicit(&args->ring_buffer_out->tail,
                                  memory_order_relaxed)) {

    VideoFrame *frame = NULL;

    if (!ring_pop(args->ring_buffer_out, (void **)&frame)) {
      sleep_us(100);
      continue;
    }

    fwrite(frame->planes[0], 1, frame->plane_sizes[0], outfile);
    fwrite(frame->planes[1], 1, frame->plane_sizes[1], outfile);
    fwrite(frame->planes[2], 1, frame->plane_sizes[2], outfile);

    if (ferror(outfile)) {
      // The output ffmpeg died (e.g. loopback device went away). Stop the
      // whole pipeline rather than spinning on a broken pipe.
      printf("Virtual camera output pipe broke; shutting down.\n");
      while (!ring_push(args->ring_buffer_free, frame))
        sleep_us(10);
      atomic_store(args->is_running, false);
      break;
    }

    printf("Sent processed frame %lld to virtual camera\n",
           (long long)frame->pts);

    while (!ring_push(args->ring_buffer_free, frame)) {
      sleep_us(100);
    }
  }

  pclose(outfile);
  return NULL;
}
