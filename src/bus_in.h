#pragma once
#include "video_frame.h"
#include <stdbool.h>
#include <stdint.h>

/*
 * Capture backend that reads a frame bus stream (fbus.h) instead of a
 * camera: frames another program publishes -- hellion decoding the ROV's
 * camera topside, for one.
 *
 * Same lifecycle as the camera backends, so producer_loop treats it the
 * same way: a stream that is missing, or goes away (its writer stopped or
 * restarted), is a "camera unavailable" handled by the reconnect loop.
 *
 * The pipeline's geometry comes from the stream (see bus_in_wait()). A
 * writer that comes back at a different size cannot be followed -- the
 * frame pool is sized once -- so bus_in_open() reports that separately and
 * eeye exits for systemd to restart it at the new size.
 */

typedef struct BusIn BusIn;

/*
 * Blocks until stream `name` exists and has a live writer, then reports
 * its geometry. Logs while it waits (first, then every 30s). Returns false
 * if *is_running goes false first.
 */
bool bus_in_wait(const char *name, const bool *is_running, uint32_t *width,
                 uint32_t *height);

/*
 * Attaches to stream `name`, which must be width x height. Returns NULL
 * if it is not available; if it is available at another size, also sets
 * *wrong_size. `log_failure` says whether to explain a failure (the
 * caller throttles it across retries).
 */
BusIn *bus_in_open(const char *name, uint32_t width, uint32_t height,
                   bool log_failure, bool *wrong_size);

/*
 * Waits for the next frame and copies it into frame->raw_planes, with its
 * capture time in frame->capture_ns. A stream that is alive but idle is
 * waited out (and logged after a few seconds); false means the stream is
 * gone, or *is_running went false.
 */
bool bus_in_capture(BusIn *in, VideoFrame *frame, const bool *is_running);

void bus_in_close(BusIn *in);
