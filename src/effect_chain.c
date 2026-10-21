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

static bool is_point_op(EffectType e) {
  switch (e) {
  case EFFECT_NONE:
  case EFFECT_GRAYSCALE:
  case EFFECT_INVERT:
  case EFFECT_THRESHOLD:
  case EFFECT_TINT:
  case EFFECT_LIGHT:
    return true;
  default:
    return false;
  }
}

// True (with the shared value in *out) when every entry of lut is the same
// -- e.g. plain grayscale's composed chroma LUT, which maps every input to
// 128 regardless. When that happens the output doesn't depend on src at
// all, so apply_lut_plane can skip reading it entirely.
static bool lut_is_constant(const uint8_t lut[256], uint8_t *out) {
  for (int i = 1; i < 256; ++i) {
    if (lut[i] != lut[0])
      return false;
  }
  *out = lut[0];
  return true;
}

// src and dst need not be the same buffer -- apply_effect_chain calls this
// with src pointing at whichever chain buffer is current and dst pointing
// at work, so a touched plane goes straight from src to its LUT'd result in
// dst without a separate copy-then-overwrite step first. Safe in-place too
// (src == dst) since dst[i] only ever depends on src[i].
static void apply_lut_plane(const uint8_t *src, uint8_t *dst, size_t size,
                            const uint8_t lut[256]) {
  uint8_t constant;
  if (lut_is_constant(lut, &constant)) {
    memset(dst, constant, size);
    return;
  }
#pragma omp parallel for
  for (size_t i = 0; i < size; ++i) {
    dst[i] = lut[src[i]];
  }
}

// Folds one consecutive run of point-op stages (grayscale/invert/threshold/
// tint/none) starting at cfg->stages[*i] into three composed 256-entry
// LUTs -- one per channel -- and advances *i past the run.
//
// Each point op is a pure function of a single input byte (never depends on
// position or a neighbor pixel), and Y is independent of U/V: only invert/
// threshold ever read or write Y, only grayscale/tint ever read or write
// U/V. That means N point ops touching the same channel compose into one
// LUT for that channel exactly like function composition, built once
// up front instead of N full-frame passes at frame time. lut_c[v] tracks
// "what does byte value v become after every stage seen so far in this
// run" -- composing a new stage is `lut_c[v] = stage_op(lut_c[v])` for
// every v, mirroring how the unfused code would have re-read whatever the
// previous stage just wrote.
//
// touched_y/u/v come back true only if some stage in the run actually
// touches that channel, so e.g. a Y-only run (invert, threshold) never
// pays for a no-op pass over U/V.
static void fuse_point_op_run(const Config *cfg, size_t *i, uint8_t lut_y[256],
                              uint8_t lut_u[256], uint8_t lut_v[256],
                              bool *touched_y, bool *touched_u,
                              bool *touched_v) {
  for (int v = 0; v < 256; ++v) {
    lut_y[v] = (uint8_t)v;
    lut_u[v] = (uint8_t)v;
    lut_v[v] = (uint8_t)v;
  }
  *touched_y = false;
  *touched_u = false;
  *touched_v = false;

  while (*i < cfg->stage_count && is_point_op(cfg->stages[*i].effect)) {
    const EffectStage *s = &cfg->stages[*i];

    switch (s->effect) {
    case EFFECT_GRAYSCALE:
      // Every chroma byte becomes 128 regardless of what it was, so the
      // composed LUT is just a constant, discarding whatever any earlier
      // stage in this run did.
      for (int v = 0; v < 256; ++v) {
        lut_u[v] = 128;
        lut_v[v] = 128;
      }
      *touched_u = true;
      *touched_v = true;
      break;

    case EFFECT_INVERT:
      // `255 - x`.
      for (int v = 0; v < 256; ++v) {
        lut_y[v] = (uint8_t)(255 - lut_y[v]);
      }
      *touched_y = true;
      break;

    case EFFECT_THRESHOLD:
      // `x < tval ? 0 : 255`.
      for (int v = 0; v < 256; ++v) {
        lut_y[v] = (lut_y[v] < s->threshold_value) ? 0 : 255;
      }
      *touched_y = true;
      break;

    case EFFECT_TINT:
      // `x + (target - x) * strength / 255` -- a lerp toward target,
      // including C's truncating integer division. tint_strength == 0 is
      // a no-op, left as the identity LUT.
      if (s->tint_strength != 0) {
        for (int v = 0; v < 256; ++v) {
          int32_t diff_u = (int32_t)s->tint_u - (int32_t)lut_u[v];
          lut_u[v] = (uint8_t)(lut_u[v] + (diff_u * s->tint_strength) / 255);
          int32_t diff_v = (int32_t)s->tint_v - (int32_t)lut_v[v];
          lut_v[v] = (uint8_t)(lut_v[v] + (diff_v * s->tint_strength) / 255);
        }
        *touched_u = true;
        *touched_v = true;
      }
      break;

    case EFFECT_LIGHT:
      // Offset (Y) / scale-around-128 (U, V). light_level == 128 is a
      // no-op, left as the identity LUT.
      if (s->light_level != 128) {
        int32_t offset = (int32_t)s->light_level - 128;
        float chroma_scale = (float)s->light_level / 128.0f;

        for (int v = 0; v < 256; ++v) {
          int32_t ny = (int32_t)lut_y[v] + offset;
          if (ny < 0)
            ny = 0;
          if (ny > 255)
            ny = 255;
          lut_y[v] = (uint8_t)ny;
        }
        for (int v = 0; v < 256; ++v) {
          float nu = 128.0f + ((float)lut_u[v] - 128.0f) * chroma_scale;
          if (nu < 0.0f)
            nu = 0.0f;
          if (nu > 255.0f)
            nu = 255.0f;
          lut_u[v] = (uint8_t)(nu + 0.5f);

          float nvv = 128.0f + ((float)lut_v[v] - 128.0f) * chroma_scale;
          if (nvv < 0.0f)
            nvv = 0.0f;
          if (nvv > 255.0f)
            nvv = 255.0f;
          lut_v[v] = (uint8_t)(nvv + 0.5f);
        }
        *touched_y = true;
        *touched_u = true;
        *touched_v = true;
      }
      break;

    case EFFECT_NONE:
    default:
      break; // no channel touched
    }

    ++*i;
  }
}

