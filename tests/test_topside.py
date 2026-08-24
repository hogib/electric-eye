#!/usr/bin/env python3
"""Topside-side tests: link staleness tracking and the config agent's
health relay.

The staleness logic is what stands between a pilot and flying on a frozen
picture, so it is tested directly rather than by eyeballing the overlay.
Standard library only, like the scripts it tests.
"""
import json
import os
import socket
import struct
import sys
import tempfile
import threading
import time
import unittest
import urllib.request

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "topside"))
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "pi"))

import web_ui          # noqa: E402
import config_agent    # noqa: E402


def loopback():
    """Whichever loopback this machine can actually bind.

    IPv6 loopback is the production case (the tether is link-local
    IPv6-only), but it is unavailable in some containers and CI sandboxes.
    The logic under test is address-family agnostic, so falling back keeps
    these tests runnable everywhere rather than silently skipped where it
    matters least.
    """
    probe = socket.socket(socket.AF_INET6, socket.SOCK_STREAM)
    try:
        probe.bind(("::1", 0))
        return socket.AF_INET6, "::1"
    except OSError:
        return socket.AF_INET, "127.0.0.1"
    finally:
        probe.close()


LOOPBACK_FAMILY, LOOPBACK_ADDR = loopback()


class FakeStreamServer:
    """Stands in for eeye's stream_server: accepts one client and sends
    length-prefixed JPEGs on demand, so a test can control exactly when
    frames stop arriving."""

    def __init__(self):
        self.sock = socket.socket(LOOPBACK_FAMILY, socket.SOCK_STREAM)
        self.sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        self.sock.bind((LOOPBACK_ADDR, 0))
        self.sock.listen(1)
        self.port = self.sock.getsockname()[1]
        self.client = None
        self._accepted = threading.Event()
        threading.Thread(target=self._accept, daemon=True).start()

    def _accept(self):
        try:
            self.client, _ = self.sock.accept()
            self._accepted.set()
        except OSError:
            pass

    def wait_accepted(self, timeout=5):
        return self._accepted.wait(timeout)

    def send_frame(self, payload=b"\xff\xd8fake\xff\xd9"):
        self.client.sendall(struct.pack("!I", len(payload)) + payload)

    def drop(self):
        if self.client:
            self.client.close()
            self.client = None

    def close(self):
        self.drop()
        self.sock.close()


class TestLinkStaleness(unittest.TestCase):
    """FrameSource.health() is what the overlay renders. Its whole job is
    telling a live picture from a frozen one."""

    def test_reports_no_frame_before_anything_arrives(self):
        src = web_ui.FrameSource(LOOPBACK_ADDR, 1)  # nothing listening
        h = src.health()
        self.assertFalse(h["have_frame"])
        self.assertIsNone(h["age_s"])
        self.assertFalse(h["connected"])

    def test_frame_age_grows_while_link_is_silent(self):
        server = FakeStreamServer()
        try:
            src = web_ui.FrameSource(LOOPBACK_ADDR, server.port)
            self.assertTrue(server.wait_accepted())
            server.send_frame()

            deadline = time.monotonic() + 5
            while time.monotonic() < deadline and not src.health()["have_frame"]:
                time.sleep(0.02)
            self.assertTrue(src.health()["have_frame"])

            first = src.health()["age_s"]
            self.assertLess(first, 1.0)  # just arrived
            time.sleep(0.4)
            later = src.health()["age_s"]
            # The age must actually advance -- a frozen picture is only
            # detectable because this number keeps climbing.
            self.assertGreater(later, first)
            self.assertGreater(later, 0.3)
        finally:
            server.close()

    def test_disconnect_is_reported(self):
        server = FakeStreamServer()
        try:
            src = web_ui.FrameSource(LOOPBACK_ADDR, server.port)
            self.assertTrue(server.wait_accepted())
            server.send_frame()
            deadline = time.monotonic() + 5
            while time.monotonic() < deadline and not src.health()["connected"]:
                time.sleep(0.02)
            self.assertTrue(src.health()["connected"])

            server.drop()
            deadline = time.monotonic() + 5
            while time.monotonic() < deadline and src.health()["connected"]:
                time.sleep(0.05)
            h = src.health()
            self.assertFalse(h["connected"])
            # The last frame is deliberately still available -- showing it
            # is fine, showing it *without saying so* is the hazard.
            self.assertTrue(h["have_frame"])
        finally:
            server.close()

    def test_wait_for_frame_times_out_instead_of_blocking_forever(self):
        """The bug this fixes: an unbounded wait meant the MJPEG stream
        stopped emitting parts when the drone went quiet, and the browser
        held the last frame on screen with no way to know."""
        src = web_ui.FrameSource(LOOPBACK_ADDR, 1)  # never connects
        started = time.monotonic()
        frame, fid = src.wait_for_frame(0, timeout=0.3)
        elapsed = time.monotonic() - started
        self.assertIsNone(frame)
        self.assertEqual(fid, 0)
        self.assertLess(elapsed, 2.0)
        self.assertGreaterEqual(elapsed, 0.25)

    def test_frame_counter_advances(self):
        server = FakeStreamServer()
        try:
            src = web_ui.FrameSource(LOOPBACK_ADDR, server.port)
            self.assertTrue(server.wait_accepted())
            for _ in range(3):
                server.send_frame()
            deadline = time.monotonic() + 5
            while time.monotonic() < deadline and src.health()["frames"] < 3:
                time.sleep(0.02)
            self.assertGreaterEqual(src.health()["frames"], 3)
        finally:
            server.close()


