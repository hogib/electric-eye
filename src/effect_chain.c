#include "effect_chain.h"
#include "conv.h"
#include "point_opps.h"
#include <string.h>

// Which of VideoFrame's three buffers currently holds the chain's state.
typedef enum { CHAIN_RAW, CHAIN_WORK, CHAIN_SPARE } ChainBuf;

// Two variants (const-source, mutable-dest) rather than one: assigning a
// mutable uint8_t* into a const uint8_t* array element is always a fine,
// standard single-level qualification add, but building one function
// that could serve either purpose would need a double-pointer parameter
// type where such a conversion gets much less forgiving.
static void chain_buf_planes_const(VideoFrame *frame, ChainBuf which,
                                   const uint8_t *out[3]) {
  uint8_t *const *src;
  switch (which) {
  case CHAIN_RAW: src = frame->raw_planes; break;
  case CHAIN_WORK: src = frame->planes; break;
  case CHAIN_SPARE: src = frame->spare_planes; break;
  default: return; // unreachable
  }
  out[0] = src[0];
  out[1] = src[1];
  out[2] = src[2];
}

static void chain_buf_planes_mut(VideoFrame *frame, ChainBuf which,
                                 uint8_t *out[3]) {
  uint8_t *const *src;
  switch (which) {
  case CHAIN_RAW: src = frame->raw_planes; break;
  case CHAIN_WORK: src = frame->planes; break;
  case CHAIN_SPARE: src = frame->spare_planes; break;
  default: return; // unreachable
  }
  out[0] = src[0];
  out[1] = src[1];
  out[2] = src[2];
}

static uint8_t *chain_buf_data(VideoFrame *frame, ChainBuf which) {
  switch (which) {
  case CHAIN_RAW: return frame->raw_data;
  case CHAIN_WORK: return frame->pixel_data;
  case CHAIN_SPARE: return frame->spare_data;
  default: return NULL; // unreachable
  }
}

void apply_effect_chain(VideoFrame *frame, const Config *cfg) {
  ChainBuf cur = CHAIN_RAW;
  size_t total =
      frame->plane_sizes[0] + frame->plane_sizes[1] + frame->plane_sizes[2];

  for (size_t i = 0; i < cfg->stage_count; ++i) {
    const EffectStage *stage = &cfg->stages[i];

    switch (stage->effect) {
    case EFFECT_BLUR:
    case EFFECT_SOBEL: {
      // Neighborhood ops ping-pong between work and spare, never
      // targeting whichever buffer 'cur' already is (that's what's being
      // read). The first neighborhood op in a chain has cur == RAW, so it
      // reads raw and writes work -- exactly the pre-chaining behavior
      // this replaces.
      ChainBuf dst = (cur == CHAIN_SPARE) ? CHAIN_WORK : CHAIN_SPARE;

      const uint8_t *src_planes[3];
      uint8_t *dst_planes[3];
      chain_buf_planes_const(frame, cur, src_planes);
      chain_buf_planes_mut(frame, dst, dst_planes);

      if (stage->effect == EFFECT_SOBEL) {
        sobel_edges(src_planes, dst_planes, frame->width, frame->height,
                   frame->stride);
      } else {
        gaussian_blur(src_planes, dst_planes, frame->width, frame->height,
                     frame->stride);
      }

      cur = dst;
      break;
    }

    case EFFECT_GRAYSCALE:
    case EFFECT_INVERT:
    case EFFECT_THRESHOLD:
    case EFFECT_TINT: {
      // These always mutate frame->planes (work) in place, exactly as
      // before chaining existed -- their signatures never changed. Bring
      // the chain's current state into work first if a prior stage left
      // it somewhere else.
      if (cur != CHAIN_WORK) {
        memcpy(frame->pixel_data, chain_buf_data(frame, cur), total);
        cur = CHAIN_WORK;
      }

      switch (stage->effect) {
      case EFFECT_GRAYSCALE:
        grayscale(frame);
        break;
      case EFFECT_INVERT:
        gs_invert(frame);
        break;
      case EFFECT_THRESHOLD:
        gs_threshold_by_value(frame, stage->threshold_value);
        break;
      case EFFECT_TINT:
        color_tint(frame, stage->tint_u, stage->tint_v, stage->tint_strength);
        break;
      default:
        break; // unreachable -- handled by the outer switch
      }
      break;
    }

    case EFFECT_NONE:
      break;
    }
  }

  // The rest of the pipeline always expects the result in frame->planes.
  // Falls through here for an empty chain (cur is still CHAIN_RAW -- copy
  // the untouched frame through unmodified) and for a chain whose last
  // stage was a neighborhood op that happened to leave the result in
  // spare rather than work.
  if (cur != CHAIN_WORK) {
    memcpy(frame->pixel_data, chain_buf_data(frame, cur), total);
  }
}
