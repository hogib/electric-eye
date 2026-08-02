#pragma once
#include "config.h"
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
