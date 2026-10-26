# Topside

Everything network-facing. All Python, standard library only, outside the C build.

| File | Runs on | Role |
|---|---|---|
| `pi/config_agent.py` | drone | HTTP: serves and writes the config, relays health |
| `topside/web_ui.py` | topside | The control page; proxies config; fans out video |
| `tools/eeye-net` | topside | Discovery and link diagnostics (CLI) |
| `tools/eeye_net.py` | — | Shared library for the above and for `--drone-host auto` |
| `tools/eeye-record` | topside | H.264 recording, wraps ffmpeg |

## The split, and why `eeye` has no networking

`eeye` never talks to the network. It reads a local JSON file and writes local status
files; `config_agent.py` accepts a config over HTTP and writes it to disk, where
`eeye`'s inotify watcher picks it up.

That keeps `eeye` exactly what it has always been — a local pipeline reacting to a local
file — and means a crash in the network layer cannot take the pipeline down. The two are
independent processes.

`config_agent.py` deliberately **does not re-validate the config's schema** beyond "is
this well-formed JSON". `config.c` already does thorough validation and logs exactly why
something was rejected; duplicating that would create a second place that can disagree
with the first. Instead a POST waits briefly for `eeye`'s own `.status` verdict and
relays *that* — true confirmation that the config took effect, not just that a file got
written.

## `config_agent.py`

Serves on all interfaces, IPv6 and IPv4 (`DualStackHTTPServer`), port 9001.

| Endpoint | Behavior |
|---|---|
| `GET /config` | The config file's raw contents (`{}` if absent) |
| `POST /config` | Validates JSON, writes atomically, waits for `eeye`'s verdict |
| `GET /health` | Relays `<config>.health`, **with the file's age attached** |

POST returns `{"written": true, "eeye_accepted": true|false|null}`. `null` means `eeye`
never weighed in within the timeout.

**`null` is ambiguous, and that ambiguity is a real trap.** An agent pointed at a path
`eeye` is not watching returns *byte-identically* to one with no `eeye` running at all.
Both processes look healthy, the UI reports success, and your edits land in a file
nothing reads. `tools/eeye-net check` detects it by cross-checking geometry — see below.

`wait_for_reload_status()` compares the status file's **mtime** against what it was
before the write. Without that, a stale status file left over from a previous run reads
identically to a fresh verdict.

## `web_ui.py`

Serves the control page on port 8080.

| Endpoint | Behavior |
|---|---|
| `GET /` | The page (single self-contained HTML template) |
| `GET /stream` | `multipart/x-mixed-replace` MJPEG for the browser |
| `GET /config` | Proxied to the agent |
| `POST /config` | Proxied to the agent |
| `GET /health` | Link state (local) **+** drone health (proxied) |
| `POST /snapshot` | Saves the current frame as a `.jpg` |
| `POST /record` | Starts/stops `tools/eeye-record` |

### Fan-out

`FrameSource` holds **one** connection to the drone regardless of how many browsers are
watching. The drone's `stream_server` is single-viewer, so this is that one viewer.

This is why a diagnostic that connects directly to port 9000 will kick the UI off, and
the UI's reconnect will kick the diagnostic off — a loop that looks exactly like a flaky
link. Use `/stream` from `web_ui.py` instead, or accept that you are displacing someone.

### Staleness detection

The original bug: `wait_for_frame` blocked forever, so when the tether dropped the
browser kept showing the last frame received, indefinitely, with nothing to say so. A
frozen picture and a live one looked identical.

Now `FrameSource` timestamps every frame and tracks socket state, `wait_for_frame` takes
a timeout, and `health()` reports `connected`, `age_s`, `frames`, `have_frame`. The page
polls `/health` every second and, past 2.5s of no frames, desaturates the image and
shows a pulsing banner naming the age.

The pulse matters: a still scene and a frozen one look the same, so motion is the
signal. Age is reported as a number rather than a yes/no so the page can decide what
counts as stale — that depends on the configured frame interval.

### Recording control

`Recorder` spawns `tools/eeye-record` as a subprocess, because a browser cannot.

**One recording at a time**, deliberately: two encoders reading the same stream would
double the CPU for two files of identical content, and "am I recording?" should have one
answer.

Two process-management details that were bugs first, both producing unplayable files:

- **`start_new_session=True`.** The recorder re-execs ffmpeg as a *grandchild*.
  Signalling only the direct child leaves ffmpeg running — still writing to a file the
  UI has already reported as stopped, and never finalized, so the mp4 has no `moov` atom
  and will not open at all.
- **`os.killpg(..., SIGINT)`**, then wait. SIGINT so ffmpeg finalizes the container;
  the whole group so it actually arrives. `SIGKILL` only after a 25s timeout.

### Config carry-through

`buildConfig()` in the page's JS rewrites the **whole** config file, so anything not
echoed back is effectively deleted.

`carriedGeometry` preserves `capture_width`, `capture_height`, `downscale`, and
`capture_source` untouched. Without it, the first slider tweak would silently drop the
operator's downscale and the drone would come back at full resolution on its next
restart, saturating the tether.

It only carries keys the drone's config *actually had* — writing back defaults it never
set would itself register as a change and trigger a spurious "restart to apply"
warning.

**If you add a startup-only config key, add it to that list.**

### Page encoding

The page is served as `text/html; charset=utf-8` and carries `<meta charset="utf-8">`.
Both are needed: the header is what browsers obey, the meta tag covers a saved page.

Without them, browsers fall back to a single-byte encoding and decode correct UTF-8 as
Latin-1 — every em dash and the micro sign in the exposure label render as mojibake.
`_respond()` appends the charset centrally for any `text/*` response, so error bodies
get it too.

