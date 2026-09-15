#!/usr/bin/env python3
"""herdr status bridge.

Reads the live agent status from the herdr daemon over its Unix socket and serves a
tiny JSON document (``GET /state``) over HTTP on the LAN so the Waveshare
ESP32-C6-Touch-LCD-1.47 companion can poll it without a USB link.

Python 3 standard library only -- no third-party imports.

The herdr socket speaks newline-delimited JSON: one request line in, one reply line
out. The server closes the connection after the first response, so this module opens a
NEW connection per herdr request and never reuses one (a second request on a reused
connection fails with BrokenPipeError). Connect-per-call measures at ~0.6 ms, so this
costs nothing at the 0.5 s poll cadence.

Usage:
    python3 bridge/herdr_status_bridge.py
    python3 bridge/herdr_status_bridge.py --once
    python3 bridge/herdr_status_bridge.py --fixture bridge/fixtures/blocked.json
"""

import argparse
import json
import os
import socket
import sys
import threading
import time
import uuid
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path

# --- constants --------------------------------------------------------------

#: Rank used to sort agents so the most important one is always the first list row.
STATE_RANK = {"blocked": 0, "working": 1, "done": 2, "idle": 3, "unknown": 4}

#: A successful poll older than this marks the wire state as stale.
STALE_AFTER_S = 5.0

#: Minimum spacing between "poll failed" warnings on stderr.
FAIL_WARN_PERIOD_S = 30.0


class BridgeError(Exception):
    """Any failure talking to herdr (socket error, empty reply, bad JSON, error key)."""


# --- herdr socket client ----------------------------------------------------


def resolve_socket_path(explicit):
    """--socket > $HERDR_SOCKET_PATH > $XDG_CONFIG_HOME/herdr/herdr.sock > ~/.config/herdr/herdr.sock."""
    if explicit:
        return explicit
    env = os.environ.get("HERDR_SOCKET_PATH")
    if env:
        return env
    xdg = os.environ.get("XDG_CONFIG_HOME")
    if xdg:
        return str(Path(xdg) / "herdr" / "herdr.sock")
    return str(Path.home() / ".config" / "herdr" / "herdr.sock")


def herdr_call(method, params, socket_path, timeout=2.0):
    """Issue one herdr request and return the parsed reply.

    One connection per call: the herdr server closes the socket after the first
    response, so a connection is never reused.
    """
    request = json.dumps({"id": uuid.uuid4().hex, "method": method, "params": params})
    try:
        sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        try:
            sock.settimeout(timeout)
            sock.connect(socket_path)
            sock.sendall((request + "\n").encode("utf-8"))
            reply = sock.makefile("rb").readline()
        finally:
            sock.close()
    except OSError as exc:
        raise BridgeError("herdr %s on %s: %s" % (method, socket_path, exc))

    if not reply.strip():
        raise BridgeError("herdr %s: empty reply (server closed without answering)" % method)
    try:
        resp = json.loads(reply.decode("utf-8", "replace"))
    except ValueError as exc:
        raise BridgeError("herdr %s: unparseable reply: %s" % (method, exc))
    if not isinstance(resp, dict):
        raise BridgeError("herdr %s: reply is not an object" % method)
    if resp.get("error") is not None:
        raise BridgeError("herdr %s: %s" % (method, resp["error"]))
    return resp


def sanitize(s, n):
    """Keep bytes 0x20..0x7E, map everything else to a space, collapse, strip, truncate.

    herdr prefixes terminal titles with non-ASCII markers (e.g. "\\u03c0 > ", "\\u2838"),
    which the device's ASCII-only Montserrat fonts would render as missing glyphs.
    """
    if not isinstance(s, str):
        s = "" if s is None else str(s)
    cleaned = "".join(ch if 0x20 <= ord(ch) <= 0x7E else " " for ch in s)
    return " ".join(cleaned.split())[:n]


def _label_maps(snapshot):
    tabs = snapshot.get("tabs")
    tabs_by_id = {}
    if isinstance(tabs, list):
        for tab in tabs:
            if isinstance(tab, dict) and tab.get("tab_id"):
                tabs_by_id[tab["tab_id"]] = tab.get("label") or ""
    workspaces = snapshot.get("workspaces")
    ws_by_id = {}
    if isinstance(workspaces, list):
        for ws in workspaces:
            if isinstance(ws, dict) and ws.get("workspace_id"):
                ws_by_id[ws["workspace_id"]] = ws.get("label") or ""
    return tabs_by_id, ws_by_id


