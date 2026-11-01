# The frame bus

How video moves between programs on one machine: `eeye` publishes, and `hellion`,
`hellion top` and autonomy code read. One shared-memory file per stream, in
`/dev/shm` (or `$EEYE_BUS_DIR`). Source: `src/fbus.c`, `src/fbus.h`.

## Streams

| Stream | Written by | Contents | Config key |
|---|---|---|---|
| `pilot` | `eeye` | The picture after the effect chain — what the pilot sees | `bus_pilot` |
| `raw` | `eeye` | The camera frame as captured, before any effect | `bus_raw` |
| `camera` | `hellion` (topside, when `eeye` runs there) | The ROV's camera, decoded | read via `capture_stream` |

Names are configurable; `""` turns a stream off. A stream is the file
`<dir>/eeye.<name>`.

**Autonomy usually wants `raw`**: the pilot's picture has contrast, tint and edge
filters in it that are there for a human.

## What a reader can rely on

- **The writer never waits for readers.** A reader that falls behind skips to the
  newest frame and is told how many it missed. No reader, however slow or stuck, can
  delay the pilot's picture.
- **A frame is whole or not returned.** Slots are protected by a sequence counter (a
  seqlock); a copy that the writer overwrote mid-way is detected and discarded.
- **`frame_no` counts up by exactly one per captured frame**, so a gap means frames
  were dropped upstream (or skipped by you — `skipped` says which).
- **`capture_mono_ns` is the camera's time**: the V4L2 driver's own timestamp where it
  stamps `CLOCK_MONOTONIC` (usually the moment the frame started arriving), else when
  `eeye` received it. Compare it with `clock_gettime(CLOCK_MONOTONIC)` on the same
  machine. `capture_real_ns` is the same moment on the wall clock, for lining up with
  data from another machine (whose clock you then need synced — chrony).
- **Same machine only.** The bus never crosses the tether; `hellion` does that.

## Reading from C

```c
#include "fbus.h"

FbusOpenResult why;
FbusReader *r = fbus_reader_open("raw", &why);   // NULL + why if not there yet
uint32_t w, h;
fbus_reader_geometry(r, &w, &h);
// I422: Y is w*h, U and V are ((w+1)/2)*h each.
uint8_t *planes[3] = {y_buf, u_buf, v_buf};

for (;;) {
  FbusFrameInfo info;
  switch (fbus_reader_next(r, planes, 1000, &info)) {
  case FBUS_FRAME:   /* use it; info.frame_no, info.capture_mono_ns, info.skipped */ break;
  case FBUS_TIMEOUT: /* writer alive, nothing new */ break;
  case FBUS_GONE:    /* writer stopped or restarted: close and reopen */ break;
  }
}
```

`fbus_status()` reads a stream's header without attaching (for dashboards), and
`fbus_list()` lists the streams in the directory.

## Reading from anything else

The file is plain shared memory with a fixed layout, native-endian (it never leaves
the machine). `mmap` it read-only.

```
[ header: 4096 bytes ][ slot 0 ][ slot 1 ] ... [ slot N-1 ]
slot = [ slot header: 64 bytes ][ Y ][ U ][ V ], padded to a multiple of 4096
```

### Header

| Offset | Type | Field | |
|---|---|---|---|
| 0 | char[8] | `magic` | `"EEYEBUS1"` |
| 8 | u32 | `version` | 1 |
| 12 | u32 | `header_bytes` | 4096 |
| 16 | u64 | `generation` | differs for every writer run |
| 24 | u32 | `width` | |
| 28 | u32 | `height` | |
| 32 | u32 | `fourcc` | `'422P'` (0x50323234): I422 planar |
| 36 | u32 | `full_range` | 1: Y and UV span 0–255 |
| 40 | u32[3] | `plane_offset` | Y, U, V, from the start of a slot's image data |
| 52 | u32[3] | `plane_stride` | `width`, `(width+1)/2`, `(width+1)/2` |
| 64 | u32[3] | `plane_bytes` | |
| 76 | u32 | `slot_count` | 4 |
| 80 | u32 | `slot_header_bytes` | 64 |
| 88 | u64 | `slot_stride` | bytes from one slot to the next |
| 96 | u64 | `slots_offset` | file offset of slot 0 |
| 104 | i32 | `writer_pid` | as the writer saw it (meaningless across pid namespaces) |
| 108 | u32 | `flags` | bit 0: closed by its writer |
| 112 | u64 | `created_real_ns` | |
| 128 | u32 | `futex_word` | changes on every publish and on close; wait on it |
| 136 | u64 | `write_seq` | frames published; frame *n* is in slot *(n−1) mod slot_count* |
| 144 | u64 | `last_publish_mono_ns` | |
| 152 | u64 | `heartbeat_mono_ns` | refreshed every 500ms while the writer runs |

