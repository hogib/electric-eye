#!/usr/bin/env python3
"""
Electric Eye topside web UI: the surface-side control station for a
submersible drone tethered over Ethernet/fiber.

Serves one page with the live video feed and a form for the effect chain,
recording, and stream-quality settings. Two things happen behind the
scenes when you hit "Apply":

  1. This script is the only thing browsers talk to (same-origin, no CORS
     to worry about). It forwards whatever config the form built straight
     to the drone's config_agent (see pi/config_agent.py) over a plain
     server-to-server HTTP request.
  2. config_agent writes it to eeye_config.json on the drone; eeye's
     existing inotify watcher (src/config.c) picks it up and hot-reloads
     within ~200ms. Neither of those two scripts talk to each other any
     other way.

The video feed (see FrameSource, carried over from milestone 1) is a
completely separate connection straight to the drone's stream_server (see
src/stream_server.c) -- video and control are two independent sockets to
the drone, so a control-panel hiccup can't stall the video and vice versa.

Only needs the Python standard library -- nothing to install.

Usage:
    python3 web_ui.py --drone-host 192.168.1.50

Then point a browser at http://<this machine's own IP>:8080/
"""
import argparse
import http.server
import json
import os
import socket
import sys
import struct
import threading
import time
import urllib.error
import urllib.request


class FrameSource:
    """Reads length-prefixed JPEG frames from the drone and keeps the
    latest one available to any number of HTTP viewers. One connection to
    the drone regardless of how many browsers are watching here -- the
    drone's stream_server is itself a single-viewer server, so this is
    that one viewer, and everyone hitting this script's own HTTP server
    shares the same frames."""

    def __init__(self, host, port):
        self.host = host
        self.port = port
        self._frame = None
        self._frame_id = 0
        self._cond = threading.Condition()
        threading.Thread(target=self._run, daemon=True).start()

    def _run(self):
        while True:
            try:
                self._read_forever()
            except (OSError, ConnectionError) as e:
                print(f"[frame source] connection to {self.host}:{self.port} "
                      f"lost/failed ({e}); retrying in 2s")
                time.sleep(2)

    def _read_forever(self):
        print(f"[frame source] connecting to {self.host}:{self.port}...")
        with socket.create_connection((self.host, self.port), timeout=5) as sock:
            sock.settimeout(10)  # a stalled drone side shouldn't hang forever
            print("[frame source] connected")
            while True:
                (length,) = struct.unpack("!I", self._recv_exact(sock, 4))
                jpeg = self._recv_exact(sock, length)
                with self._cond:
                    self._frame = jpeg
                    self._frame_id += 1
                    self._cond.notify_all()

    @staticmethod
    def _recv_exact(sock, n):
        buf = bytearray()
        while len(buf) < n:
            chunk = sock.recv(n - len(buf))
            if not chunk:
                raise ConnectionError("drone closed the connection")
            buf.extend(chunk)
        return bytes(buf)

    def wait_for_frame(self, last_seen_id):
        """Blocks until a frame newer than last_seen_id is available, then
        returns (frame_bytes, frame_id)."""
        with self._cond:
            while self._frame_id == last_seen_id or self._frame is None:
                self._cond.wait()
            return self._frame, self._frame_id


# Mirrors config.h's JSON schema exactly: for each effect, the list of
# [key, min, max, default] fields it takes. The "unknown key is a hard
# error" policy in config.c's parse_effect_stage() means the browser-side
# JS below must send *only* these keys for a given stage's effect -- never
# every possible key with the unused ones just left at 0 -- or eeye will
# reject the whole config on reload.
EFFECT_FIELDS = {
    "none": [],
    "grayscale": [],
    "invert": [],
    "threshold": [["threshold_value", 0, 255, 128]],
    "tint": [
        ["tint_u", 0, 255, 90],
        ["tint_v", 0, 255, 150],
        ["tint_strength", 0, 255, 180],
    ],
    "sobel": [["sobel_threshold", 0, 255, 0]],
    # Range capped well below the field's real 0-255 (repeat-count) domain:
    # each unit is a full extra 5-tap pass over the whole frame, and past
    # ~20 the added blur is visually indistinguishable (effective radius
    # grows with sqrt(passes)) while cost keeps climbing linearly -- at 255
    # it's ~5x slower per frame than at 20 for no visible gain. Edit
    # eeye_config.json directly if you ever want higher than the slider goes.
    "blur": [["blur_strength", 0, 20, 0]],
    "contrast": [],
    "light": [["light_level", 0, 255, 128]],
}

