#pragma once
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>

typedef enum {
  EFFECT_NONE = 0,
  EFFECT_GRAYSCALE,
  EFFECT_INVERT,
  EFFECT_THRESHOLD,
  EFFECT_TINT,
  EFFECT_SOBEL,
} EffectType;

typedef struct {
  EffectType effect;
  uint8_t threshold_value; // used when effect == EFFECT_THRESHOLD
  uint8_t tint_u;          // used when effect == EFFECT_TINT
  uint8_t tint_v;
  uint8_t tint_strength;
} Config;

/*
 * Watches a JSON config file and republishes a new Config every time it
 * changes on disk, so a separate process (a web UI on another device,
 * writing over scp/sftp) can retune the running pipeline without a
 * restart.
 *
 * Expected file, all keys optional (missing ones take the default shown):
 *   {
 *     "effect": "sobel",         // none|grayscale|invert|threshold|tint|sobel
 *     "threshold_value": 128,    // 0-255
 *     "tint_u": 90,              // 0-255
 *     "tint_v": 150,             // 0-255
 *     "tint_strength": 180       // 0-255
 *   }
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
 * hardcoded default (EFFECT_SOBEL) rather than failing.
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
 * parameters.
 */
const Config *config_current(const ConfigWatcher *watcher);
