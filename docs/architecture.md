# Architecture

Threads, frames, and how a frame gets from the sensor to the loopback device.

## Threads

Five, all created in `main()` except the config watcher, which
`config_watch_start()` creates for itself.

| Thread | Entry point | Job |
|---|---|---|
| producer | `producer_loop` | Owns the camera. Captures into a frame, pushes it downstream. |
| effects | `effects_loop` | Runs the effect chain on each frame. |
| consumer | `consumer_loop` | Writes to the virtual camera; runs the recording and preview taps. |
| stats | `stats_loop` | Logs ring occupancy every 5s. Purely diagnostic. |
| config watcher | inside `config.c` | Watches the config file with inotify, republishes on change. |

The stats thread is optional: if `pthread_create` fails for it, `main()` logs and
carries on. The other three are fatal to start.

Only the producer thread ever touches the camera device, and only the consumer thread
ever touches the loopback device. That is what makes the reconnect logic tractable —
see [Camera reconnection](#camera-reconnection).

## Frame flow

Three ring buffers connect the stages, and frames circulate rather than being
allocated:

```
                  ring_buffer_free (the pool)
                   ▲                        │
                   │ frame returned          │ empty frame
                   │                         ▼
            ┌──────────────┐          ┌──────────────┐
            │   consumer   │          │   producer   │
            └──────────────┘          └──────────────┘
                   ▲                         │
                   │ ring_buffer_out         │ ring_buffer_in
                   │                         ▼
                          ┌──────────────┐
                          │   effects    │
                          └──────────────┘
```

A frame's round trip:

1. Producer pops an empty frame from **free**.
2. Producer captures into it (writing `raw_planes`), stamps `pts`, pushes to **in**.
3. Effects pops from **in**, runs the chain (writing `planes`), pushes to **out**.
4. Consumer pops from **out**, writes to the loopback device, taps recording and
   preview, pushes the frame back to **free**.

The pool is allocated once, up front, and never grows. `ring_buffer_size - 1` frames,
i.e. 31 — see [Ring buffers](#ring-buffers) for why one slot is missing.

**A frame must never be abandoned.** If a frame is dropped instead of returned to
**free**, it leaks out of the pool permanently and the pipeline runs one frame poorer
until restart. Every push back to the free list passes `NULL` for `is_running`
specifically so that it completes even during shutdown:

```c
// NULL: this frame must go back to the pool regardless of is_running --
// abandoning it here would leak it out of the pool for the rest of the
// process's life.
ring_push_wait(args->ring_buffer_free, frame, NULL);
```

If you add a code path that takes a frame, audit every exit from it.

## VideoFrame

Defined in `video_frame.h`. One frame carries **three** full-size buffers with
identical layout:

| Buffer | Accessors | Written by |
|---|---|---|
| `raw_data` / `raw_planes` | the untouched camera frame | producer only |
| `pixel_data` / `planes` | the work buffer — what gets sent out | effects |
| `spare_data` / `spare_planes` | scratch for chained neighborhood ops | effects |

All three share one set of `plane_sizes[3]` and `stride[3]`.

**Pixel format is I422 throughout** (planar YUV 4:2:2): a full-resolution Y plane, and
U and V planes at half width, full height. So `stride[0] == width` and
`stride[1] == stride[2] == width / 2`.

Two consequences that constrain a lot of the code:

- **Width must be even**, or the chroma halving is not exact. Enforced in config
  validation.
- **`stride[0] == width` exactly** — no row padding. Several scratch buffers
  (`log_blurred`, `canny_blurred`) are allocated tightly packed at `width * height` and
  would corrupt memory if that stopped being true. `log_edges` and `canny_edges` both
  check and bail rather than assume it.

`raw` exists so that neighborhood operators have a stable source: a filter reading a
3×3 neighborhood must not read pixels a previous stage already overwrote. `spare`
exists so that *two* chained neighborhood ops can ping-pong — see
[effects.md](effects.md).

## Ring buffers

`frame_ring_buffer.h`. 32 slots, power of two (there is a `#error` if you change that
to a non-power-of-two).

**Usable capacity is 31, not 32.** One slot is always kept empty so that "full" and
"empty" are distinguishable from head/tail alone.

The synchronization is deliberately asymmetric:

- **Push takes a mutex.** More than one thread pushes to the free-list ring — the
  consumer on every frame, and the producer when a capture fails and it has to return
  the frame it borrowed. Two threads racing the same read-check-write on `head` would
  corrupt the ring.
- **Pop is lock-free.** Every ring in this pipeline has exactly one reader, so the pop
  side needs no lock.

If you ever add a second reader to a ring, that assumption breaks and pop needs a lock
too.

Two semaphores (`filled`, `empty`) sit on top as pure wakeup signals. They never gate
correctness — the head/tail scheme does that — they only decide whether a waiter blocks
or spins.

`ring_push_wait` / `ring_pop_wait` take an `atomic_bool *is_running`. They wake up on a
bounded timeout (~200ms) to re-check it, so shutdown is noticed promptly rather than
deadlocking on an empty ring. Passing `NULL` means "this must complete regardless",
which is the frame-leak guard above.

The diagnostic counters (`high_water`, `full_stalls`, `empty_stalls`) are relaxed
atomics and never influence behavior. Note the stall counters count ~200ms *wait
windows*, not events — a stall count of 5 means roughly a second spent blocked, not
five blocked pushes.

## Startup order

`main()` in `eeye.c`. The order matters and is commented in place:

1. **Re-exec with `OMP_WAIT_POLICY=passive`** if it is not already set. libgomp's
   default makes every OpenMP worker busy-spin between parallel regions, which here run
   once per frame with long gaps. Measured at ~178% CPU idle versus ~15–19% with the
   variable set. It cannot be fixed in-process — neither `setenv` nor an early
   constructor works — so the process replaces its own image via `execve("/proc/self/exe")`.
   Guarded by `getenv` so it happens once.
2. **Signal handlers** for SIGINT/SIGTERM (flip `is_running`), SIGPIPE ignored.
3. **`config_load_once()`** — reads geometry and startup-only keys. A malformed config
   aborts here, before anything else is allocated.
4. **Resolve `capture_source`** if it is `auto` (probes for a Pi camera).
5. **`v4l2_in_negotiate_size()`** (V4L2 backend only) — asks the camera what it can
   actually do and rewrites the requested geometry. Advisory; failure is fine.
6. **Load v4l2loopback** via `virtual_cam_ensure_loaded()`, with `atexit` registered
   *first* so cleanup runs even on the failure path.
7. **Allocate the frame pool** at the now-final output geometry.
8. **Start the config watcher**, then the three pipeline threads, then stats.

Step 3 deliberately precedes step 6: a bad config should fail without having loaded a
kernel module. Step 5 must precede step 7 because the pool is sized from the negotiated
geometry and every frame in it has to match what the camera is eventually opened at.

## Shutdown

A signal handler sets `is_running = false` — nothing else, because `printf` is not
signal-safe. There is a `static_assert` that `atomic_bool` is lock-free, since a
lock-based atomic would mean the handler could touch a mutex from arbitrary interrupted
context.

Every loop polls `is_running` on a bounded interval, so all three drain and join. The
effects and consumer loops additionally keep running while their input ring is
non-empty, so in-flight frames are finished rather than dropped. Then: join, final
stats snapshot, free the pool, and `atexit` unloads v4l2loopback if this process loaded
it.

`virtual_cam_unload()` only unloads a module this process loaded — it will not yank the
device out from under another consumer that set it up independently.

## Camera reconnection

`producer_loop` has an outer loop that owns the camera for as long as it stays usable,
then lets go and retries. This covers both "no camera at startup" (USB enumeration can
lag process start) and "camera disappeared mid-session" with the same mechanism.

Retry is every 2s, logged on the first failure and then every 15th attempt (~30s) so an
unplugged camera does not flood the log for hours.

**This deliberately does not extend to the loopback device.** A missing v4l2loopback
module is a setup problem no amount of retrying `open()` fixes, unlike a USB camera
that may genuinely come back. That class of failure exits the process and relies on the
systemd unit's `Restart=` instead.

One consequence worth remembering: a reconnect calls `v4l2_in_open()` fresh, so a
replugged camera comes back at its factory defaults. Camera controls are re-applied on
every successful open for exactly this reason — see [capture.md](capture.md).

## Config snapshots

`config_current()` returns a pointer to an immutable snapshot. Both the effects and
consumer loops read it **once per frame**, not once per stage:

```c
// One snapshot per frame, not per effect: this guarantees a config push
// landing mid-processing can't split a single output frame between old
// and new parameters.
const Config *cfg = config_current(args->config);
```

The consumer takes its own independent read, because the recording decision is its own
to make. Both follow the same discipline.

Publishing writes into a slot of an 8-entry ring inside `ConfigWatcher` and then does a
release store on `current`. Readers do an acquire load. A reader holding an old pointer
stays valid until the ring wraps, which needs 8 reloads — far longer than a frame.

## Timing constants

All in `video_threads.c` unless noted:

| Constant | Value | Notes |
|---|---|---|
| `CAMERA_FRAMERATE` | 30 | Best-effort request; V4L2 does not guarantee it |
| `CAMERA_RECONNECT_DELAY_MS` | 2000 | |
| `CAMERA_RECONNECT_LOG_EVERY` | 15 | Log 1st failure, then every 15th |
| `STATS_LOG_INTERVAL_MS` | 5000 | |
| `HEALTH_PUBLISH_INTERVAL_MS` | 1000 | |
| `ring_buffer_size` | 32 | `frame_ring_buffer.h`; power of two enforced |
| `send_timeout_ms` | 200 | `stream_server.c`; viewer dropped past this |
| `virtual_cam_video_nr` | 10 | `eeye.c` → `/dev/video10` |
| `stream_server_port` | 9000 | `eeye.c` |

Framerate and both device paths are compile-time constants. Resolution is not — it
comes from config.
