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
import socket
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
    "blur": [["blur_strength", 0, 255, 0]],
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
  .stage input[type=number] { width: 60px; }
  .stage .spacer { flex: 1; }
  button { font-size: 13px; padding: 4px 10px; cursor: pointer; }
  #apply { font-size: 15px; padding: 8px 20px; margin-top: 16px; }
  #status { margin-top: 10px; font-size: 13px; white-space: pre-wrap; }
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

    <button id="apply" type="button">Apply</button>
    <div id="status"></div>
  </div>

<script>
const EFFECT_FIELDS = __EFFECT_FIELDS_JSON__;
const stagesEl = document.getElementById("stages");

function fieldRowHtml(stage, key, min, max, def) {
  const val = (stage && stage[key] !== undefined) ? stage[key] : def;
  return `<label>${key}<input type="number" min="${min}" max="${max}" `
       + `value="${val}" data-field="${key}"></label>`;
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
  };
}

function loadConfig(cfg) {
  stagesEl.innerHTML = "";
  for (const stage of (cfg.chain || [])) addStage(stage);
  if (stagesEl.children.length === 0) addStage();
  document.getElementById("record_path").value = cfg.record_path || "";
  document.getElementById("stream_frame_interval").value = cfg.stream_frame_interval || 0;
  document.getElementById("stream_quality").value = cfg.stream_quality || 60;
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

document.getElementById("apply").addEventListener("click", async () => {
  const status = document.getElementById("status");
  status.textContent = "Applying...";
  status.className = "";
  try {
    const res = await fetch("/config", {
      method: "POST",
      headers: { "Content-Type": "application/json" },
      body: JSON.stringify(buildConfig()),
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
    status.textContent = "Request failed: " + e;
    status.className = "err";
  }
});

refreshFromDrone();
</script>
</body>
</html>
"""


def make_handler(source, agent_host, agent_port):
    agent_base = f"http://{agent_host}:{agent_port}"
    index_html = INDEX_HTML_TEMPLATE.replace(
        "__EFFECT_FIELDS_JSON__", json.dumps(EFFECT_FIELDS)
    ).encode()

    class Handler(http.server.BaseHTTPRequestHandler):
        def log_message(self, fmt, *log_args):
            pass  # the default per-request access log is just noise here

        def do_GET(self):
            if self.path == "/":
                self._respond(200, "text/html", index_html)
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

        def _respond(self, status, content_type, body):
            self.send_response(status)
            self.send_header("Content-Type", content_type)
            self.send_header("Content-Length", str(len(body)))
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
                    self.wfile.write(f"--{boundary}\r\n".encode())
                    self.wfile.write(b"Content-Type: image/jpeg\r\n")
                    self.wfile.write(f"Content-Length: {len(frame)}\r\n\r\n".encode())
                    self.wfile.write(frame)
                    self.wfile.write(b"\r\n")
            except (BrokenPipeError, ConnectionResetError):
                pass  # viewer closed the tab -- not an error

    return Handler


def main():
    parser = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter
    )
    parser.add_argument(
        "--drone-host", required=True,
        help="IP/hostname of the drone's Pi over the tether",
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

    source = FrameSource(args.drone_host, args.drone_stream_port)
    server = http.server.ThreadingHTTPServer(
        ("0.0.0.0", args.http_port),
        make_handler(source, args.drone_host, args.drone_agent_port),
    )
    print(f"Serving control UI on http://0.0.0.0:{args.http_port}/")
    server.serve_forever()


if __name__ == "__main__":
    main()