class TestAgentHealthRelay(unittest.TestCase):
    """config_agent's /health endpoint relays what eeye wrote, plus the
    file's age -- a health file that stopped updating means eeye is gone,
    which looks identical to a healthy one if you only read its contents."""

    def setUp(self):
        self.dir = tempfile.mkdtemp()
        self.config_path = os.path.join(self.dir, "eeye_config.json")
        self.health_path = self.config_path + ".health"
        # config_agent binds IPv6 dual-stack by design; where that is
        # unavailable, fall back so the relay logic is still exercised.
        if LOOPBACK_FAMILY == socket.AF_INET6:
            self.server = config_agent.DualStackHTTPServer(
                ("::1", 0), config_agent.make_handler(self.config_path)
            )
        else:
            import http.server
            self.server = http.server.ThreadingHTTPServer(
                ("127.0.0.1", 0), config_agent.make_handler(self.config_path)
            )
        self.port = self.server.server_address[1]
        threading.Thread(target=self.server.serve_forever, daemon=True).start()

    def tearDown(self):
        self.server.shutdown()
        self.server.server_close()

    def get_health(self):
        host = f"[{LOOPBACK_ADDR}]" if ":" in LOOPBACK_ADDR else LOOPBACK_ADDR
        url = f"http://{host}:{self.port}/health"
        with urllib.request.urlopen(url, timeout=5) as r:
            return json.loads(r.read())

    def test_missing_health_file_is_reported_not_faked(self):
        h = self.get_health()
        self.assertIn("error", h)  # must not invent a healthy-looking answer

    def test_relays_recording_state(self):
        with open(self.health_path, "w") as f:
            json.dump({"recording": {"state": "failed",
                                     "error": "write failed: No space left"}}, f)
        h = self.get_health()
        self.assertEqual(h["recording"]["state"], "failed")
        self.assertIn("No space left", h["recording"]["error"])

    def test_attaches_file_age(self):
        with open(self.health_path, "w") as f:
            json.dump({"recording": {"state": "active"}}, f)
        h = self.get_health()
        self.assertIn("age_s", h)
        self.assertLess(h["age_s"], 5)

        # A stale file must report a large age rather than looking fresh.
        old = time.time() - 120
        os.utime(self.health_path, (old, old))
        h = self.get_health()
        self.assertGreater(h["age_s"], 100)

    def test_corrupt_health_file_is_reported(self):
        with open(self.health_path, "w") as f:
            f.write("{not json")
        h = self.get_health()
        self.assertIn("error", h)