INDEX_HTML_TEMPLATE = """<!doctype html>
<html>
<head>
<title>Electric Eye - Control</title>
<meta name="viewport" content="width=device-width, initial-scale=1">
<style>
  body { margin: 0; font: 14px/1.4 system-ui, sans-serif; background: #111; color: #eee; }
  #video { width: 100%; max-height: 60vh; object-fit: contain; background: #000; display: block; }
  #panel { padding: 12px 16px 32px; max-width: 640px; margin: 0 auto; }
  h2 { font-size: 15px; margin: 20px 0 8px; color: #9cf; }
  .stage { display: flex; flex-wrap: wrap; align-items: center; gap: 6px;
           background: #1c1c1c; border-radius: 6px; padding: 8px; margin-bottom: 6px; }
  .stage select { font-size: 14px; }
  .stage label { font-size: 12px; color: #aaa; margin-left: 6px; }
  .stage input[type=range] { width: 90px; vertical-align: middle; margin: 0 4px; }
  .stage .val { display: inline-block; min-width: 26px; text-align: right;
                font-variant-numeric: tabular-nums; color: #ddd; }
  .stage .spacer { flex: 1; }
  .rgb-row { display: inline-flex; align-items: center; gap: 2px; }
  .rgb-row span.chan { color: #aaa; font-size: 12px; }
  .rgb-row input[type=range] { width: 70px; }
  .swatch { display: inline-block; width: 22px; height: 22px; border-radius: 4px;
            border: 1px solid #555; margin-left: 6px; vertical-align: middle; }
  button { font-size: 13px; padding: 4px 10px; cursor: pointer; }
  #apply { font-size: 15px; padding: 8px 20px; margin-top: 16px; }
  #status { margin-top: 10px; font-size: 13px; white-space: pre-wrap; }
  .readonly { font-size: 13px; opacity: 0.75; font-family: monospace; }
  #status.ok { color: #8f8; }
  #status.err { color: #f88; }
  label.field { display: block; margin: 6px 0; }
  input[type=text] { width: 100%; box-sizing: border-box; }
</style>
</head>
<body>
  <img id="video" src="/stream">
  <div id="panel">
    <h2>Effect chain</h2>
    <div id="stages"></div>
    <button id="add-stage" type="button">+ Add stage</button>

    <h2>Recording (onboard, full quality)</h2>
    <label class="field">Path (blank = off)
      <input type="text" id="record_path" placeholder="/opt/electric-eye/recordings/session.raw">
    </label>

    <h2>Live preview stream</h2>
    <label class="field">Send every Nth frame (0 = off)
      <input type="number" id="stream_frame_interval" min="0" max="255" value="0">
    </label>
    <label class="field">JPEG quality (1-100)
      <input type="number" id="stream_quality" min="1" max="100" value="60">
    </label>

    <h2>Geometry</h2>
    <!-- Read-only on purpose: eeye only reads these at startup, so a
         control here would look live and quietly not be. Edit the drone's
         eeye_config.json and restart eeye to change it. -->
    <div id="geometry" class="readonly">-</div>

    <button id="apply" type="button">Apply</button>
    <div id="status"></div>
  </div>

<script>
const EFFECT_FIELDS = __EFFECT_FIELDS_JSON__;
const stagesEl = document.getElementById("stages");

function fieldRowHtml(stage, key, min, max, def) {
  const val = (stage && stage[key] !== undefined) ? stage[key] : def;
  return `<label>${key}`
       + `<input type="range" min="${min}" max="${max}" value="${val}" `
       + `data-field="${key}" oninput="this.nextElementSibling.textContent=this.value">`
       + `<span class="val">${val}</span></label>`;
}

// tint's config fields are chroma (tint_u/tint_v, 128 = neutral, see
// src/point_opps.c's doc comment) -- not something anyone should have to
// pick by typing raw YCbCr numbers. These convert an RGB color picked via
// sliders into that chroma pair (BT.601, Y dropped since the tint effect
// never touches luma) and back (assuming Y=128, since the real per-pixel Y
// isn't known client-side -- only used to seed the sliders' starting
// position from an existing config, never sent anywhere).
function clampByte(n) { return Math.max(0, Math.min(255, Math.round(n))); }
function rgbToUV(r, g, b) {
  return [
    clampByte(-0.169 * r - 0.331 * g + 0.5 * b + 128),
    clampByte(0.5 * r - 0.419 * g - 0.081 * b + 128),
  ];
}
function uvToRgb(u, v) {
  const cb = u - 128, cr = v - 128;
  return [
    clampByte(128 + 1.402 * cr),
    clampByte(128 - 0.344136 * cb - 0.714136 * cr),
    clampByte(128 + 1.772 * cb),
  ];
}

function renderTintFields(fields, stage) {
  const u0 = (stage && stage.tint_u !== undefined) ? stage.tint_u : 90;
  const v0 = (stage && stage.tint_v !== undefined) ? stage.tint_v : 150;
  const strength0 = (stage && stage.tint_strength !== undefined) ? stage.tint_strength : 180;
  const [r0, g0, b0] = uvToRgb(u0, v0);

  fields.innerHTML =
    `<span class="rgb-row">`
    + `<span class="chan">R</span><input type="range" min="0" max="255" value="${r0}" class="rgb-r" `
    + `oninput="this.nextElementSibling.textContent=this.value"><span class="val">${r0}</span>`
    + `<span class="chan">G</span><input type="range" min="0" max="255" value="${g0}" class="rgb-g" `
    + `oninput="this.nextElementSibling.textContent=this.value"><span class="val">${g0}</span>`
    + `<span class="chan">B</span><input type="range" min="0" max="255" value="${b0}" class="rgb-b" `
    + `oninput="this.nextElementSibling.textContent=this.value"><span class="val">${b0}</span>`
    + `<span class="swatch"></span>`
    + `</span>`
    + `<label>strength`
    + `<input type="range" min="0" max="255" value="${strength0}" data-field="tint_strength" `
    + `oninput="this.nextElementSibling.textContent=this.value"><span class="val">${strength0}</span></label>`
    + `<input type="hidden" data-field="tint_u" value="${u0}">`
    + `<input type="hidden" data-field="tint_v" value="${v0}">`;

  const rIn = fields.querySelector(".rgb-r");
  const gIn = fields.querySelector(".rgb-g");
  const bIn = fields.querySelector(".rgb-b");
  const swatch = fields.querySelector(".swatch");
  const uHidden = fields.querySelector('[data-field="tint_u"]');
  const vHidden = fields.querySelector('[data-field="tint_v"]');

  function syncFromRgb() {
    const r = Number(rIn.value), g = Number(gIn.value), b = Number(bIn.value);
    swatch.style.background = `rgb(${r}, ${g}, ${b})`;
    const [u, v] = rgbToUV(r, g, b);
    uHidden.value = u;
    vHidden.value = v;
  }
  for (const el of [rIn, gIn, bIn]) el.addEventListener("input", syncFromRgb);
  syncFromRgb();
}

function addStage(stage) {
  stage = stage || { effect: "none" };
  const row = document.createElement("div");
  row.className = "stage";

  const select = document.createElement("select");
  for (const effect of Object.keys(EFFECT_FIELDS)) {
    const opt = document.createElement("option");
    opt.value = effect;
    opt.textContent = effect;
    if (effect === stage.effect) opt.selected = true;
    select.appendChild(opt);
  }

  const fields = document.createElement("span");
  fields.className = "fields";

  function renderFields() {
    fields.innerHTML = "";
    if (select.value === "tint") {
      renderTintFields(fields, stage);
      return;
    }
    for (const [key, min, max, def] of EFFECT_FIELDS[select.value]) {
      fields.insertAdjacentHTML("beforeend", fieldRowHtml(stage, key, min, max, def));
    }
  }
  select.addEventListener("change", renderFields);
  renderFields();

  const up = document.createElement("button");
  up.type = "button"; up.textContent = "\\u2191";
  up.onclick = () => { const prev = row.previousElementSibling;
                       if (prev) stagesEl.insertBefore(row, prev); };

  const down = document.createElement("button");
  down.type = "button"; down.textContent = "\\u2193";
  down.onclick = () => { const next = row.nextElementSibling;
                         if (next) stagesEl.insertBefore(next, row); };

  const remove = document.createElement("button");
  remove.type = "button"; remove.textContent = "\\u2715";
  remove.onclick = () => row.remove();

  const spacer = document.createElement("span");
  spacer.className = "spacer";

  row.append(select, fields, spacer, up, down, remove);
  stagesEl.appendChild(row);
}

document.getElementById("add-stage").addEventListener("click", () => addStage());

// Geometry (capture_width/capture_height/downscale) and capture_source are
// startup-only on the drone, so this UI deliberately doesn't offer them as
// controls -- but they must still be carried through untouched.
// buildConfig() writes the whole config file, so anything not echoed back
// here is effectively deleted: without this, the first slider tweak would
// silently drop the operator's downscale (the drone would come back up at
// full resolution on its next restart, saturating the tether) or their
// capture_source (a drone pinned to one backend would revert to
// auto-detect, and pick the wrong camera on a vehicle carrying both).
let carriedGeometry = {};

function buildConfig() {
  const chain = [];
  for (const row of stagesEl.children) {
    const effect = row.querySelector("select").value;
    const stage = { effect };
    for (const input of row.querySelectorAll("input[data-field]")) {
      stage[input.dataset.field] = Number(input.value);
    }
    chain.push(stage);
  }
  return {
    chain,
    record_path: document.getElementById("record_path").value,
    stream_frame_interval: Number(document.getElementById("stream_frame_interval").value),
    stream_quality: Number(document.getElementById("stream_quality").value),
    ...carriedGeometry,
  };
}

function loadConfig(cfg) {
  stagesEl.innerHTML = "";
  for (const stage of (cfg.chain || [])) addStage(stage);
  if (stagesEl.children.length === 0) addStage();
  document.getElementById("record_path").value = cfg.record_path || "";
  document.getElementById("stream_frame_interval").value = cfg.stream_frame_interval || 0;
  document.getElementById("stream_quality").value = cfg.stream_quality || 60;

  // Only carry keys the drone's config actually had. Writing back defaults
  // it never set would itself be a geometry change from eeye's point of
  // view, which is exactly the spurious "restart to apply" warning this is
  // meant to avoid.
  carriedGeometry = {};
  for (const k of ["capture_width", "capture_height", "downscale",
                   "capture_source"]) {
    if (cfg[k] !== undefined) carriedGeometry[k] = cfg[k];
  }

  const cw = cfg.capture_width || 1280, chh = cfg.capture_height || 720;
  const ds = cfg.downscale || 1;
  const src = cfg.capture_source || "auto";
  const geom = ds > 1
    ? `${cw}x${chh} capture, ${ds}x downscale -> ${cw / ds}x${chh / ds} pipeline`
    : `${cw}x${chh}, no downscaling`;
  document.getElementById("geometry").textContent = `${geom} (source: ${src})`;
}

async function refreshFromDrone() {
  const status = document.getElementById("status");
  try {
    const res = await fetch("/config");
    loadConfig(await res.json());
  } catch (e) {
    status.textContent = "Could not load current config from drone: " + e;
    status.className = "err";
    addStage();
  }
}

// web_ui.py's own proxy already gives up on the drone's config_agent after
// 5s, and config_agent gives up on eeye's own confirmation after 2s -- so
// a healthy round trip never takes long. But this request crosses a real
// network hop over the tether, which can just drop packets with no RST/FIN
// to ever wake up fetch()'s promise, especially before this ever runs over
// the actual fiber run rather than localhost. Without a hard client-side
// deadline, that kind of stall leaves the button stuck on "Applying..."
// forever with no way out except reloading the page -- so this aborts and
// reports it instead of waiting indefinitely.
const APPLY_TIMEOUT_MS = 8000;

document.getElementById("apply").addEventListener("click", async () => {
  const status = document.getElementById("status");
  status.textContent = "Applying...";
  status.className = "";
  const controller = new AbortController();
  const timer = setTimeout(() => controller.abort(), APPLY_TIMEOUT_MS);
  try {
    const res = await fetch("/config", {
      method: "POST",
      headers: { "Content-Type": "application/json" },
      body: JSON.stringify(buildConfig()),
      signal: controller.signal,
    });
    const text = await res.text();
    if (!res.ok) {
      status.textContent = "Rejected: " + text;
      status.className = "err";
    } else {
      let data;
      try { data = JSON.parse(text); } catch { data = {}; }
      if (data.eeye_accepted === true) {
        status.textContent = "Applied -- eeye confirmed it took effect.";
        status.className = "ok";
      } else if (data.eeye_accepted === false) {
        status.textContent = "Written, but eeye REJECTED it and kept the "
          + "previous config. Check eeye's own log/journalctl on the drone "
          + "for why.";
        status.className = "err";
      } else {
        status.textContent = "Written, but got no confirmation from eeye "
          + "within the timeout -- is it running and watching this exact "
          + "config path?";
        status.className = "err";
      }
    }
  } catch (e) {
    status.textContent = (e.name === "AbortError")
      ? `Timed out after ${APPLY_TIMEOUT_MS / 1000}s waiting for a response `
        + "-- check the connection to the topside server and the drone."
      : "Request failed: " + e;
    status.className = "err";
  } finally {
    clearTimeout(timer);
  }
});

refreshFromDrone();
</script>
</body>
</html>
"""


