#include "video_threads.h"
#include "frame_ring_buffer.h"
#include "point_opps.h"
#include "video_frame.h"
#include <pthread.h>
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
    VideoFrame *frame =
        video_frame_create_i422(args->frame_width, args->frame_height, pts++);
    if (!frame)
      continue;

    size_t y_size = (size_t)frame->stride[0] * frame->height;
    size_t u_size = (size_t)frame->stride[1] * frame->height;
    size_t v_size = (size_t)frame->stride[2] * frame->height;

    size_t bytes_read = 0;
    bytes_read += fread(frame->planes[0], 1, y_size, infile);
    bytes_read += fread(frame->planes[1], 1, u_size, infile);
    bytes_read += fread(frame->planes[2], 1, v_size, infile);

    if (bytes_read < (y_size + u_size + v_size)) {
      printf("End of file reached.\n");
      video_frame_free(frame);
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

    size_t y_size = (size_t)frame->stride[0] * frame->height;
    size_t u_size = (size_t)frame->stride[1] * frame->height;
    size_t v_size = (size_t)frame->stride[2] * frame->height;

    fwrite(frame->planes[0], 1, y_size, outfile);
    fwrite(frame->planes[1], 1, u_size, outfile);
    fwrite(frame->planes[2], 1, v_size, outfile);

    printf("Saved processed frame %lld\n", frame->pts);

    video_frame_free(frame);
  }

  fclose(outfile);
  return NULL;
}
