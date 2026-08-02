#pragma once
#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef enum {
  EFFECT_NONE = 0,
  EFFECT_GRAYSCALE,
  EFFECT_INVERT,
  EFFECT_THRESHOLD,
  EFFECT_TINT,
  EFFECT_SOBEL,
  EFFECT_BLUR,
} EffectType;

// One entry in an effect chain. Every stage carries its own parameters
// (rather than one shared set for the whole Config) so e.g. two EFFECT_TINT
// stages in the same chain, or a threshold before a tint, each get their
// own values.
typedef struct {
  EffectType effect;
  uint8_t threshold_value; // used when effect == EFFECT_THRESHOLD
  uint8_t tint_u;          // used when effect == EFFECT_TINT
  uint8_t tint_v;
  uint8_t tint_strength;
} EffectStage;

// Bounds the chain to a fixed-size array rather than something dynamically
// allocated: Config is copied by value into ConfigWatcher's ring buffer
// (see config.c) on every reload, and keeping it a plain, pointer-free POD
// struct is what makes that ring safe without any per-slot allocation or
// cleanup. 8 is comfortably past any chain that makes visual sense.
constexpr size_t max_chain_stages = 8;

// Shared with consumer_loop's own local copy of the current recording
// path (see video_threads.c), so the two can never drift out of sync.
constexpr size_t max_record_path_len = 256;

typedef struct {
  EffectStage stages[max_chain_stages];
  size_t stage_count; // may be 0: an empty chain is a valid pass-through

  // Recording tap: the producer-filled, untouched camera frame (raw_data
  // in video_frame.h) is written here on every frame once this is
  // non-empty, independent of whatever the effect chain above does to the
  // frame that's actually sent to the virtual camera -- see consumer_loop.
  // Empty string means recording is off. Written as raw I422 with no
  // container; see video_threads.c's own note on how to play it back.
  char record_path[max_record_path_len];
} Config;

/*
 * Watches a JSON config file and republishes a new Config every time it
 * changes on disk, so a separate process (a web UI on another device,
 * writing over scp/sftp) can retune the running pipeline without a
 * restart.
 *
 * Expected file:
 *   {
 *     "chain": [
 *       {"effect": "blur"},
 *       {"effect": "sobel"},
 *       {"effect": "tint", "tint_u": 90, "tint_v": 150, "tint_strength": 180}
 *     ],
 *     "record_path": "/opt/electric-eye/recordings/session.raw"
 *   }
 *
 * "chain" is a list of stages, applied in order; each is one of
 * none|grayscale|invert|threshold|tint|sobel|blur, plus that effect's own
 * parameters where it takes any (threshold_value for threshold; tint_u/
 * tint_v/tint_strength for tint -- all 0-255). An empty chain ([]) is a
 * valid pass-through. "record_path" is optional; omit it (or set "") to
 * leave recording off.
 *
 * Writer contract: write to a temp file in the same directory, then
 * rename() it onto the target path. A plain in-place overwrite can be
 * observed mid-write and read as a truncated, invalid file -- rename is
 * atomic from a reader's point of view. inotify watches the directory
 * rather than the file itself for exactly this reason: a watch on the
 * file's inode goes stale the moment rename() replaces it.
 */
typedef struct ConfigWatcher ConfigWatcher;

/*
 * Loads `path` once, synchronously, before returning -- config_current()
 * is guaranteed to return a valid snapshot immediately, never NULL. If the
 * file is missing or malformed at startup, logs why and falls back to a
 * hardcoded default (a single-stage EFFECT_SOBEL chain) rather than
 * failing.
 *
 * Then starts a background thread that reloads on every subsequent write.
 * If the watch itself can't be established (e.g. the containing directory
 * doesn't exist), that failure degrades gracefully too: the process keeps
 * running with whatever was loaded at startup, just without live reload,
 * and says so on stdout. The only way this returns NULL is an allocation
 * failure.
 *
 * *is_running follows the same convention as ProducerArgs/WorkerArgs/
 * ConsumerArgs: the caller flips it to false to signal every thread,
 * including this one, to shut down.
 */
ConfigWatcher *config_watch_start(const char *path, atomic_bool *is_running);

void config_watch_stop(ConfigWatcher *watcher);

/*
 * The live config, as of whenever this is called. Call once per frame, not
 * once per effect stage -- one snapshot per frame guarantees a config push
 * can't land mid-frame and split a single output frame between old and new
 * parameters (both for the effect chain and for consumer_loop's own
 * separate read of record_path).
 */
const Config *config_current(const ConfigWatcher *watcher);
