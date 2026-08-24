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
second place that can disagree with the first about what's valid. Instead,
a POST here waits briefly for eeye's own verdict (see
wait_for_reload_status() below) and relays *that* -- true confirmation
that the config took effect, not just that this script managed to write a
file.

Only needs the Python standard library -- nothing to install.

Usage:
    python3 config_agent.py --config-path /opt/electric-eye/eeye_config.json

Serves on all interfaces, IPv6 and IPv4 alike (see DualStackHTTPServer
below -- the IPv6 half is what makes this reachable over a direct cable,
where link-local is the only addressing available):
  GET  /health   eeye's own runtime health -- above all whether recording
                 is actually still running (see src/health.h). Served from
                 the "<config-path>.health" file eeye republishes on a
                 timer; a missing or stale file means eeye isn't writing
                 one, which is itself the answer.
  GET  /config   the current config file's raw contents
  POST /config   replace the file's contents with the request body (must be
                 well-formed JSON; written atomically -- see
                 write_config_atomic() below), then respond with JSON:
                 {"written": true, "eeye_accepted": true|false|null}
                 -- null means eeye didn't report back within the timeout
                 (not running, or hot-reload disabled -- check its own log)
"""
import argparse
import http.server
import json
import os
import socket
import time


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


def wait_for_reload_status(config_path, previous_mtime, timeout=2.0):
    """Polls "<config_path>.status" (see config.h's doc comment on
    ConfigWatcher) until it changes from whatever it was before we wrote
    the new config, then returns its "ok" value -- or None on timeout,
    meaning eeye never weighed in (not running, watching a different path,
    or hot-reload disabled). previous_mtime lets this tell "eeye just
    processed our write" apart from "that's a stale status left over from
    before we even wrote anything" -- both look identical as a bare file
    read, only the mtime comparison distinguishes them."""
    status_path = config_path + ".status"
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        try:
            mtime = os.path.getmtime(status_path)
        except FileNotFoundError:
            mtime = None
        if mtime is not None and mtime != previous_mtime:
            try:
                with open(status_path) as f:
                    return json.load(f).get("ok")
            except (OSError, json.JSONDecodeError):
                pass  # caught mid-write (rename isn't visible until it completes,
                      # but guard anyway); keep polling
        time.sleep(0.1)
    return None


class DualStackHTTPServer(http.server.ThreadingHTTPServer):
    """Listens on IPv6 *and* IPv4 from one socket.

    The stock ThreadingHTTPServer is AF_INET only, which makes it
    unreachable over IPv6 link-local (fe80::/64) -- and link-local is the
    only addressing that works with no configuration at all on a direct
    Ethernet tether, where there is no DHCP server and no DNS. Binding "::"
    with IPV6_V6ONLY off accepts both families, so an IPv4 client on a
    normal LAN keeps working exactly as before.

    Mirrors the same change in src/stream_server.c, so both of the drone's
    ports behave identically.
    """
    address_family = socket.AF_INET6

    def server_bind(self):
        # Not fatal if it fails: the socket still serves IPv6, which is the
        # case that needs it most. Linux defaults this off already
        # (net.ipv6.bindv6only=0), but that is an operator-flippable sysctl,
        # so set it explicitly rather than inherit whatever the host has.
        try:
            self.socket.setsockopt(socket.IPPROTO_IPV6, socket.IPV6_V6ONLY, 0)
        except OSError as e:
            print(f"[config_agent] could not enable dual-stack ({e}); "
                  "IPv4 clients may not be able to connect")
        super().server_bind()


def make_handler(config_path):
    class Handler(http.server.BaseHTTPRequestHandler):
        def log_message(self, fmt, *log_args):
            print("[config_agent]", fmt % log_args)

        def do_GET(self):
            if self.path == "/health":
                self._serve_health()
                return
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

            status_path = config_path + ".status"
            try:
                previous_mtime = os.path.getmtime(status_path)
            except FileNotFoundError:
                previous_mtime = None

            write_config_atomic(config_path, body)
            print(f"[config_agent] wrote new config to {config_path}")

            eeye_accepted = wait_for_reload_status(config_path, previous_mtime)
            if eeye_accepted is None:
                print("[config_agent] no reload confirmation from eeye within "
                      "the timeout -- is it running and watching this path?")

            response = json.dumps(
                {"written": True, "eeye_accepted": eeye_accepted}
            ).encode()
            self.send_response(200)
            self.send_header("Content-Type", "application/json")
            self.send_header("Content-Length", str(len(response)))
            self.end_headers()
            self.wfile.write(response)

        def _serve_health(self):
            """Relays eeye's health file, with the file's own age attached.

            The age matters as much as the contents: a health file that
            stopped being updated means eeye is wedged or gone, which
            looks identical to a healthy one if you only read the JSON.
            Reported rather than judged here -- what counts as too old
            depends on eeye's publish interval, which topside knows.
            """
            health_path = config_path + ".health"
            try:
                with open(health_path, "rb") as hf:
                    raw = hf.read()
                age = time.time() - os.path.getmtime(health_path)
                payload = json.loads(raw)
                payload["age_s"] = round(age, 1)
                body = json.dumps(payload).encode()
            except FileNotFoundError:
                body = json.dumps({
                    "error": "no health file; is eeye running with this "
                             "config path?",
                }).encode()
            except (OSError, json.JSONDecodeError) as e:
                body = json.dumps({"error": f"unreadable health file: {e}"}).encode()

            self.send_response(200)
            self.send_header("Content-Type", "application/json")
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            self.wfile.write(body)

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

    server = DualStackHTTPServer(
        ("::", args.port), make_handler(args.config_path)
    )
    print(f"config_agent listening on :{args.port}, writing to {args.config_path}")
    server.serve_forever()


if __name__ == "__main__":
    main()
