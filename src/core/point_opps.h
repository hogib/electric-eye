#pragma once
#include "video_frame.h"

void grayscale(VideoFrame *frame);

void gs_contrast_normalize(VideoFrame *frame);

void gs_threshold_by_value(VideoFrame *frame);
