#pragma once
#include "img_stats.h"
#include <stddef.h>
#include <stdint.h>

/*
 * Auto visibility: continuously makes the pilot's picture as legible as the
 * camera's frame allows, so nobody has to tune effects by hand while
 * piloting. Off unless the config's "auto_visibility" is true; when on it
 * runs ahead of the effect chain, on the pilot's picture only (raw is never
 * touched -- autonomy and the "is it my filter?" check need the camera's
 * own frame).
 *
 * What underwater does to a picture, and what answers each:
 *
 *  1. Colour cast. Water absorbs red first, so everything goes blue-green
 *     and the camera's auto white balance runs out of range. A gray-world
 *     correction moves the average colour of well-exposed pixels back to
 *     within a few units of neutral -- no closer: a scene's own colours
 *     average out to a small cast that must be left alone. Scaled with
 *     brightness (a cast is a gain on the light, so it is stronger in bright
 *     pixels than in dark ones), then a mild saturation boost brings back
 *     colours the water muted.
 *
 *  2. A narrow brightness range. Scattered light lifts the blacks and dim
 *     light lowers the whites. The range between the 0.5th and 99.5th
 *     percentiles is stretched to full (percentiles, so a few specks of
 *     backscatter cannot defeat it -- the `contrast` effect's min/max can be),
 *     with the stretch capped so a near-uniform frame does not turn its
 *     noise into texture, and a gamma that puts the median at mid-gray.
 *
 *  3. Haze. Contrast is lost unevenly -- worse with distance, worse in the
 *     middle of the frame than near a light. Contrast-limited adaptive
 *     histogram equalization (CLAHE) on luma restores it tile by tile,
 *     with a clip limit that bounds how far any tile's noise is amplified,
 *     blended with the globally corrected picture.
 *
 * Everything it derives from a frame is smoothed over ~0.4s of capture
 * time, so the picture never flickers or pumps as fish swim through, but
 * follows a descent. Switching it on (or a gap in the frames) starts from
 * the current frame's values: no fade-in from "no correction".
 *
 * Not thread-safe: one AutoVis belongs to the effects thread.
 */

typedef struct AutoVis AutoVis;

AutoVis *auto_vis_create(void);
void auto_vis_destroy(AutoVis *av);

// Processes src (the raw planes) into dst (the work planes), steered by
// `stats`, which must have been computed from src. capture_ns is the
// frame's capture time (CLOCK_MONOTONIC ns; 0 if unknown), for smoothing.
// src and dst must not overlap. Returns false only if it could not
// allocate its working state, in which case dst is a plain copy of src.
bool auto_vis_apply(AutoVis *av, const ImgStats *stats,
                    const uint8_t *const src[3], uint8_t *const dst[3],
                    uint32_t width, uint32_t height, const size_t stride[3],
                    uint64_t capture_ns);
