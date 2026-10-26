# Architecture

Threads, frames, and how a frame gets from the sensor to the loopback device.

## Threads

Seven. Four are created in `main()`; the config watcher, recorder and stream threads
are started by the modules that own them.

| Thread | Entry point | Job |
|---|---|---|
| producer | `producer_loop` | Owns the camera. Captures into a frame, pushes it downstream. |
| effects | `effects_loop` | Runs the effect chain on each frame. |
| consumer | `consumer_loop` | Writes to the virtual camera, then hands the frame to the recording and preview taps. |
| recorder | `writer_loop` in `recorder.c` | Owns the recording file. Writes frames the consumer hands it, returns them to the pool. |
| stream | `stream_loop` in `stream_server.c` | JPEG-encodes and sends the preview to the connected viewer. |
| stats | `stats_loop` | Logs ring occupancy and effects time every 5s. Purely diagnostic. |
| config watcher | inside `config.c` | Watches the config file with inotify, republishes on change. |

The stats, recorder and stream threads are optional: if one fails to start, that
feature is disabled with a log line and the pipeline carries on. The producer, effects
and consumer threads are fatal to start.

**Nothing slow runs on the consumer thread.** It feeds the virtual camera, which is
what the pilot is looking at, so the two taps only take a copy (preview) or ownership
(recording) of the frame and return. A full disk or a bad tether can cost recorded or
previewed frames, never the live feed. See [output.md](output.md).

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
4. Consumer pops from **out**, writes to the loopback device, offers the preview a
   copy, then either hands the frame to the recorder (which pushes it back to **free**
   once written) or pushes it back to **free** itself.

The pool is allocated once, up front, and never grows: `frame_pool_size` frames, 8.
That is deliberately far below the rings' 31-slot capacity. Every pooled frame can end
up queued in front of the virtual camera, so the pool size is also the worst-case lag
between camera and output: 8 frames is ~270ms at 30fps, where the old 31 was a full
second. When the pipeline falls behind, the producer blocks on an empty pool and the
camera driver drops its oldest frames, which is the right trade for a live view. Each
frame is three full I422 buffers, ~5.5MB at 720p, so it is also 44MB instead of 171MB.

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

**The roles can move between allocations.** `owned[3]` holds the three blocks; the
role pointers above say which block plays which part right now. A chain that finishes
in spare swaps the work and spare roles instead of copying, and an empty chain points
work at raw itself. `vf_restore_roles()` at the start of the next chain separates them
again, and `vf_free()` frees `owned[]`, never the roles. Raw always stays on its own
block.

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
or spins. They are `MonoSem`, a mutex+condvar counting semaphore, not `sem_t`: their
timeouts have to run on `CLOCK_MONOTONIC` (a Pi has no RTC, and timesyncd can step the
wall clock), and `sem_t` can only do that through `sem_clockwait`, which needs glibc
2.30.

Variables shared between threads are plain types accessed only through the GCC
`__atomic` builtins rather than C11 `<stdatomic.h>`, which old compilers lack; see
`src/compat.h`.

`ring_push_wait` / `ring_pop_wait` take a `bool *is_running` (atomic). They wake up on a
bounded timeout (~200ms) to re-check it, so shutdown is noticed promptly rather than
deadlocking on an empty ring. Passing `NULL` means "this must complete regardless",
which is the frame-leak guard above.

The diagnostic counters (`high_water`, `full_stalls`, `empty_stalls`) are relaxed
atomics and never influence behavior. Note the stall counters count ~200ms *wait
windows*, not events — a stall count of 5 means roughly a second spent blocked, not
five blocked pushes.

## Startup order

`main()` in `eeye.c`. The order matters and is commented in place:

1. **Re-exec with `OMP_WAIT_POLICY=passive`** if neither it nor `GOMP_SPINCOUNT` is
   set. libgomp's default makes every OpenMP worker busy-spin between parallel
   regions, which here run once per frame with long gaps. Measured at ~178% CPU idle
   versus ~15–19% with the variable set. A bounded spin (`GOMP_SPINCOUNT=10000`) was
   also tried, to avoid waking the team at each region within a frame, and measured
   worse: 55% CPU and 2.0–2.3ms of effects time per frame against 27% and 1.3ms
   for passive, since spinning workers compete for cores with the other pipeline
   threads. It cannot be fixed in-process — neither `setenv` nor an early
   constructor works — so the process replaces its own image, exec'ing the path
   `/proc/self/exe` resolves to. Not `/proc/self/exe` itself: the kernel names a
   process after the path it was exec'd from, so it would run as `exe` and
   `pgrep eeye` / `pkill eeye` would not find it. Guarded by `getenv` so it happens
   once.
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
signal-safe. There is a compile-time check (`__GCC_ATOMIC_BOOL_LOCK_FREE`) that `bool` atomics are
lock-free, since a
lock-based atomic would mean the handler could touch a mutex from arbitrary interrupted
context.

Every loop polls `is_running` on a bounded interval, so all three drain and join. The
effects and consumer loops additionally keep running while their input ring is
non-empty, so in-flight frames are finished rather than dropped. On its way out the
consumer stops the recorder, which writes whatever it still has queued, closes the file
and returns its frames to the pool, then stops the stream thread. Then: join, final
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
| `send_timeout_ms` | 200 | `stream_server.c`; a viewer that can't take a whole frame in this long is dropped |
| `frame_pool_size` | 8 | `eeye.c`; frames in circulation, and the worst-case output lag |
| `recorder_max_queued` | 3 | `recorder.h`; frames the recording thread may hold before it drops |
| `virtual_cam_video_nr` | 10 | `eeye.c` → `/dev/video10` |
| `stream_server_port` | 9000 | `eeye.c` |

Framerate and both device paths are compile-time constants. Resolution is not — it
comes from config.
