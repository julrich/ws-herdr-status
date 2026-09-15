#!/usr/bin/env python3
"""herdr status bridge.

Reads the live agent status from the herdr daemon over its Unix socket and serves a
tiny JSON document (``GET /state``) over HTTP on the LAN so the Waveshare
ESP32-C6-Touch-LCD-1.47 companion can poll it without a USB link. A second document,
``GET /stats``, carries per-session token counts summed from the agent harness's own
session logs (see the "omp session logs" section below).

Python 3 standard library only -- no third-party imports.

The herdr socket speaks newline-delimited JSON: one request line in, one reply line
out. The server closes the connection after the first response, so this module opens a
NEW connection per herdr request and never reuses one (a second request on a reused
connection fails with BrokenPipeError). Connect-per-call measures at ~0.6 ms, so this
costs nothing at the 0.5 s poll cadence.

Usage:
    python3 bridge/herdr_status_bridge.py
    python3 bridge/herdr_status_bridge.py --once
    python3 bridge/herdr_status_bridge.py --once --stats
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

#: Period of the thread that rebuilds the /stats document. It only reads the bytes
#: appended to the session logs since the previous pass, so this is ~1 ms per agent
#: and never on the request path.
STATS_REFRESH_S = 1.0

#: Longest model string put on the wire: the device's buffer is 16 bytes with the NUL
#: (HERDR_MODEL_LEN in main/herdr_status_types.h).
STATS_MODEL_LEN = 15


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
    agents = []
    paths = {}
    for a in raw_agents:
        if not isinstance(a, dict):
            continue
        agents.append(wire_agent(a, tabs_by_id, ws_by_id))
        # herdr is the only thing that knows which session log belongs to which pane:
        # agent_session is {"kind": "path", "value": "/…/<session>.jsonl"}. Everything
        # the stats document says about an agent is derived from that one file.
        session = a.get("agent_session")
        if isinstance(session, dict) and session.get("kind") == "path":
            value = session.get("value")
            if isinstance(value, str) and value:
                paths[agents[-1]["id"]] = value

    agents = sort_agents(agents)
    # Keyed and ordered like the wire rows, so /stats rows line up with /state rows and
    # the device can match them by id. An agent with no session path simply has no row.
    sessions = {a["id"]: paths[a["id"]] for a in agents if a["id"] in paths}
    return {"agents": agents, "sessions": sessions}


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


# --- omp session logs, and the /stats document ------------------------------

# The agent harness (omp) appends one newline-delimited JSON record per event to
# ~/.omp/agent/sessions/<slug>/<ts>_<uuid>.jsonl -- 45 files / 58 MB on the machine
# this was written for, the largest 11 MB. Reading one whole log takes ~120 ms and all
# of them ~440 ms, so a request must never do it: a thread folds in the appended bytes
# every STATS_REFRESH_S and the handler serves the finished document.
#
# omp's own ~/.omp/stats.db is deliberately NOT used: it is pre-aggregated, but only
# as fresh as the last `omp stats` run (29 h stale when this was written), so it is
# useless as a live source.


def _token_count(v):
    """Usage counters are integers, but never trust a file this process does not own."""
    if isinstance(v, bool) or not isinstance(v, (int, float)):
        return 0
    return int(v)


class SessionTail:
    """Incremental parser state for one omp session log.

    A live session only appends (a few KB in bursts), so the offset is the whole trick:
    only the bytes written since the previous pass are read and folded in. Re-reading
    instead would cost 33 ms for the 4.5 MB log of a busy session and 442 ms for all 45
    logs on this machine, against 0.03 ms for a pass with nothing to read and 3.7 ms for
    one that reads back a 60 KB burst. Records are newline-delimited and can be tens of
    KB, so the last line of a read is often a record still being written; it is kept in
    ``partial`` and prepended to the next read instead of being parsed (and the read
    offset still advances past it, or it would be read twice).
    """

    def __init__(self, path):
        self.path = path
        self.offset = 0
        self.partial = b""
        self.tokens_in = 0
        self.tokens_out = 0
        self.calls = 0
        self.messages = 0
        self.model = ""
        self.mtime = 0.0

    def refresh(self):
        """Fold the appended bytes in. Raises OSError when the file cannot be read."""
        st = os.stat(self.path)
        if st.st_size < self.offset:
            # Truncated, rotated or replaced under us: start over rather than resume in
            # the middle of a record. The totals stay correct because they are rebuilt
            # from the first byte.
            self.offset = 0
            self.partial = b""
            self.tokens_in = 0
            self.tokens_out = 0
            self.calls = 0
            self.messages = 0
            self.model = ""
        if st.st_size > self.offset:
            with open(self.path, "rb") as fh:
                fh.seek(self.offset)
                data = fh.read()
            # Advance by what was actually read, not by st.st_size: a session writing
            # while we read gives us more bytes than the stat promised, and counting
            # those twice would double whatever the last record carried.
            self.offset += len(data)
            lines = (self.partial + data).split(b"\n")
            self.partial = lines.pop()
            self._fold(lines)
        self.mtime = st.st_mtime

    def _fold(self, lines):
        """Fold complete records into the running totals.

        Only these fields are read, nothing else is kept, and the parsed record is
        dropped as soon as it has been looked at: the record type, the tool-execution
        marker, and message.{model,usage.{input,output,cacheRead,cacheWrite}}. The
        records also carry the whole conversation, and none of it leaves this process.
        """
        for line in lines:
            try:
                rec = json.loads(line)
            except ValueError:
                continue  # an unreadable record is skipped, never fatal
            if not isinstance(rec, dict):
                continue
            kind = rec.get("type")
            if kind == "custom":
                if rec.get("customType") == "tool_execution_start":
                    self.calls += 1  # one tool invocation
                continue
            if kind != "message":
                continue
            message = rec.get("message")
            if not isinstance(message, dict):
                continue
            usage = message.get("usage")
            if not isinstance(usage, dict):
                continue  # user and tool-result messages carry model output, not usage
            self.messages += 1
            # Cache reads and writes are prompt tokens too, they are just billed as
            # cache: omp's own totalTokens is exactly these four numbers added up, so
            # counting only `input` would report a long session as nearly free.
            self.tokens_in += (
                _token_count(usage.get("input"))
                + _token_count(usage.get("cacheRead"))
                + _token_count(usage.get("cacheWrite"))
            )
            self.tokens_out += _token_count(usage.get("output"))
            model = message.get("model")
            if isinstance(model, str) and model:
                self.model = model  # last one wins: the model the session is on now


class StatsStore:
    """The /stats document, rebuilt by its own thread and served as a locked copy.

    ``_tails`` is only ever touched by that one thread, so it needs no lock of its
    own; the lock covers the published document, which is what the HTTP threads read.
    No file I/O ever happens while it is held.
    """

    def __init__(self):
        self._lock = threading.Lock()
        self._tails = {}
        self._gen = 0
        self._doc = {
            "gen": 0,
            "stale": True,
            "sessions": 0,
            "totals": {"in": 0, "out": 0, "calls": 0, "messages": 0, "age_s": 0},
            "agents": [],
        }

    def refresh(self, sessions, stale):
        """Rebuild the document from the session files behind ``sessions``.

        ``sessions`` maps pane id to log path, in the order /state lists its agents.
        """
        # Wall clock, like st_mtime: time.monotonic() has an arbitrary origin and would
        # make every age come out as "just now".
        now = time.time()
        rows = []
        for pane_id, path in sessions.items():
            tail = self._tails.get(path)
            if tail is None:
                tail = self._tails[path] = SessionTail(path)
            try:
                tail.refresh()
            except OSError:
                # Missing, renamed or unreadable right now. The device draws the row
                # with zeros, so drop it; forgetting the state means a file that comes
                # back is read from its first byte instead of from a stale offset.
                del self._tails[path]
                continue
            rows.append(
                {
                    "id": pane_id,
                    "in": tail.tokens_in,
                    "out": tail.tokens_out,
                    "calls": tail.calls,
                    "messages": tail.messages,
                    # The mtime of the log IS the session's last activity: it is
                    # appended to on every event, and left alone once the agent stops.
                    "age_s": max(0, int(now - tail.mtime)),
                    # "provider/model" is too long for the device's 16-byte buffer;
                    # the provider half is the same for every model it will ever see.
                    "model": sanitize(tail.model.rsplit("/", 1)[-1], STATS_MODEL_LEN),
                }
            )

        # Sessions end and herdr starts new ones under new paths; without this the table
        # would grow for as long as the bridge runs. An agent herdr still reports keeps
        # its state, so a poll that fails while herdr is down never resets the counters.
        live = set(sessions.values())
        for path in [p for p in self._tails if p not in live]:
            del self._tails[path]

        totals = {"in": 0, "out": 0, "calls": 0, "messages": 0, "age_s": 0}
        for row in rows:
            for key in ("in", "out", "calls", "messages"):
                totals[key] += row[key]
        if rows:
            totals["age_s"] = min(row["age_s"] for row in rows)  # youngest session

        with self._lock:
            self._gen += 1
            # A fresh dict per pass, never a mutation of the published one: a handler
            # that is mid-dump can then never see half a document.
            self._doc = {
                "gen": self._gen,
                "stale": stale,
                "sessions": len(rows),
                "totals": totals,
                "agents": rows,
            }

    def snapshot(self):
        """Short locked read: the document is already built, so this only copies."""
        with self._lock:
            return self._doc.copy()


def stats_body(snap):
    """The exact /stats body: version, generation, staleness, sessions, totals, rows."""
    return {
        "v": 1,
        "gen": snap["gen"],
        "stale": snap["stale"],
        "sessions": snap["sessions"],
        "totals": snap["totals"],
        "agents": snap["agents"],
    }


def stats_loop(stats, store, interval):
    """Daemon loop: keep the /stats document in step with the session logs.

    The pane -> log map comes from the last herdr poll, so this thread never talks to
    herdr itself; it only ever reads bytes that were appended since the last pass.
    """
    while True:
        sessions, stale = store.stats_inputs()
        stats.refresh(sessions, stale)
        time.sleep(interval)


# --- shared state -----------------------------------------------------------


class StateStore:
    """The one piece of mutable state: latest agents, generation counter, last success."""

    def __init__(self, poll_period=0.5):
        self._lock = threading.Lock()
        self._agents = []
        self._sessions = {}
        self._gen = 0
        self._ok_at = None
        self._fixture = None
        self._last_warn = 0.0
        self.poll_period = poll_period

    # -- poll thread side --

    def publish(self, agents, sessions):
        """Record a successful poll."""
        with self._lock:
            self._agents = agents
            self._sessions = sessions
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
        """Serve a fixed agent list; ok_at stays fresh so the wire is never stale.

        A fixture only carries the /state fields, so /stats has no session to describe
        and reports zero sessions while the bridge runs in this mode.
        """
        with self._lock:
            self._fixture = agents
            self._agents = agents
            self._sessions = {}
            self._ok_at = None

    # -- HTTP side --

    def _is_stale(self):
        """True when no successful poll is recent enough (the caller holds the lock)."""
        return self._ok_at is None or (time.monotonic() - self._ok_at > STALE_AFTER_S)

    def snapshot(self):
        """Short locked read: in fixture mode bumping gen per /state request."""
        with self._lock:
            if self._fixture is not None:
                self._gen += 1
                stale = False
            else:
                stale = self._is_stale()
            return {"gen": self._gen, "stale": stale, "agents": list(self._agents)}

    def stats_inputs(self):
        """Locked read for the stats thread: the pane -> session log map, and staleness.

        Staleness mirrors /state on purpose: both documents are built from the same
        poll, and herdr is the only source for which log belongs to which pane, so an
        unreachable herdr makes the stats stale too even though the logs still read.
        """
        with self._lock:
            if self._fixture is not None:
                return {}, False
            return dict(self._sessions), self._is_stale()

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
                store.publish(state["agents"], state["sessions"])
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
        elif path == "/stats":
            snap = self.server.stats.snapshot()
            body = json.dumps(stats_body(snap)).encode("utf-8")
            status = 200
            self._respond(status, "application/json", body)
            count = snap["sessions"]
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

    def __init__(self, address, store, stats):
        self.store = store
        self.stats = stats
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
    parser.add_argument(
        "--stats",
        action="store_true",
        help="with --once, print the /stats body instead of the /state body",
    )
    return parser.parse_args(argv)


def run_once(args):
    """Print one /state body -- or, with --stats, one /stats body -- and exit.

    Exits 0 on success, 1 when herdr is unreachable.
    """
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
        store.publish(state["agents"], state["sessions"])

    if args.stats:
        # One pass, right here: the first one reads every session log in full (~40 ms
        # for the live ones), which is exactly why the server does this in a thread.
        stats = StatsStore()
        sessions, stale = store.stats_inputs()
        stats.refresh(sessions, stale)
        print(json.dumps(stats_body(stats.snapshot())))
    else:
        print(json.dumps(state_body(store.snapshot())))
    return 0


def main(argv=None):
    args = parse_args(argv)
    if args.once:
        return run_once(args)

    socket_path = resolve_socket_path(args.socket)
    store = StateStore(poll_period=args.interval)
    stats = StatsStore()

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

    # Always running: /stats is an endpoint of its own, and a pass that finds nothing
    # appended still costs a stat() per session log.
    threading.Thread(
        target=stats_loop,
        args=(stats, store, STATS_REFRESH_S),
        name="herdr-stats",
        daemon=True,
    ).start()

    print(
        "herdr-status-bridge: socket=%s http=%s:%d %s"
        % (socket_path, args.bind, args.port, source),
        file=sys.stderr,
        flush=True,
    )

    server = BridgeServer((args.bind, args.port), store, stats)
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        pass
    finally:
        server.server_close()
    return 0


if __name__ == "__main__":
    sys.exit(main())