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

/*
 * Canny edge detection: Gaussian smooth, gradient with direction,
 * non-maximum suppression, then hysteresis. Output is a binary edge map of
 * single-pixel-wide contours, filtered by connectivity -- a weak edge
 * survives only where it joins a strong one, which is what separates this
 * from a plain threshold on sobel's magnitude.
 *
 * strength: Gaussian passes. Unlike the other operators' strength knobs
 * this is never zero -- 0 is treated as 1 -- because the smoothing is what
 * bounds hysteresis's cost, not merely a quality setting. Measured at
 * 1280x720, hysteresis on unsmoothed noise costs 10.8ms against 0.5ms
 * after a single 0.07ms blur pass.
 *
 * low / high: hysteresis thresholds on gradient magnitude. At or above
 * `high` is an edge outright; between `low` and `high` only when connected
 * to one. Passed in either order (they are swapped if reversed). A `high`
 * of 0 yields an empty map, since nothing seeds the fill.
 *
 * Requires stride[0] == width (how vf_create lays out the luma plane); it
 * returns without writing dst if that does not hold, rather than
 * corrupting memory.
 */
void canny_edges(const uint8_t *const src_planes[3],
                 uint8_t *const dst_planes[3], uint32_t width, uint32_t height,
                 const size_t stride[3], uint8_t strength, uint8_t low,
                 uint8_t high);

// strength: how many times to repeat the 5-tap pass. 0 and 1 both mean a
// single pass (src read once, dst written once); each pass after the
// first re-blurs dst in place -- safe because blur_plane() fully consumes
// its source into an internal scratch buffer before writing dst at all
// (see conv.c), so src and dst may be the same buffer.
void gaussian_blur(const uint8_t *const src_planes[3], uint8_t *const dst_planes[3],
                   uint32_t width, uint32_t height, const size_t stride[3],
                   uint8_t strength);
