#pragma once
#include "video_frame.h"

/*
 * Sobel edge magnitude. Reads frame->raw_planes (the untouched camera
 * frame), writes frame->planes (the work buffer the consumer sends out).
 *
 * Source and destination are disjoint buffers, so unlike an in-place
 * neighborhood op there is no risk of a stage reading pixels it already
 * overwrote -- and no ordering constraint that would block parallelizing
 * the row loop.
 */
void sobel_edges(VideoFrame *frame);
