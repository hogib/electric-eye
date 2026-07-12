#pragma once
#include <stdatomic.h>
#include <stdbool.h>

#define RING_BUFFER_SIZE 32

typedef struct FrameRingBuffer FrameRingBuffer;

void ring_init(FrameRingBuffer *rb);

bool ring_push(FrameRingBuffer *rb, void *frame_ptr);

bool ring_pop(FrameRingBuffer *rb, void **frame_ptr);
