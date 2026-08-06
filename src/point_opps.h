#pragma once
#include <stddef.h>
#include <stdint.h>

constexpr uint8_t y_plain_max_jpeg = 255u;
constexpr uint8_t y_plain_min_jpeg = 0u;

constexpr uint8_t y_plain_max_std = 235u;
constexpr uint8_t y_plain_min_std = 16u;

// src/dst need not be the same buffer -- effect_chain.c calls this with
// src pointing at whichever chain buffer is current and dst pointing at
// work, so the LUT built from src's scan applies straight into dst without
// a separate copy-then-overwrite step first. Safe in-place too (src == dst)
// since dst[y][x] only ever depends on src[y][x], never a neighbor.
void gs_contrast_normalize(const uint8_t *src, uint8_t *dst, uint32_t width,
                           uint32_t height, size_t stride);
