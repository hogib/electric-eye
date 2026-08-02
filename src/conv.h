#pragma once
#include <stddef.h>
#include <stdint.h>

/*
 * Both functions below share one shape so effect_chain.c can invoke either
 * uniformly as "a neighborhood op": 3 source planes, 3 destination planes,
 * frame dimensions, and per-plane stride. src and dst must not alias --
 * each reads a neighborhood of src while writing dst, so a stage that
 * targeted its own source would risk reading pixels it already overwrote.
 * (Point ops don't have this restriction and don't take this shape --
 * see point_opps.h.)
 */

void sobel_edges(const uint8_t *const src_planes[3], uint8_t *const dst_planes[3],
                 uint32_t width, uint32_t height, const size_t stride[3]);

void gaussian_blur(const uint8_t *const src_planes[3], uint8_t *const dst_planes[3],
                   uint32_t width, uint32_t height, const size_t stride[3]);