// Does stage `e` overwrite chroma without reading it? Edge maps reset both
// chroma planes to neutral, and grayscale's composed chroma LUT is a
// constant (see lut_is_constant), so whatever chroma reached them is never
// looked at.
static bool kills_chroma(EffectType e) {
  return e == EFFECT_SOBEL || e == EFFECT_LOG || e == EFFECT_CANNY ||
         e == EFFECT_GRAYSCALE;
}

// Does stage `e` pass chroma through untouched (copied at most)?
static bool passes_chroma(EffectType e) {
  return e == EFFECT_NONE || e == EFFECT_INVERT || e == EFFECT_THRESHOLD ||
         e == EFFECT_CONTRAST;
}

// chroma_live[i]: whether the chroma stage i produces is ever looked at --
// by a later stage, or by the output once the chain ends. Computed back to
// front. A blur whose chroma is dead blurs luma only: in the shipped
// blur -> sobel chain that is half the blur's work, thrown away by sobel's
// chroma reset on every frame. Tint and light are treated as reading
// chroma even at their no-op settings; being conservative there only costs
// a skipped optimization, never a wrong frame.
static void compute_chroma_liveness(const Config *cfg,
                                    bool chroma_live[max_chain_stages]) {
  bool live = true; // the output itself shows chroma
  for (size_t i = cfg->stage_count; i-- > 0;) {
    chroma_live[i] = live;
    EffectType e = cfg->stages[i].effect;
    if (kills_chroma(e))
      live = false;
    else if (e == EFFECT_BLUR)
      ; // reads chroma exactly when its own chroma output is live
    else if (!passes_chroma(e))
      live = true; // tint, light
  }
}