def wire_agent(a, tabs_by_id, ws_by_id):
    """Normalise one herdr agent record into the device wire shape."""
    return {
        "id": sanitize(a.get("pane_id") or "", 24),
        "kind": sanitize(a.get("agent") or "", 12),
        "label": sanitize(
            tabs_by_id.get(a.get("tab_id"))
            or ws_by_id.get(a.get("workspace_id"))
            or a.get("terminal_title_stripped")
            or a.get("pane_id", ""),
            40,
        ),
        "status": a.get("agent_status") or "unknown",
        "focus": bool(a.get("focused", False)),
    }


def normalize_wire_agent(a):
    """Normalise an agent record that is already in wire shape (fixtures)."""
    return {
        "id": sanitize(a.get("id") or "", 24),
        "kind": sanitize(a.get("kind") or "", 12),
        "label": sanitize(a.get("label") or "", 40),
        "status": a.get("status") or "unknown",
        "focus": bool(a.get("focus", False)),
    }


def sort_agents(agents):
    """Most important first: BLOCKED, WORKING, DONE, IDLE, then unknown; ties by id."""
    return sorted(agents, key=lambda a: (STATE_RANK.get(a["status"], 4), a["id"]))


def build_state(socket_path):
    """Poll herdr once and return ``{"agents": [...]}``, or None if the snapshot is unusable."""
    resp = herdr_call("session.snapshot", {}, socket_path)
    result = resp.get("result")
    if not isinstance(result, dict):
        return None
    snapshot = result.get("snapshot")
    if not isinstance(snapshot, dict):
        return None
    raw_agents = snapshot.get("agents")
    if not isinstance(raw_agents, list):
        return None

    tabs_by_id, ws_by_id = _label_maps(snapshot)
    agents = [wire_agent(a, tabs_by_id, ws_by_id) for a in raw_agents if isinstance(a, dict)]
    return {"agents": sort_agents(agents)}


def load_fixture(path):
    """Read a fixture file once and return its agents in wire shape."""
    try:
        raw = Path(path).read_text(encoding="utf-8")
    except OSError as exc:
        raise BridgeError("fixture %s: %s" % (path, exc))
    try:
        doc = json.loads(raw)
    except ValueError as exc:
        raise BridgeError("fixture %s: invalid JSON: %s" % (path, exc))
    if not isinstance(doc, dict) or not isinstance(doc.get("agents"), list):
        raise BridgeError('fixture %s: expected {"agents": [...]}' % path)
    agents = [normalize_wire_agent(a) for a in doc["agents"] if isinstance(a, dict)]
    return sort_agents(agents)


# --- shared state -----------------------------------------------------------


class StateStore:
    """The one piece of mutable state: latest agents, generation counter, last success."""

    def __init__(self, poll_period=0.5):
        self._lock = threading.Lock()
        self._agents = []
        self._gen = 0
        self._ok_at = None
        self._fixture = None
        self._last_warn = 0.0
        self.poll_period = poll_period

    # -- poll thread side --

    def publish(self, agents):
        """Record a successful poll."""
        with self._lock:
            self._agents = agents
            self._gen += 1
            self._ok_at = time.monotonic()

    def note_failure(self, message):
        """Keep the previous agents and warn about the failure (at most one per 30 s)."""
        now = time.monotonic()
        with self._lock:
            if now - self._last_warn < FAIL_WARN_PERIOD_S:
                return
            self._last_warn = now
        print("WARNING: herdr poll failed: %s" % message, file=sys.stderr, flush=True)

    # -- fixture mode --

    def set_fixture(self, agents):
        """Serve a fixed agent list; ok_at stays fresh so the wire is never stale."""
        with self._lock:
            self._fixture = agents
            self._agents = agents
            self._ok_at = None

    # -- HTTP side --

    def snapshot(self):
        """Short locked read: in fixture mode bumping gen per /state request."""
        with self._lock:
            if self._fixture is not None:
                self._gen += 1
                stale = False
            else:
                stale = self._ok_at is None or (time.monotonic() - self._ok_at > STALE_AFTER_S)
            return {"gen": self._gen, "stale": stale, "agents": list(self._agents)}

    def agent_count(self):
        with self._lock:
            return len(self._agents)


def state_body(snap):
    """The exact /state body: version, generation, staleness, agents."""
    return {"v": 1, "gen": snap["gen"], "stale": snap["stale"], "agents": snap["agents"]}