def agent_base_url(host, port):
    """An IPv6 literal has to be bracketed inside a URL, or the colons in
    the address are read as the port separator. Link-local addresses also
    carry a %interface scope, which urllib accepts either raw or
    percent-encoded -- verified both against a live agent."""
    if ":" in host and not host.startswith("["):
        return f"http://[{host}]:{port}"
    return f"http://{host}:{port}"


def make_handler(source, agent_host, agent_port):
    agent_base = agent_base_url(agent_host, agent_port)
    index_html = INDEX_HTML_TEMPLATE.replace(
        "__EFFECT_FIELDS_JSON__", json.dumps(EFFECT_FIELDS)
    ).encode()

    class Handler(http.server.BaseHTTPRequestHandler):
        # /stream sends each frame as several small writes (see
        # _serve_stream's boundary/header/body/CRLF sequence) -- without
        # this, Nagle's algorithm can hold the first of those back waiting
        # to coalesce with the next one or an ACK, adding latency to every
        # single frame this process forwards to a browser.
        disable_nagle_algorithm = True

        def log_message(self, fmt, *log_args):
            pass  # the default per-request access log is just noise here

        def do_GET(self):
            if self.path == "/":
                # no-store: a browser tab left open across a server restart
                # (e.g. after a code update) must never keep running JS from
                # before the restart -- a stale Apply handler is exactly the
                # kind of thing that's silently wrong instead of loudly
                # broken.
                self._respond(200, "text/html", index_html,
                               extra_headers={"Cache-Control": "no-store"})
            elif self.path == "/stream":
                self._serve_stream()
            elif self.path == "/config":
                self._proxy_to_agent("GET")
            else:
                self.send_error(404)

        def do_POST(self):
            if self.path == "/config":
                self._proxy_to_agent("POST")
            else:
                self.send_error(404)

        def _respond(self, status, content_type, body, extra_headers=None):
            self.send_response(status)
            self.send_header("Content-Type", content_type)
            self.send_header("Content-Length", str(len(body)))
            for key, value in (extra_headers or {}).items():
                self.send_header(key, value)
            self.end_headers()
            self.wfile.write(body)

        def _proxy_to_agent(self, method):
            """Forwards GET/POST /config verbatim to the drone's
            config_agent and relays its response back to the browser --
            see this module's docstring for why this hop exists (avoids
            the browser needing to talk cross-origin to the drone
            directly)."""
            body = None
            if method == "POST":
                length = int(self.headers.get("Content-Length", 0))
                body = self.rfile.read(length)
            req = urllib.request.Request(
                f"{agent_base}/config", data=body, method=method,
                headers={"Content-Type": "application/json"} if body else {},
            )
            try:
                with urllib.request.urlopen(req, timeout=5) as resp:
                    self._respond(resp.status, "application/json", resp.read())
            except urllib.error.HTTPError as e:
                self._respond(e.code, "text/plain", e.read())
            except urllib.error.URLError as e:
                self._respond(
                    502, "text/plain",
                    f"could not reach config_agent at {agent_base}: {e.reason}".encode(),
                )

        def _serve_stream(self):
            boundary = "frame"
            self.send_response(200)
            self.send_header(
                "Content-Type", f"multipart/x-mixed-replace; boundary={boundary}"
            )
            self.end_headers()
            last_id = 0
            try:
                while True:
                    frame, last_id = source.wait_for_frame(last_id)
                    # One write, not five: even with Nagle disabled
                    # (disable_nagle_algorithm above), splitting a single
                    # frame across several small writes is needless syscall
                    # overhead on what's already a per-frame hot path.
                    header = (
                        f"--{boundary}\r\n"
                        f"Content-Type: image/jpeg\r\n"
                        f"Content-Length: {len(frame)}\r\n\r\n"
                    ).encode()
                    self.wfile.write(header + frame + b"\r\n")
            except (BrokenPipeError, ConnectionResetError):
                pass  # viewer closed the tab -- not an error

    return Handler


