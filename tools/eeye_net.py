"""
Shared plumbing for finding an Electric Eye drone on a directly-connected
cable, and for checking that the link is actually carrying what it should.

Imported by tools/eeye-net (the operator-facing CLI) and by
topside/web_ui.py's --drone-host auto. Standard library only.

WHY LINK-LOCAL RATHER THAN DHCP
-------------------------------
Field deployments have no topside network: the drone's Pi and the topside
machine are joined by one Ethernet cable, with no router, no DHCP server
and no DNS. That rules out anything needing an addressing authority.

Every IPv6 host self-assigns an fe80::/64 address on every link that comes
up (RFC 4862), with no server involved -- so on a bare cable, link-local is
the one addressing scheme guaranteed to already work. Reaching such an
address requires naming the interface too (every link has its own fe80::/64,
so the address alone is ambiguous), which is what the "scope ID" in a
4-tuple sockaddr_in6 is for -- see scoped_connect() below.

Discovery, rather than configuration, is the point: instead of pinning
addresses on both ends and keeping them in sync by hand, ask the link who is
out there and probe each answer for something that behaves like eeye.
"""
import json
import socket
import struct
import subprocess
import time

# eeye's own defaults -- stream_server_port in src/eeye.c, and
# config_agent.py's --port default.
DEFAULT_STREAM_PORT = 9000
DEFAULT_AGENT_PORT = 9001


def _ip_json(*args):
    """Runs `ip -j ...` and returns parsed JSON, or [] if that isn't
    possible. -j (rather than scraping human output) because the text
    format varies across iproute2 versions and quietly changes columns;
    the JSON schema is stable."""
    try:
        out = subprocess.run(("ip", "-j") + args, capture_output=True,
                             text=True, timeout=5)
    except (OSError, subprocess.TimeoutExpired):
        return []
    if out.returncode != 0:
        return []
    try:
        return json.loads(out.stdout or "[]")
    except json.JSONDecodeError:
        return []


def candidate_interfaces():
    """Interfaces worth searching for a drone, best-first.

    Filters to links that are actually up with a carrier: LOWER_UP means
    the physical layer is live, which on Ethernet means a cable is plugged
    in at both ends. A port with no carrier can't have a drone behind it,
    and skipping those keeps discovery from spending its timeout budget on
    empty links.

    Loopback is excluded (nothing external is behind it) and so are
    point-to-point tunnels like wireguard, where "the link" isn't a cable
    and multicast discovery is meaningless.

    Wired interfaces sort first: this whole module exists for a direct
    Ethernet tether, so when a machine has both, the cable is the better
    guess and gets its probe budget spent first.
    """
    result = []
    for link in _ip_json("link", "show"):
        name = link.get("ifname")
        flags = link.get("flags", [])
        if not name or name == "lo":
            continue
        if "LOOPBACK" in flags or "POINTOPOINT" in flags:
            continue
        if "UP" not in flags or "LOWER_UP" not in flags:
            continue
        result.append(name)
    # en*/eth* (wired) before wl* (wireless); stable order within each group.
    result.sort(key=lambda n: (not n.startswith(("en", "eth")), n))
    return result


def link_is_up(iface):
    """(up, detail) for one interface -- used by `check` to tell "no cable"
    apart from "cable, but nothing answering", which have completely
    different fixes."""
    for link in _ip_json("link", "show", iface):
        flags = link.get("flags", [])
        if "LOWER_UP" in flags:
            return True, "up, carrier present"
        if "UP" in flags:
            return False, ("admin up but NO-CARRIER (cable unplugged, or "
                           "nothing powered on the other end)")
        return False, "administratively down"
    return False, "no such interface"


def neighbours(iface):
    """Link-local addresses the kernel already knows on this interface.

    Free to consult -- it's just the neighbour cache -- but only sees peers
    something has talked to recently, so it complements rather than replaces
    the multicast sweep below. Excludes anything flagged as a router: a
    drone is an endpoint, and on a shared network the routers are exactly
    the noise worth dropping.
    """
    found = []
    for entry in _ip_json("-6", "neigh", "show", "dev", iface):
        dst = entry.get("dst", "")
        if not dst.startswith("fe80::"):
            continue
        if "router" in entry:
            continue
        if entry.get("state") == ["FAILED"]:
            continue
        found.append(dst)
    return found


