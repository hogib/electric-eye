#ifndef VIDPOOL_H
#define VIDPOOL_H

#include "video_frame.h"

typedef struct VideoBufferPool VideoBufferPool;

VideoBufferPool *vpool_create(int num_frames, int width, int height);

void vpool_destroy(VideoBufferPool *pool);

VideoFrame *vpool_acquire_frame(VideoBufferPool *pool);

void vpool_release_frame(VideoBufferPool *pool, VideoFrame *frame);

#endif