def discover_drone(stream_port, agent_port):
    """Finds the drone on a directly-connected cable, for --drone-host auto.

    Exists so the field workflow is just `python3 topside/web_ui.py
    --drone-host auto` with nothing to look up or type: on a bare tether
    there is no DHCP and no DNS, so the address is whatever link-local the
    drone happened to self-assign, and it is not worth an operator reading
    it off a screen before every dive.

    Resolves once, at startup, rather than re-discovering on every
    reconnect: FrameSource already retries a known host indefinitely, and
    re-running discovery inside that loop would let the UI silently latch
    onto a *different* machine mid-session if one appeared on the link.
    """
    sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)),
                                    "..", "tools"))
    try:
        import eeye_net
    except ImportError:
        print("--drone-host auto needs tools/eeye_net.py, which isn't next to "
              "this script; pass the drone's address explicitly instead.")
        return None

    # flush=True throughout: discovery takes a few seconds per link, and
    # Python block-buffers stdout whenever it isn't a terminal (a log file,
    # journald, an ssh pipe). Without this the operator sees nothing at all
    # while it works and reasonably concludes it has hung.
    print("Searching for the drone on directly-connected links...", flush=True)
    found = eeye_net.discover(stream_port=stream_port, agent_port=agent_port,
                              on_progress=lambda m: print(f"  {m}", flush=True))
    if not found:
        print("No drone found. Check the tether is plugged in at both ends "
              "and the\ndrone is powered, then run `tools/eeye-net discover` "
              "for details.", flush=True)
        return None
    if len(found) > 1:
        print(f"Found {len(found)} drones; using the first. Pass --drone-host "
              "explicitly to pick:", flush=True)
        for d in found:
            print(f"    {d['host']}", flush=True)
    host = found[0]["host"]
    print(f"Using drone at {host}", flush=True)
    return host