def ping_all_nodes(iface, wait_s=2):
    """Every IPv6 host on a link must join ff02::1 (all-nodes multicast) and
    answer an echo request to it, so one ping enumerates the whole link
    without needing root, a raw socket, or an address range to scan.

    Responders come back as ICMP "duplicates" (one request, many replies),
    which is why this parses reply lines directly instead of trusting
    ping's own summary counters.
    """
    try:
        out = subprocess.run(
            # No -i: unprivileged multicast ping is rejected outright for
            # intervals under 1000ms ("minimal interval for multicast ping
            # for user must be >= 1000 ms"), and the default 1s is fine.
            ("ping", "-6", "-c", "2", "-W", "1", f"ff02::1%{iface}"),
            capture_output=True, text=True, timeout=wait_s + 6,
        )
    except (OSError, subprocess.TimeoutExpired):
        return []

    found = []
    for line in out.stdout.splitlines():
        # "64 bytes from fe80::...%wlo1: icmp_seq=1 ttl=64 time=1.34 ms"
        if "bytes from " not in line:
            continue
        rest = line.split("bytes from ", 1)[1]
        if not rest.startswith("fe80"):
            continue
        # "fe80::1%wlo1: icmp_seq=1 ..." -> strip the scope and trailing colon
        addr = rest.split(" ", 1)[0].rstrip(":").split("%")[0]
        if addr and addr not in found:
            found.append(addr)
    return found


def scoped_connect(host, port, iface=None, timeout=2.0):
    """Opens a TCP connection, handling IPv6 link-local scope correctly.

    A link-local address is only meaningful together with the interface to
    use it on -- fe80::1 on the tether and fe80::1 on wifi are different
    machines. The kernel wants that as a numeric scope ID in the sockaddr's
    4th field, so this resolves the interface name to an index and passes
    a 4-tuple. Accepts the address either as "fe80::1%eth0" (embedded
    scope) or as a bare address plus an explicit iface.

    Falls through to ordinary getaddrinfo for global addresses and
    hostnames, so callers don't need to care which kind they were handed.
    """
    if "%" in host:
        host, _, embedded = host.partition("%")
        iface = iface or embedded

    if host.startswith("fe80"):
        if not iface:
            raise ValueError(
                f"{host} is a link-local address, which needs an interface "
                "to be reachable (pass one, or write it as "
                f"{host}%<interface>)")
        sock = socket.socket(socket.AF_INET6, socket.SOCK_STREAM)
        sock.settimeout(timeout)
        try:
            sock.connect((host, port, 0, socket.if_nametoindex(iface)))
        except Exception:
            sock.close()
            raise
        return sock

    # Not link-local: let getaddrinfo pick the family (v4 or v6).
    return socket.create_connection((host, port), timeout=timeout)


def format_host(addr, iface):
    """How an address should be written so it survives being passed back in
    on a command line -- link-local addresses need their interface, and the
    %iface suffix is the portable way to carry it."""
    if addr.startswith("fe80") and iface and "%" not in addr:
        return f"{addr}%{iface}"
    return addr


def probe_agent(host, port=DEFAULT_AGENT_PORT, iface=None, timeout=2.0):
    """Is config_agent.py answering here?

    Deliberately stricter than a port check: it must return HTTP 200 on
    GET /config *and* the body must parse as JSON. Anything else on 9001
    (a different service, a captive portal, a half-open socket) is not a
    drone, and treating "the port is open" as proof is how a wrong host
    gets chosen.

    Returns (ok, detail, config_dict_or_None).
    """
    try:
        sock = scoped_connect(host, port, iface, timeout)
    except OSError as e:
        return False, f"no answer on port {port} ({e.strerror or e})", None
    except ValueError as e:
        return False, str(e), None

    try:
        sock.settimeout(timeout)
        sock.sendall(b"GET /config HTTP/1.0\r\nHost: eeye\r\n\r\n")
        chunks = []
        while True:
            chunk = sock.recv(65536)
            if not chunk:
                break
            chunks.append(chunk)
            if sum(len(c) for c in chunks) > 1024 * 1024:
                break  # a sane config is tiny; don't read forever
        raw = b"".join(chunks)
    except OSError as e:
        return False, f"connected to port {port} but the read failed ({e})", None
    finally:
        sock.close()

    head, _, body = raw.partition(b"\r\n\r\n")
    if not head.startswith(b"HTTP/"):
        return False, (f"something is listening on port {port}, but it isn't "
                       "speaking HTTP -- this is probably not the drone"), None
    status = head.split(b"\r\n", 1)[0]
    if b" 200" not in status:
        return False, f"port {port} answered {status.decode(errors='replace')}", None
    try:
        cfg = json.loads(body)
    except json.JSONDecodeError:
        return False, (f"port {port} returned HTTP 200 but the body isn't "
                       "JSON -- probably not config_agent"), None
    return True, "config_agent responding", cfg


