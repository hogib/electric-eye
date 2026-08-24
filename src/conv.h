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

// threshold: gradient magnitudes below this are clamped to 0 before the
// chroma reset. 0 keeps every magnitude as computed, identical to no
// thresholding at all.
void sobel_edges(const uint8_t *const src_planes[3], uint8_t *const dst_planes[3],
                 uint32_t width, uint32_t height, const size_t stride[3],
                 uint8_t threshold);

/*
 * Laplacian of Gaussian, rendered as Marr-Hildreth zero-crossings: thin,
 * closed 1px contours rather than Sobel's thicker gradient ridges.
 *
 * strength: Gaussian passes before the Laplacian, so it sets sigma (which
 * grows as sqrt(strength)). 0 and 1 both mean a single pass, matching
 * gaussian_blur's strength. Higher values reject progressively finer
 * detail -- the knob that matters underwater, where backscatter is
 * high-frequency noise.
 *
 * threshold: how steep the response has to be across a crossing to count
 * as an edge. 0 marks every sign change, including noise grazing zero in
 * otherwise flat regions.
 *
 * Requires stride[0] == width (how vf_create lays out the luma plane); it
 * returns without writing dst if that does not hold, rather than
 * corrupting memory.
 */
void log_edges(const uint8_t *const src_planes[3],
               uint8_t *const dst_planes[3], uint32_t width, uint32_t height,
               const size_t stride[3], uint8_t strength, uint8_t threshold);

// strength: how many times to repeat the 5-tap pass. 0 and 1 both mean a
// single pass (src read once, dst written once); each pass after the
// first re-blurs dst in place -- safe because blur_plane() fully consumes
// its source into an internal scratch buffer before writing dst at all
// (see conv.c), so src and dst may be the same buffer.
void gaussian_blur(const uint8_t *const src_planes[3], uint8_t *const dst_planes[3],
                   uint32_t width, uint32_t height, const size_t stride[3],
                   uint8_t strength);
