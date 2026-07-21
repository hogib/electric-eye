#include "video_frame.h"

/*
 *@params:
 *  planes: Pointer to stream data. I422 Input stream expected.
 *  stride: Plane stride, refer to YUV I422 format.
 *  height: Height of video frame.
 *  width: Width of video frame.
 *  pts: Presentation time stamp. Feed to consumer to properly decode video.
 **/

/*
 * Constructor for an I422 VideoFrame.
 * Returns a pointer to the allocated frame, or NULL on failure.
 */
VideoFrame *video_frame_create_i422(uint32_t width, uint32_t height,
                                    int64_t pts) {
  if (width == 0 || height == 0) {
    return NULL;
  }

  VideoFrame *frame = (VideoFrame *)calloc(1, sizeof(VideoFrame));

  if (!frame) {
    return NULL;
  }

  frame->width = width;
  frame->height = height;
  frame->pts = pts;

  // I422 Chroma width is halved.
  uint32_t chroma_width = (width + 1) / 2;

  frame->stride[0] = width;        // Y plane stride
  frame->stride[1] = chroma_width; // U plane stride
  frame->stride[2] = chroma_width; // V plane stride

  size_t y_size = (size_t)frame->stride[0] * height;
  size_t u_size = (size_t)frame->stride[1] * height;
  size_t v_size = (size_t)frame->stride[2] * height;

  frame->pixel_data = (uint8_t *)malloc(y_size + u_size + v_size);
  if (!frame->pixel_data) {
    free(frame);
    return NULL;
  }

  // Each plane is an offset of pixel data.
  frame->planes[0] = frame->pixel_data;
  frame->planes[1] = frame->pixel_data + y_size;
  frame->planes[2] = frame->pixel_data + y_size + u_size;

  return frame;
}

/*
 * Destructor for the VideoFrame
 */
void video_frame_free(VideoFrame *frame) {
  if (!frame)
    return;

  if (frame->pixel_data) {
    free(frame->pixel_data);
  }

  free(frame);
}
