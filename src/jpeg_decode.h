#pragma once
#include "video_frame.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/*
 * Shared MJPEG-to-I422 decode, used by both capture backends: v4l2_in.c
 * (a UVC webcam's MJPEG stream, straight out of the driver's MMAP buffer)
 * and rpicam_in.c (rpicam-vid's MJPEG on a pipe). The two get their bytes
 * from completely different places but need identical decoding, so this
 * lives in one place rather than being duplicated per backend.
 *
 * Decodes into a VideoFrame's raw_planes with no intermediate RGB
 * conversion and no extra copy, via libjpeg-turbo's
 * tjDecompressToYUVPlanes.
 *
 * Handles two real-world quirks transparently:
 *
 *  - Missing DHT (Huffman table) markers, which some UVC webcams omit
 *    entirely on the assumption the decoder already knows the standard
 *    tables. Each frame is scanned for an existing DHT and the standard
 *    tables are spliced in only when one is genuinely absent, so a camera
 *    that emits its own DHT pays only the cost of that scan.
 *
 *  - 4:2:0 chroma subsampling, which is what rpicam-vid's MJPEG encoder
 *    produces (it calls jpeg_set_defaults() without overriding the
 *    sampling factors). VideoFrame's layout is I422, so a 4:2:0 source has
 *    its chroma planes line-doubled during decode -- see JpegDecoder's
 *    note on the scratch buffers.
 */

typedef enum {
  JPEG_DECODE_OK,
  JPEG_DECODE_TRANSIENT_FAIL,  // malformed/corrupt payload -- worth a retry
  JPEG_DECODE_FORMAT_MISMATCH, // disagrees with what open() validated --
                               // not transient, don't retry
} JpegDecodeResult;

typedef struct JpegDecoder JpegDecoder;

/*
 * `width`/`height` are the *source* geometry -- what the encoder actually
 * sends, before any downscale. Every frame is checked against them, so a
 * camera that changes format mid-stream is caught rather than silently
 * producing garbage.
 *
 * Returns NULL on failure, having already reported the reason.
 */
JpegDecoder *jpeg_decoder_create(uint32_t width, uint32_t height);

void jpeg_decoder_destroy(JpegDecoder *dec);

/*
 * Decodes one complete JPEG (SOI..EOI) into frame->raw_planes. The frame's
 * own width/height are the *output* geometry: when they are smaller than
 * the source geometry given to jpeg_decoder_create(), libjpeg-turbo scales
 * during decompression to hit them -- by discarding high-frequency DCT
 * coefficients rather than decoding fully and resampling, so a half-size
 * decode is genuinely cheaper rather than merely cheaper downstream.
 */
JpegDecodeResult jpeg_decode_frame(JpegDecoder *dec, const uint8_t *data,
                                   size_t size, VideoFrame *frame);