void apply_effect_chain(VideoFrame *frame, const Config *cfg) {
  ChainBuf cur = CHAIN_RAW;

  // The previous trip through the pool may have left work pointing at raw
  // (an empty chain, see the end of this function). Every stage below
  // needs work and spare to be real buffers of their own.
  vf_restore_roles(frame);

  bool chroma_live[max_chain_stages];
  compute_chroma_liveness(cfg, chroma_live);

  size_t i = 0;
  while (i < cfg->stage_count) {
    const EffectStage *stage = &cfg->stages[i];

    if (stage->effect == EFFECT_BLUR || stage->effect == EFFECT_SOBEL ||
        stage->effect == EFFECT_LOG || stage->effect == EFFECT_CANNY) {
      // Neighborhood ops ping-pong between work and spare, never
      // targeting whichever buffer 'cur' already is (that's what's being
      // read). The first neighborhood op in a chain has cur == RAW, so it
      // reads raw and writes spare; a chain that ends there is fixed up by
      // a pointer swap at the bottom of this function, not a copy.
      ChainBuf dst = (cur == CHAIN_SPARE) ? CHAIN_WORK : CHAIN_SPARE;

      const uint8_t *src_planes[3];
      uint8_t *dst_planes[3];
      chain_buf_planes_const(frame, cur, src_planes);
      chain_buf_planes_mut(frame, dst, dst_planes);

      if (stage->effect == EFFECT_SOBEL) {
        sobel_edges(src_planes, dst_planes, frame->width, frame->height,
                   frame->stride, stage->sobel_threshold);
      } else if (stage->effect == EFFECT_CANNY) {
        canny_edges(src_planes, dst_planes, frame->width, frame->height,
                    frame->stride, stage->canny_strength, stage->canny_low,
                    stage->canny_high);
      } else if (stage->effect == EFFECT_LOG) {
        log_edges(src_planes, dst_planes, frame->width, frame->height,
                  frame->stride, stage->log_strength, stage->log_threshold);
      } else if (chroma_live[i]) {
        gaussian_blur(src_planes, dst_planes, frame->width, frame->height,
                     frame->stride, stage->blur_strength);
      } else {
        gaussian_blur_luma(src_planes, dst_planes, frame->width,
                           frame->height, frame->stride, stage->blur_strength);
      }

      cur = dst;
      ++i;
      continue;
    }

    if (stage->effect == EFFECT_CONTRAST) {
      // Unlike grayscale/invert/threshold/tint, this LUT depends on the
      // actual min/max luma present in the frame *at this point in the
      // chain* -- gs_contrast_normalize scans for it itself every call, so
      // unlike the point ops below it can't be folded into a LUT composed
      // ahead of any real pixel data. It only ever touches Y, though, so
      // unlike the old code there's no need to bring the whole frame into
      // work first: gs_contrast_normalize reads straight from cur's Y plane
      // and writes straight into work's, and only the untouched U/V planes
      // need an explicit copy (when cur isn't already work).
      const uint8_t *src_planes[3];
      chain_buf_planes_const(frame, cur, src_planes);
      if (cur != CHAIN_WORK) {
        memcpy(frame->planes[1], src_planes[1], frame->plane_sizes[1]);
        memcpy(frame->planes[2], src_planes[2], frame->plane_sizes[2]);
      }
      gs_contrast_normalize(src_planes[0], frame->planes[0], frame->width,
                            frame->height, frame->stride[0]);
      cur = CHAIN_WORK;
      ++i;
      continue;
    }

    // A run of one or more consecutive point-op stages: fold them into one
    // LUT per channel (see fuse_point_op_run) instead of dispatching each
    // stage to its own full-frame pass.
    uint8_t lut_y[256], lut_u[256], lut_v[256];
    bool touched_y, touched_u, touched_v;
    fuse_point_op_run(cfg, &i, lut_y, lut_u, lut_v, &touched_y, &touched_u,
                      &touched_v);

    // Touched planes go straight from cur's buffer to their LUT'd result in
    // work -- no separate copy-then-overwrite first. Only an untouched
    // plane needs a plain copy (when cur isn't already work), since nothing
    // else is going to write it before the chain moves on.
    const uint8_t *src_planes[3];
    chain_buf_planes_const(frame, cur, src_planes);

    if (touched_y) {
      apply_lut_plane(src_planes[0], frame->planes[0], frame->plane_sizes[0],
                      lut_y);
    } else if (cur != CHAIN_WORK) {
      memcpy(frame->planes[0], src_planes[0], frame->plane_sizes[0]);
    }
    if (touched_u) {
      apply_lut_plane(src_planes[1], frame->planes[1], frame->plane_sizes[1],
                      lut_u);
    } else if (cur != CHAIN_WORK) {
      memcpy(frame->planes[1], src_planes[1], frame->plane_sizes[1]);
    }
    if (touched_v) {
      apply_lut_plane(src_planes[2], frame->planes[2], frame->plane_sizes[2],
                      lut_v);
    } else if (cur != CHAIN_WORK) {
      memcpy(frame->planes[2], src_planes[2], frame->plane_sizes[2]);
    }
    cur = CHAIN_WORK;
  }

  // The rest of the pipeline always expects the result in frame->planes.
  // Two cases leave it elsewhere, and both are fixed by moving pointers
  // rather than copying 1.8MB at 720p:
  //  - a chain whose last stage was a neighborhood op that wrote spare:
  //    swap the work and spare roles;
  //  - an empty chain (cur is still CHAIN_RAW): point work at raw itself.
  //    Nothing downstream writes work, so sharing the block is safe, and
  //    vf_restore_roles() at the top of the next trip separates them again.
  if (cur == CHAIN_SPARE)
    vf_swap_work_spare(frame);
  else if (cur == CHAIN_RAW)
    vf_set_work(frame, frame->raw_data);
}