The capture button icons are **drawn in CSS**, not typed as glyphs. A character like
U+25CF renders as a tofu box wherever no installed font covers it, and it lets the
record dot actually be red.

## Tether networking

Field deployments have **no topside network**: the drone's Pi and the topside machine
are joined by one Ethernet cable, no router, no DHCP, no DNS.

**The zero-config primitive is IPv6 link-local.** Every host self-assigns an `fe80::/64`
address on every live link with no server involved (RFC 4862), so a bare cable is a
working network the moment both ends are powered. Reaching such an address needs the
interface too — every link has its own `fe80::/64` — which is the scope ID in a 4-tuple
`sockaddr_in6`:

```python
sock.connect((addr, port, 0, socket.if_nametoindex(iface)))
```

Both drone-side ports listen dual-stack specifically so this works.

### The trap that will bite you

**NetworkManager needs at least one address family to succeed**, or it tears the
connection down — *removing the address* — and retries. On a direct cable both of its
defaults fail: `ipv4.method auto` has no DHCP server to answer it, and `ipv6.method
auto` waits for a router advertisement that never arrives. So it sits in "getting IP
configuration", times out, drops the connection, and starts over. That produces outages
of 75+ seconds while the physical link never drops.

Diagnosing it, the giveaway is that `carrier_changes` in
`/sys/class/net/<iface>/carrier_changes` **does not increment**. The cable is fine; the
interface simply has no address.

`ip link` says `UP` the whole time, because that is layer 2. **Check for an address, not
for "up".**

**One setting fixes it**, on both ends. A link-local address needs no server, so that
family always succeeds and the connection stays activated:

```sh
sudo nmcli connection modify "Wired connection 1" ipv6.method link-local
```

IPv4 is deliberately left on DHCP, so the same profile still works normally on a bench
LAN — it is simply allowed to fail on a bare cable (`ipv4.may-fail` defaults to yes).

If there is no wired profile yet:

```sh
sudo nmcli connection add type ethernet con-name eeye-tether ifname '*' \
    ipv4.method auto ipv6.method link-local \
    connection.autoconnect yes connection.autoconnect-priority 50
```

`ifname '*'` matters — a profile pinned to one interface name silently stops applying
the day you use a USB adapter instead.

**On the drone, `sudo ./install.sh` does this automatically.** It fixes every wired
profile, creates one if none exists, and deliberately does *not* reactivate an
already-connected interface — the installer is often run over SSH, quite possibly over
the very interface being changed. `--no-network` skips the step.

A VPN killswitch is worth ruling out early too: if it drops ICMP, discovery finds
nothing. The tell is that pinging *your own* link-local address fails, which can only be
a local filter.

## `tools/eeye-net`

Runs topside, probes the drone. **Only ever reads** — it changes nothing on either
machine, so it is safe mid-dive — and prints the exact command to fix what it finds.

```sh
tools/eeye-net discover          # find drones on any live link
tools/eeye-net check [host]      # full health ladder
tools/eeye-net fix               # print (never run) setup commands
```

Exit codes suit a pre-dive checklist: `0` healthy, `1` degraded, `2` no drone found.

Discovery enumerates candidates from the kernel's neighbour cache **and** an ICMPv6
all-nodes multicast sweep (`ff02::1%iface`), then probes each for a real
`config_agent`. It only searches links with a carrier, and sorts wired before wireless.

A candidate qualifies on **port 9001 returning valid JSON**, not on the video stream:
the stream tap is off by default, so requiring video would hide exactly the drones most
in need of being found.

Two failures it exists to catch, because neither is visible otherwise:

- **The video port accepts your connection and then sends nothing.** That is what
  `stream_frame_interval: 0` looks like from outside. A port check calls it healthy
  while you stare at a black screen, so `check` reads an actual frame and distinguishes
  "connected but silent" from "nothing listening".
- **Mismatched config paths.** It cross-checks the config's declared geometry against
  the live stream's real dimensions, which disagree exactly when the paths do.

`tools/eeye_net.py` is the shared library — **it has no entry point**, so running it
directly prints nothing and exits 0. The CLI is the extensionless executable.

## `tools/eeye-record`

Records the preview topside as H.264: **~1.2 GB/hour against ~199 GB/hour** for the
drone's raw tap, and it costs the drone nothing, because those frames are already being
sent.

```sh
tools/eeye-record --split 10m --quality high -o dive.mp4
tools/eeye-record --raw          # flip stream_raw first, so no effects are burned in
```

**It records the preview, which is not what the drone sees.** Framerate is the capture
rate divided by `stream_frame_interval`; quality is `stream_quality`, then re-encoded.
Both facts are invisible in the finished file, so the tool prints what it is about to
capture, derived from the drone's live config:

```
Source : processed image (effects burned in), ~15 fps, JPEG quality 75
```

It connects through `web_ui.py`, **not** to the drone directly — the drone serves one
viewer at a time, so a recorder talking to it would kick the pilot off their own video.

Two ffmpeg flags worth keeping:

- `-use_wallclock_as_timestamps` — the multipart stream carries no timestamps, so
  without this any hitch in the tether silently shifts the recording out of sync with
  when things actually happened.
- `+faststart` — a dive recording can end by the boat losing power rather than by
  someone pressing stop, and the file should still play.

It handles SIGINT/SIGTERM explicitly rather than relying on `KeyboardInterrupt`: ffmpeg
shares the process group and already receives the signal, so the parent was racing its
own child's shutdown.