def poll_loop(store, socket_path, interval):
    """Daemon loop: poll herdr, publish on success, rate-limited warning on failure."""
    while True:
        try:
            state = build_state(socket_path)
            if state is None:
                store.note_failure("session.snapshot returned no agents list")
            else:
                store.publish(state["agents"])
        except (BridgeError, OSError) as exc:
            store.note_failure(str(exc))
        time.sleep(interval)


# --- HTTP layer -------------------------------------------------------------


class BridgeHandler(BaseHTTPRequestHandler):
    server_version = "herdr-status-bridge/1"
    protocol_version = "HTTP/1.1"

    def log_message(self, fmt, *args):  # noqa: A003 - silence the default access log
        """Replaced by the single line written from do_GET."""

    def _respond(self, status, content_type, body):
        self.send_response(status)
        self.send_header("Content-Type", content_type)
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Cache-Control", "no-store")
        self.end_headers()
        self.wfile.write(body)

    def do_GET(self):  # noqa: N802 - BaseHTTPRequestHandler API
        path = self.path.split("?", 1)[0]
        if path == "/state":
            snap = self.server.store.snapshot()
            body = json.dumps(state_body(snap)).encode("utf-8")
            status = 200
            self._respond(status, "application/json", body)
            count = len(snap["agents"])
        elif path == "/healthz":
            status = 200
            self._respond(status, "text/plain", b"ok\n")
            count = self.server.store.agent_count()
        else:
            status = 404
            self._respond(status, "text/plain", b"not found\n")
            count = self.server.store.agent_count()

        print(
            "%s %s -> %d (%d agents)" % (self.command, self.path, status, count),
            file=sys.stderr,
            flush=True,
        )


class BridgeServer(ThreadingHTTPServer):
    daemon_threads = True
    allow_reuse_address = True

    def __init__(self, address, store):
        self.store = store
        super().__init__(address, BridgeHandler)


# --- entry points -----------------------------------------------------------


def parse_args(argv):
    parser = argparse.ArgumentParser(description="herdr status bridge for the ESP32 desk companion")
    parser.add_argument("--bind", default="0.0.0.0", help="HTTP bind address (default 0.0.0.0)")
    parser.add_argument("--port", type=int, default=8787, help="HTTP port (default 8787)")
    parser.add_argument("--socket", default=None, help="herdr Unix socket path")
    parser.add_argument("--interval", type=float, default=0.5, help="herdr poll period in seconds (default 0.5)")
    parser.add_argument("--fixture", default=None, help="serve agents from this JSON file instead of polling herdr")
    parser.add_argument("--once", action="store_true", help="print one /state body and exit")
    return parser.parse_args(argv)


def run_once(args):
    """Print one /state body to stdout; exit 0 on success, 1 when herdr is unreachable."""
    socket_path = resolve_socket_path(args.socket)
    store = StateStore()
    if args.fixture:
        try:
            store.set_fixture(load_fixture(args.fixture))
        except BridgeError as exc:
            print("ERROR: %s" % exc, file=sys.stderr)
            return 1
    else:
        try:
            state = build_state(socket_path)
        except (BridgeError, OSError) as exc:
            print("ERROR: %s" % exc, file=sys.stderr)
            return 1
        if state is None:
            print("ERROR: herdr %s returned no usable snapshot" % socket_path, file=sys.stderr)
            return 1
        store.publish(state["agents"])

    print(json.dumps(state_body(store.snapshot())))
    return 0


def main(argv=None):
    args = parse_args(argv)
    if args.once:
        return run_once(args)

    socket_path = resolve_socket_path(args.socket)
    store = StateStore(poll_period=args.interval)

    if args.fixture:
        try:
            store.set_fixture(load_fixture(args.fixture))
        except BridgeError as exc:
            print("ERROR: %s" % exc, file=sys.stderr)
            return 1
        source = "fixture=%s" % args.fixture
    else:
        threading.Thread(
            target=poll_loop,
            args=(store, socket_path, args.interval),
            name="herdr-poll",
            daemon=True,
        ).start()
        source = "poll=%.2fs" % args.interval

    print(
        "herdr-status-bridge: socket=%s http=%s:%d %s"
        % (socket_path, args.bind, args.port, source),
        file=sys.stderr,
        flush=True,
    )

    server = BridgeServer((args.bind, args.port), store)
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        pass
    finally:
        server.server_close()
    return 0


if __name__ == "__main__":
    sys.exit(main())