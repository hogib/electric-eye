#!/usr/bin/env python3
"""
Milestone 1 topside preview server.

Connects to the drone's stream_server (see src/stream_server.c) over a
plain TCP socket, reads its length-prefixed JPEG frames, and re-serves them
to a browser as MJPEG-over-HTTP -- an <img> tag on the browser side needs
nothing more than that to show a live feed, no video-player JS at all.

Only needs the Python standard library -- nothing to install.

Usage:
    python3 preview_server.py --drone-host 192.168.1.50

Then point a browser at http://<this machine's own IP>:8080/

The drone side must have a non-zero "stream_frame_interval" in its running
config (see config.h's JSON schema doc comment) -- the tap is off by
default, so nothing arrives here until that's set.
"""
import argparse
import http.server
import socket
import struct
import threading
import time


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


INDEX_HTML = b"""<!doctype html>
<html>
<head><title>Electric Eye - Live Preview</title></head>
<body style="margin:0;background:#000">
  <img src="/stream" style="width:100%;height:100vh;object-fit:contain">
</body>
</html>
"""


def make_handler(source):
    class Handler(http.server.BaseHTTPRequestHandler):
        def log_message(self, fmt, *log_args):
            pass  # the default per-request access log is just noise here

        def do_GET(self):
            if self.path == "/":
                self.send_response(200)
                self.send_header("Content-Type", "text/html")
                self.send_header("Content-Length", str(len(INDEX_HTML)))
                self.end_headers()
                self.wfile.write(INDEX_HTML)
            elif self.path == "/stream":
                self._serve_stream()
            else:
                self.send_error(404)

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
        "--drone-port", type=int, default=9000,
        help="stream_server port on the drone (default: 9000, matches "
             "eeye.c's stream_server_port)",
    )
    parser.add_argument(
        "--http-port", type=int, default=8080,
        help="port this script's own web server listens on (default: 8080)",
    )
    args = parser.parse_args()

    source = FrameSource(args.drone_host, args.drone_port)
    server = http.server.ThreadingHTTPServer(
        ("0.0.0.0", args.http_port), make_handler(source)
    )
    print(f"Serving live preview on http://0.0.0.0:{args.http_port}/")
    server.serve_forever()


if __name__ == "__main__":
    main()