def main():
    parser = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter
    )
    parser.add_argument(
        "--drone-host", required=True,
        help="IP/hostname of the drone's Pi over the tether, or 'auto' to "
             "find it on the directly-connected cable (see tools/eeye-net)",
    )
    parser.add_argument(
        "--drone-stream-port", type=int, default=9000,
        help="stream_server port on the drone (default: 9000, matches "
             "eeye.c's stream_server_port)",
    )
    parser.add_argument(
        "--drone-agent-port", type=int, default=9001,
        help="config_agent.py's port on the drone (default: 9001, matches "
             "its own default)",
    )
    parser.add_argument(
        "--http-port", type=int, default=8080,
        help="port this script's own web server listens on (default: 8080)",
    )
    args = parser.parse_args()

    drone_host = args.drone_host
    if drone_host == "auto":
        drone_host = discover_drone(args.drone_stream_port, args.drone_agent_port)
        if drone_host is None:
            return 2

    source = FrameSource(drone_host, args.drone_stream_port)
    server = http.server.ThreadingHTTPServer(
        ("0.0.0.0", args.http_port),
        make_handler(source, drone_host, args.drone_agent_port),
    )
    print(f"Serving control UI on http://0.0.0.0:{args.http_port}/")
    server.serve_forever()


if __name__ == "__main__":
    sys.exit(main() or 0)