class TestRecorder(unittest.TestCase):
    """The recorder controller behind the UI's Record button.

    The bug these guard against, found in testing: stop() signalled only
    the direct child, so ffmpeg kept running and kept writing to a file
    the UI had already reported as stopped -- and because it never
    finalized, the resulting mp4 had no moov atom and would not play at
    all.
    """

    def setUp(self):
        self.dir = tempfile.mkdtemp()
        # A stand-in for tools/eeye-record that spawns a grandchild, so
        # the process-group behaviour is what is actually exercised.
        self.script = os.path.join(self.dir, "fake-record")
        with open(self.script, "w") as f:
            f.write(
                "#!/usr/bin/env python3\n"
                "import subprocess, sys, time, os\n"
                "out = sys.argv[sys.argv.index('-o') + 1]\n"
                "# Grandchild: writes continuously until signalled, the\n"
                "# same shape as ffmpeg under the real recorder.\n"
                "child = subprocess.Popen([sys.executable, '-c',\n"
                "    \"import sys,time\\n\"\n"
                "    \"f=open(sys.argv[1],'ab')\\n\"\n"
                "    \"import signal,os\\n\"\n"
                "    \"signal.signal(signal.SIGINT, lambda *a: os._exit(0))\\n\"\n"
                "    \"\\nwhile True:\\n    f.write(b'x'*1000); f.flush(); time.sleep(0.05)\",\n"
                "    out])\n"
                "# Exit quietly on SIGINT: an uncaught KeyboardInterrupt\n"
                "# traceback here is expected behaviour, but it clutters\n"
                "# the test output enough to hide a real failure.\n"
                "import signal\n"
                "signal.signal(signal.SIGINT, lambda *a: os._exit(0))\n"
                "try:\n"
                "    child.wait()\n"
                "except KeyboardInterrupt:\n"
                "    os._exit(0)\n"
            )
        os.chmod(self.script, 0o755)

    def make_recorder(self):
        r = web_ui.Recorder(self.script, 8080, self.dir)
        # available() also checks for ffmpeg; the fake script does not
        # need it, so bypass that one check rather than skipping the test
        # on machines without ffmpeg installed.
        r.available = lambda: True
        return r

    def test_start_then_status_reports_recording(self):
        r = self.make_recorder()
        ok, detail = r.start()
        self.assertTrue(ok, detail)
        time.sleep(0.5)
        st = r.status()
        self.assertTrue(st["recording"])
        self.assertIsNotNone(st["file"])
        r.stop()

    def test_stop_actually_stops_the_grandchild(self):
        """The regression test: after stop() returns, nothing may still be
        writing to the file."""
        r = self.make_recorder()
        ok, _ = r.start()
        self.assertTrue(ok)
        time.sleep(0.6)
        path = r._path
        r.stop()

        size_after_stop = os.path.getsize(path)
        time.sleep(0.8)
        self.assertEqual(os.path.getsize(path), size_after_stop,
                         "file kept growing after stop() returned")
        self.assertFalse(r.status()["recording"])

    def test_double_start_is_refused(self):
        r = self.make_recorder()
        self.assertTrue(r.start()[0])
        ok, detail = r.start()
        self.assertFalse(ok)
        self.assertIn("already", detail)
        r.stop()

    def test_stop_without_start_is_refused(self):
        r = self.make_recorder()
        ok, detail = r.stop()
        self.assertFalse(ok)
        self.assertIn("not recording", detail)

    def test_unavailable_recorder_reports_rather_than_crashing(self):
        r = web_ui.Recorder("/nonexistent/eeye-record", 8080, self.dir)
        self.assertFalse(r.available())
        ok, detail = r.start()
        self.assertFalse(ok)
        self.assertIn("missing", detail)
        self.assertFalse(r.status()["available"])


class TestAgentBaseUrl(unittest.TestCase):
    """IPv6 literals must be bracketed in a URL or the colons read as a
    port separator -- link-local addresses are the normal case on a bare
    tether, so this is not an edge case here."""

    def test_ipv6_is_bracketed(self):
        self.assertEqual(web_ui.agent_base_url("fe80::1%eth0", 9001),
                         "http://[fe80::1%eth0]:9001")

    def test_ipv4_and_hostname_are_untouched(self):
        self.assertEqual(web_ui.agent_base_url("192.168.1.5", 9001),
                         "http://192.168.1.5:9001")
        self.assertEqual(web_ui.agent_base_url("drone.local", 9001),
                         "http://drone.local:9001")


if __name__ == "__main__":
    unittest.main(verbosity=2)
