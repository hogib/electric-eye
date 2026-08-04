#pragma once
#include "video_frame.h"
#include <stdint.h>

constexpr uint8_t y_plain_max_jpeg = 255u;
constexpr uint8_t y_plain_min_jpeg = 0u;

constexpr uint8_t y_plain_max_std = 235u;
constexpr uint8_t y_plain_min_std = 16u;

void grayscale(VideoFrame *frame);

void gs_contrast_normalize(VideoFrame *frame);

void gs_threshold_by_value(VideoFrame *frame, uint8_t tval);

void gs_invert(VideoFrame *frame);

void color_tint(VideoFrame *frame, uint8_t target_u, uint8_t target_v,
             uint8_t strength);

void color_light(VideoFrame *frame, uint8_t level);