### Slot header

| Offset | Type | Field | |
|---|---|---|---|
| 0 | u64 | `seq` | `2n` once frame *n* is complete in this slot; odd while being written |
| 8 | i64 | `frame_no` | |
| 16 | u64 | `capture_mono_ns` | 0 if unknown |
| 24 | u64 | `capture_real_ns` | 0 if unknown |
| 32 | u64 | `publish_mono_ns` | |

### Reading the newest frame

```
n  = header.write_seq                (acquire)
if n == the last n you read: nothing new — wait
s  = slot (n-1) % slot_count
s1 = s.seq                           (acquire)
if s1 != 2n: the slot was already reused for a newer frame — start over
copy the planes and the slot header fields
s2 = s.seq                           (after an acquire fence)
if s2 != s1: the writer overwrote it while you copied — start over
```

In Python, with `mmap` and `struct` (no numpy needed to read; `numpy.frombuffer` on
the Y bytes gives you an image):

```python
import mmap, struct
m = mmap.mmap(open("/dev/shm/eeye.raw", "rb").fileno(), 0, access=mmap.ACCESS_READ)
w, h = struct.unpack_from("<II", m, 24)
y_off = struct.unpack_from("<I", m, 40)[0]
y_bytes = struct.unpack_from("<I", m, 64)[0]
slots, slot_hdr = struct.unpack_from("<II", m, 76)
stride, first = struct.unpack_from("<QQ", m, 88)

def newest():
    while True:
        n = struct.unpack_from("<Q", m, 136)[0]
        base = first + ((n - 1) % slots) * stride
        s1 = struct.unpack_from("<Q", m, base)[0]
        if s1 != 2 * n:
            continue
        frame_no, capture_ns = struct.unpack_from("<qQ", m, base + 8)
        y = m[base + slot_hdr + y_off : base + slot_hdr + y_off + y_bytes]
        if struct.unpack_from("<Q", m, base)[0] == s1:
            return frame_no, capture_ns, y      # y is w*h bytes of luma
```

Python cannot express the acquire ordering, but on x86 and ARM a `struct.unpack_from`
of an aligned 8-byte field is a single load, and the before/after check still catches
a torn copy. Poll `write_seq` (every couple of milliseconds is plenty at 30fps), or
`FUTEX_WAIT` on `futex_word` from code that can.

### Knowing the stream is still alive

A stream is **gone** — close it and reopen by name — when any of these hold:

- `flags` bit 0 is set (its writer closed it);
- `heartbeat_mono_ns` is more than 3s old (its writer died without closing);
- the file at `<dir>/eeye.<name>` is no longer the one you mapped (compare inode):
  a restarted writer replaces the file atomically, possibly at a new size.

Do not use `writer_pid` for this: a reader in a container (a BlueOS extension, say)
cannot see the writer's process.

## Sizes and permissions

Each stream holds 4 frames: about **2.4MB at 640×480 and 7.4MB at 720p** of RAM in
`/dev/shm`. The writer reserves it up front, so a full `/dev/shm` fails at startup with
a clear message instead of crashing mid-run.

Files are mode `0660`, owned by the writer's user and group. Run the services with
`Group=video` and put readers in the `video` group.

## Not built yet

An **`overlay` input**: autonomy publishing boxes and labels for `eeye` to draw on the
pilot's picture only. The design leaves room for it; nothing reads it yet.