def probe_stream(host, port=DEFAULT_STREAM_PORT, iface=None, connect_timeout=2.0,
                 frame_timeout=6.0):
    """Does the video tap actually deliver a frame?

    This reads a whole frame rather than just connecting, because
    connecting proves almost nothing here. Verified against the real
    binary: with "stream_frame_interval": 0 the server accepts the TCP
    connection and then sends nothing, forever. A port check calls that
    healthy while the operator stares at a black screen with no error
    anywhere -- so "connected, then silent" is reported as its own distinct
    state, with its own fix, rather than being lumped in with success or
    with connection refused.

    Wire format is stream_server.h's: 4-byte big-endian length, then that
    many bytes of one complete JPEG.

    Returns (state, detail, info) where state is one of:
      "ok"       -- a full frame arrived; info has its size and dimensions
      "silent"   -- connected, but no frame within frame_timeout
      "refused"  -- nothing listening
      "bad"      -- something answered but isn't speaking this protocol
    """
    try:
        sock = scoped_connect(host, port, iface, connect_timeout)
    except OSError as e:
        return "refused", f"nothing listening on port {port} ({e.strerror or e})", None
    except ValueError as e:
        return "refused", str(e), None

    try:
        sock.settimeout(frame_timeout)
        started = time.monotonic()
        header = _recv_exact(sock, 4)
        if header is None:
            return ("silent",
                    f"connected to port {port}, but no frame arrived within "
                    f"{frame_timeout:.0f}s", None)
        (length,) = struct.unpack("!I", header)
        # A frame that isn't plausibly a JPEG means we're talking to the
        # wrong service, not that the drone is unhealthy.
        if length < 2 or length > 64 * 1024 * 1024:
            return ("bad",
                    f"port {port} sent an implausible frame length "
                    f"({length} bytes) -- probably not eeye's stream", None)
        payload = _recv_exact(sock, length)
        if payload is None:
            return ("bad",
                    f"port {port} announced a {length}-byte frame but the "
                    "connection closed mid-frame", None)
        elapsed = time.monotonic() - started
        if payload[:2] != b"\xff\xd8":
            return ("bad",
                    f"port {port} sent {length} bytes that aren't a JPEG "
                    "(no SOI marker)", None)
        info = {"bytes": length, "seconds": elapsed}
        dims = _jpeg_dimensions(payload)
        if dims:
            info["width"], info["height"] = dims
        return "ok", "stream delivering frames", info
    except OSError as e:
        return "bad", f"port {port} connected but then failed ({e})", None
    finally:
        sock.close()


def _recv_exact(sock, n):
    """Reads exactly n bytes, or None if the socket times out or closes
    first. Returning None rather than raising keeps the two genuinely
    different outcomes -- "nothing came" and "it broke" -- separable by the
    caller."""
    buf = bytearray()
    while len(buf) < n:
        try:
            chunk = sock.recv(n - len(buf))
        except socket.timeout:
            return None
        if not chunk:
            return None
        buf.extend(chunk)
    return bytes(buf)


def _jpeg_dimensions(data):
    """Pulls width/height out of a JPEG's frame header.

    Walks marker segments rather than scanning for the SOF bytes, since a
    thumbnail or vendor blob inside an APP segment can contain anything.
    Same reasoning as find_jpeg_end() in src/rpicam_in.c.
    """
    i = 2
    while i + 9 < len(data):
        if data[i] != 0xFF:
            i += 1
            continue
        marker = data[i + 1]
        if marker == 0xFF:
            i += 1
            continue
        if marker == 0xD8 or 0xD0 <= marker <= 0xD7:
            i += 2
            continue
        if marker in (0xC0, 0xC1, 0xC2, 0xC3):
            height, width = struct.unpack(">HH", data[i + 5:i + 9])
            return width, height
        if marker == 0xDA:  # SOS: image data follows, no more headers
            return None
        seg_len = struct.unpack(">H", data[i + 2:i + 4])[0]
        if seg_len < 2:
            return None
        i += 2 + seg_len
    return None


def discover(stream_port=DEFAULT_STREAM_PORT, agent_port=DEFAULT_AGENT_PORT,
             interfaces=None, on_progress=None):
    """Searches every live link for something that answers like a drone.

    A candidate qualifies on config_agent (port 9001 returning valid JSON),
    not on the video stream: the stream tap is off by default
    (stream_frame_interval starts at 0), so a perfectly healthy drone can
    legitimately have nothing on 9000. Requiring video here would hide
    exactly the drones most in need of being found.

    Returns a list of dicts, best-first, each with host/iface/agent/stream.
    """
    results = []
    for iface in (interfaces or candidate_interfaces()):
        if on_progress:
            on_progress(f"searching {iface}...")

        seen = []
        for addr in neighbours(iface) + ping_all_nodes(iface):
            if addr not in seen:
                seen.append(addr)

        for addr in seen:
            ok, detail, cfg = probe_agent(addr, agent_port, iface, timeout=1.5)
            if not ok:
                continue
            host = format_host(addr, iface)
            state, stream_detail, info = probe_stream(
                addr, stream_port, iface, connect_timeout=1.5,
                frame_timeout=4.0)
            results.append({
                "host": host, "iface": iface, "config": cfg,
                "agent_detail": detail, "stream_state": state,
                "stream_detail": stream_detail, "stream_info": info,
            })
            if on_progress:
                on_progress(f"  found a drone at {host}")
    return results
