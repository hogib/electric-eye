#include "frame_ring_buffer.h"
#include "point_opps.h"
#include "video_frame.h"
#include <pthread.h>
#include <stdio.h>
#include <threads.h>

FrameRingBuffer ring_buffer_in;
FrameRingBuffer ring_buffer_out;

atomic_bool is_running = true;

const char *input_filename = "test/input_1920x1080_i422.yuv";
uint32_t frame_width = 1920;
uint32_t frame_height = 1080;

static inline void sleep_us(long microseconds) {
  struct timespec ts;
  ts.tv_sec = microseconds / 1000000;
  ts.tv_nsec = (microseconds % 1000000) * 1000;

  thrd_sleep(&ts, NULL);
}

void *producer_loop(void *arg) {
  FILE *infile = fopen(input_filename, "rb");
  if (!infile) {
    printf("Failed to open input file.\n");
    atomic_store(&is_running, false);
    return NULL;
  }

  int64_t pts = 0;

  while (atomic_load(&is_running)) {
    VideoFrame *frame =
        video_frame_create_i422(frame_width, frame_height, pts++);
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
      atomic_store(&is_running, false);
      break;
    }

    while (!ring_push(&ring_buffer_in, frame) && atomic_load(&is_running)) {
      sleep_us(100);
    }
  }

  fclose(infile);
  return NULL;
}

void *effects_loop(void *arg) {
  while (atomic_load(&is_running) || /* Keep processing if buffer has frames */
         atomic_load_explicit(&ring_buffer_in.head, memory_order_relaxed) !=
             atomic_load_explicit(&ring_buffer_in.tail, memory_order_relaxed)) {

    VideoFrame *frame = NULL;

    if (!ring_pop(&ring_buffer_in, (void **)&frame)) {
      sleep_us(100);
      continue;
    }

    grayscale(frame);
    gs_invert(frame);

    while (!ring_push(&ring_buffer_out, frame)) {
      sleep_us(100);
    }
  }
  return NULL;
}

const char *output_filename = "test/output_1920x1080_i422.yuv";

void *consumer_loop(void *arg) {
  FILE *outfile = fopen(output_filename, "wb");
  if (!outfile) {
    printf("Failed to open output file.\n");
    return NULL;
  }

  while (
      atomic_load(&is_running) ||
      atomic_load_explicit(&ring_buffer_out.head, memory_order_relaxed) !=
          atomic_load_explicit(&ring_buffer_out.tail, memory_order_relaxed)) {

    VideoFrame *frame = NULL;

    if (!ring_pop(&ring_buffer_out, (void **)&frame)) {
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

int main(void) {
  printf("Initializing pipeline...\n");

  ring_init(&ring_buffer_in);
  ring_init(&ring_buffer_out);

  atomic_store(&is_running, true);

  pthread_t producer, worker, consumer;

  if (pthread_create(&producer, NULL, producer_loop, NULL) != 0) {
    printf("Error: Failed to create producer thread.\n");
    return 1;
  }
  if (pthread_create(&worker, NULL, effects_loop, NULL) != 0) {
    printf("Error: Failed to create worker thread.\n");
    return 1;
  }
  if (pthread_create(&consumer, NULL, consumer_loop, NULL) != 0) {
    printf("Error: Failed to create consumer thread.\n");
    return 1;
  }

  printf("Pipeline running. Processing video file...\n");

  pthread_join(producer, NULL);
  pthread_join(worker, NULL);
  pthread_join(consumer, NULL);

  printf("Processing complete. Output saved and pipeline shut down cleanly.\n");

  return 0;
}
