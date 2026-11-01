#pragma once
#include <stddef.h>
#include <stdint.h>

/*
 * I422 frames to JPEG, for the browser preview and snapshots. The frame
 * bus is full range, which is what JPEG (JFIF) means by default, so the
 * planes go in as they are. One encoder per thread: not thread-safe.
 */

typedef struct JpegEnc JpegEnc;

JpegEnc *jpeg_enc_create(void);

// Encodes one frame. Returns the JPEG (owned by the encoder, valid until
// its next call) and its size, or NULL on failure.
const uint8_t *jpeg_enc_i422(JpegEnc *e, const uint8_t *const planes[3],
                             uint32_t width, uint32_t height, int quality,
                             size_t *size);

void jpeg_enc_destroy(JpegEnc *e);
