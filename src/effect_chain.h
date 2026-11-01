#pragma once
#include "auto_vis.h"
#include "config.h"
#include "img_stats.h"
#include "video_frame.h"

/*
 * Runs cfg's effect chain over frame, applying each stage in order. Always
 * leaves the result in frame->planes (the work buffer the rest of the
 * pipeline -- consumer_loop, v4l2_out_write -- already expects), regardless
 * of which of frame's three buffers (raw/work/spare) the chain's stages
 * actually passed the frame through internally to get there.
 *
 * frame->raw_planes is never written to, whatever the chain does -- see
 * conv.h's neighborhood-op contract and video_frame.h's note on why a
 * third (spare) buffer exists at all.
 */
void apply_effect_chain(VideoFrame *frame, const Config *cfg);

/*
 * The same, with auto visibility (auto_vis.h) run first when
 * cfg->auto_visibility is set: it reads raw and writes work, and the chain
 * continues from work, so a neighborhood op after it sees the corrected
 * picture. `stats` must describe frame->raw_planes. Sets frame->stats.auto_on
 * to whether it ran.
 */
void apply_effect_chain_auto(VideoFrame *frame, const Config *cfg,
                             AutoVis *av, const ImgStats *stats);
