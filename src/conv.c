#include "conv.h"
#include <string.h>

/*
 * TODO(you): replace the body below with the real Sobel gradient.
 *
 * Contract sobel_edges() must satisfy:
 *
 *   - Read only frame->raw_planes[0..2]. Never touch frame->planes as a
 *     source -- that's the write side.
 *
 *   - Write the *entirety* of frame->planes[0..2]. Nothing upstream
 *     initializes the work buffer anymore now that raw and work are
 *     separate allocations (see video_frame.h), so any pixel this function
 *     doesn't write is whatever garbage was left in that pool slot two
 *     cycles ago.
 *
 *   - planes[1]/planes[2] (chroma) should end up flat at 128 -- a pure
 *     edge map has no color, same as grayscale() in point_opps.c.
 *
 *   - planes[0] (Y) should hold |Gx| + |Gy| clamped to [0, 255] for every
 *     pixel, computed from the 3x3 neighborhood around that pixel in
 *     raw_planes[0]. Use int16_t/int32_t intermediates -- Gx and Gy each
 *     range roughly +-1020, well outside uint8_t.
 *
 *   - The 1px border has no full 3x3 neighborhood. Decide explicitly:
 *     zero it, or replicate the nearest interior row/column. Leaving it
 *     unwritten fails the "write the entirety" rule above.
 *
 * Because raw and planes are different buffers, this loop can run with
 * #pragma omp parallel for over rows with no banding/ordering concerns --
 * there's nothing here like the rolling-scratch write-behind trick that
 * would need loop-carried state.
 *
 * What's below is a placeholder, not a step toward the real thing: raw Y
 * copied straight through, chroma neutralized. It satisfies the contract
 * (builds, runs, produces real output) so the raw/work plumbing can be
 * verified before the gradient math exists. Delete it once the real loop
 * covers every pixel.
 */
void sobel_edges(VideoFrame *frame) {
  if (!frame)
    return;

  memcpy(frame->planes[0], frame->raw_planes[0], frame->plane_sizes[0]);
  memset(frame->planes[1], 128, frame->plane_sizes[1]);
  memset(frame->planes[2], 128, frame->plane_sizes[2]);
}
