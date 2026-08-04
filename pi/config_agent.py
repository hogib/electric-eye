#!/usr/bin/env python3
"""
Runs on the drone's Pi, alongside eeye. The entire amount of "control"
plumbing needed on this side: accepts a new config over the network and
writes it to disk, where eeye's own inotify watcher (see src/config.c)
picks it up and hot-reloads within ~200ms. This script never talks to the
eeye process directly, and eeye never talks to the network directly --
eeye stays exactly what it's always been, a local pipeline reacting to a
local file; everything network-facing lives here instead.

Deliberately does not re-validate the config's schema beyond "is this
well-formed JSON" -- config.c already does thorough validation on reload
and logs exactly why an invalid config was rejected (see eeye's own
stdout/journalctl), so duplicating that logic here would just create a
second place that can disagree with the first about what's valid.

Only needs the Python standard library -- nothing to install.

Usage:
    python3 config_agent.py --config-path /opt/electric-eye/eeye_config.json

Serves, on all interfaces:
  GET  /config   the current config file's raw contents
  POST /config   replace the file's contents with the request body (must be
                 well-formed JSON; written atomically -- see
                 write_config_atomic() below)
"""
import argparse
import http.server
import json
import os


def write_config_atomic(path, data: bytes):
    """The writer contract config.c's watcher requires (see config.h's own
    doc comment): write to a temp file in the same directory, then
    rename() onto the target. A plain in-place overwrite could be observed
    mid-write as a truncated, invalid file -- inotify watches the
    directory rather than the file itself for exactly this reason, and
    rename() is what makes that safe."""
    directory = os.path.dirname(path) or "."
    tmp_path = os.path.join(directory, f".{os.path.basename(path)}.tmp")
    with open(tmp_path, "wb") as f:
        f.write(data)
        f.flush()
        os.fsync(f.fileno())
    os.replace(tmp_path, path)


def make_handler(config_path):
    class Handler(http.server.BaseHTTPRequestHandler):
        def log_message(self, fmt, *log_args):
            print("[config_agent]", fmt % log_args)

        def do_GET(self):
            if self.path != "/config":
                self.send_error(404)
                return
            try:
                with open(config_path, "rb") as f:
                    body = f.read()
            except FileNotFoundError:
                body = b"{}"  # matches config.c's own empty-object default
            self.send_response(200)
            self.send_header("Content-Type", "application/json")
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            self.wfile.write(body)

        def do_POST(self):
            if self.path != "/config":
                self.send_error(404)
                return
            length = int(self.headers.get("Content-Length", 0))
            body = self.rfile.read(length)
            try:
                json.loads(body)  # only confirms it's well-formed JSON
            except json.JSONDecodeError as e:
                self.send_response(400)
                self.send_header("Content-Type", "text/plain")
                self.end_headers()
                self.wfile.write(f"invalid JSON: {e}".encode())
                return

            write_config_atomic(config_path, body)
            print(f"[config_agent] wrote new config to {config_path}")
            self.send_response(200)
            self.end_headers()

    return Handler


def main():
    parser = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter
    )
    parser.add_argument(
        "--config-path", default="eeye_config.json",
        help="the exact path eeye was started with (default: eeye_config.json)",
    )
    parser.add_argument(
        "--port", type=int, default=9001,
        help="port to listen on (default: 9001)",
    )
    args = parser.parse_args()

    server = http.server.ThreadingHTTPServer(
        ("0.0.0.0", args.port), make_handler(args.config_path)
    )
    print(f"config_agent listening on :{args.port}, writing to {args.config_path}")
    server.serve_forever()


if __name__ == "__main__":
    main()
