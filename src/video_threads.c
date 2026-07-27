#include "video_threads.h"
#include "frame_ring_buffer.h"
#include "point_opps.h"
#include "video_frame.h"
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <threads.h>

void *producer_loop(void *arg) {
  ProducerArgs *args = (ProducerArgs *)arg;

  FILE *infile = fopen(args->filename, "rb");
  if (!infile) {
    printf("Failed to open input file.\n");
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
      printf("End of file reached.\n");
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

  fclose(infile);
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
  FILE *outfile = fopen(args->outpath, "wb");
  if (!outfile) {
    printf("Failed to open output file.\n");
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

    printf("Saved processed frame %lld\n", (long long)frame->pts);

    while (!ring_push(args->ring_buffer_free, frame)) {
      sleep_us(100);
    }
  }

  fclose(outfile);
  return NULL;
}
