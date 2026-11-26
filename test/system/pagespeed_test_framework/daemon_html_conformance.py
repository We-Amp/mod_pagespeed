# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2026 We-Amp B.V.

"""Live runner core for the HTML cache conformance cases.

test/system/daemon_html/conformance_cases.json holds the `live` rows: what a
server port sends for a sequence of requests against a configured origin.
This module loads them, drives their steps and checks every expectation,
including exact header line counts. It knows nothing about any server: a
lane implements `Provider` (configure a virtual host and origin for a row,
send a request, read the volume, read the counters, run the row's purge and
origin operations) and hands it to `run_row`.

The only skip is a row whose `ports` list leaves the provider's port out
(`skip_reason`). Anything else a provider cannot do is a failure, never a
skip: `ProviderUnsupported` raised from a provider becomes a failure of the
row. The Apache lane is the reference lane and allows no skip at all: the
rows for other ports are not collected there (`lane_rows`), and a lane that
skips a row it should run fails the run (`skip_violations`).

Retries: a provider may retry what is transport or setup (a reload that has
not taken effect yet, a statistics read a closing worker dropped), never a
request a row asserts on. A row that waits (`wait_markdown`, a `window`)
waits for a stated deadline and fails at it.
"""

import abc
import http.client
import importlib
import json
import os
import threading
import time
from dataclasses import dataclass, field
from typing import Callable, Dict, Iterable, List, Optional, Set, Tuple

HeaderLines = List[Tuple[str, str]]

CASES_PATH = os.path.join(
    os.path.dirname(os.path.dirname(os.path.abspath(__file__))),
    "daemon_html",
    "conformance_cases.json",
)

PORTS = ("apache", "nginx", "iis")

# The ports whose lane ships a provider, as the module
# pagespeed_test_framework.daemon_html_provider_<port> with
# make_provider(server_config). A lane outside this set skips every row; a
# lane in it fails when its provider cannot be imported. The change that
# adds a lane's provider adds its port here.
PROVIDER_PORTS = frozenset(["apache", "nginx"])

# Expect keys a background step may carry: what its own response shows. Its
# expectations are checked after the next step ran, so volume, counter and
# origin state could belong to that later step.
BACKGROUND_EXPECT_KEYS = frozenset(
    [
        "status",
        "content_type",
        "headers_exact",
        "headers_only",
        "headers_absent",
        "body_ref",
        "body_bytes",
        "length_matches_body",
    ]
)

# The server core's transport lines, allowed beside headers_exact under
# headers_only.
TRANSPORT_LINES = frozenset(
    ["date", "server", "connection", "keep-alive", "transfer-encoding"]
)

# Lines a differential comparison leaves out: each response has its own.
VOLATILE_LINES = frozenset(["date"])

LIVE_KEYS = frozenset(["mode", "agent_optimize", "worker", "vhost", "origin", "steps"])
VHOST_KNOBS = frozenset(
    [
        "header_add",
        "header_always",
        "header_early",
        "expires",
        "auth",
        "alias",
        "default_vhost",
        "in_place",
        "pagespeed_off_location",
        "compress_markdown",
        "third_party_filter",
        "merge_slashes",
        "poll_interval_s",
        "chained_module_origin",
        "named_location",
        "proxy_hide",
        "trailer",
    ]
)
ORIGIN_KEYS = frozenset(
    ["status", "headers", "body_ref", "echo_conditionals", "truncate_body"]
)
STEP_KEYS = frozenset(["request", "pre", "process", "background", "expect", "window"])
# A window step (see RowRunner._run_window): after its purge, the same
# request is sent to one named process every `every_s` seconds for
# `observe_s` seconds. A response matching `stale` is the pre-purge entry; it
# may appear only while the request is sent within `stale_max_s` (the poll
# interval of the row's server) plus `margin_s` of the purge, and never after
# a response that did not match it. Every other response must meet `expect`.
WINDOW_KEYS = frozenset(["stale_max_s", "margin_s", "observe_s", "every_s", "stale"])
WINDOW_STALE_KEYS = frozenset(["content_type"])
PURGE_OPS = frozenset(["purge_url", "purge_global"])
REQUEST_KEYS = frozenset(["method", "path", "host", "headers"])
OPS = frozenset(
    [
        "purge_url",
        "purge_global",
        "wait_markdown",
        "sleep_s",
        "origin_set",
        "hold_origin",
        "release_origin",
        "set_mode",
    ]
)
EXPECT_KEYS = frozenset(
    [
        "status",
        "content_type",
        "headers_exact",
        "headers_only",
        "headers_absent",
        "stored",
        "stored_lifetime_s",
        "sidecar_absent",
        "body_ref",
        "body_bytes",
        "length_matches_body",
        "origin_saw",
        "origin_saw_absent",
        "counter_delta",
        "differential",
    ]
)
HEADER_EXPECT_KEYS = frozenset(["count", "value", "contains"])
MODES = frozenset(["off", "report-only", "serve-variants"])

MARKDOWN_ETAG = "$markdown_etag"


@dataclass
class Response:
    """One response as the client received it."""

    status: int
    # Every header line, in order, as received.
    headers: HeaderLines
    body: bytes = b""

    def lines(self, name: str) -> List[str]:
        return [v for n, v in self.headers if n.lower() == name.lower()]


@dataclass
class StoredEntry:
    """The volume's original entry for a URL, read by exact id."""

    lifetime_s: int
    # The stored header block's lines; None when the entry has none.
    sidecar: Optional[HeaderLines] = None


class ProviderUnsupported(Exception):
    """A provider cannot do what a row asks. The row fails; it is never
    skipped."""


class Provider(abc.ABC):
    """One server lane, as the conformance runner drives it.

    A provider is set up once per row (`setup`) and torn down after it
    (`teardown`), whatever the outcome. Requests name a path and, optionally,
    a host; the provider sends them to the row's virtual host on its own
    port. Every method may raise ProviderUnsupported.

    The runner calls `settle` before every read of the volume, the counters
    or the origin's request log, and before the counter snapshot a step
    starts with.
    """

    #: The port this provider serves: one of PORTS.
    port: str = ""

    @abc.abstractmethod
    def setup(self, row_id: str, live: dict, bodies: Dict[str, str]) -> Iterable[str]:
        """Configure the row's mode, agent_optimize, worker state, vhost
        knobs and origin (status, headers, body from `bodies`, whether it
        echoes the conditionals it receives), on a volume holding nothing
        for the row's URLs.

        Returns the names of everything it applied: "mode",
        "agent_optimize", "worker", each vhost knob by its name and each
        origin key as "origin.<key>". The row fails when a key it uses is
        not in that set (`required_keys`), so a knob a provider does not
        implement can never turn a row into a plainer one that passes."""

    @abc.abstractmethod
    def settle(self) -> None:
        """Return only once every commit, notification and purge removal the
        server started for requests already answered has finished. The
        provider proves it with a positive signal (for example the server's
        in-flight commit gauge reaching zero, or a commit counter reaching
        the number of responses it recorded), never with a fixed sleep. On
        every port the commit runs after the client has the response, so a
        read without this can see a page that will be stored as absent."""

    @abc.abstractmethod
    def teardown(self) -> None:
        """Undo setup: release a held origin, restore the configuration."""

    @abc.abstractmethod
    def request(
        self,
        method: str,
        path: str,
        host: Optional[str],
        headers: HeaderLines,
        process: Optional[str] = None,
    ) -> Response:
        """Send one request and return what the client received.

        `path` goes on the request line byte for byte: no client
        normalisation of `//`, `%2F`, dot segments or anything else. The
        returned headers are one (name, value) pair per line as received,
        never merged: two Cache-Control lines are two pairs (many clients
        fold repeated fields into one; `send_raw` does not). `headers` are
        sent as given, one line each.

        `process` names the server process to send it to; None is any. Only
        the cross-process rows set it. A provider that cannot pin a process
        raises ProviderUnsupported, which fails those rows on that lane."""

    @abc.abstractmethod
    def stored_entry(self, path: str, host: Optional[str]) -> Optional[StoredEntry]:
        """The volume's original entry for the request URL, or None."""

    @abc.abstractmethod
    def counters(self) -> Dict[str, int]:
        """The module's daemon_* counters, summed over the server's
        processes."""

    @abc.abstractmethod
    def origin_requests(self) -> List[HeaderLines]:
        """The header lines of every request the origin received since
        setup, in order."""

    @abc.abstractmethod
    def markdown_ready(self, path: str, host: Optional[str]) -> bool:
        """Whether the URL's Markdown copy is servable now. A probe that
        changes nothing the row asserts (no counter a row reads, no
        store)."""

    @abc.abstractmethod
    def purge_url(self, url: str, form: str) -> None:
        """Purge one URL: form PURGE (the method) or query (?purge=)."""

    @abc.abstractmethod
    def purge_global(self, via: str) -> None:
        """Purge everything: via admin or flush_file."""

    @abc.abstractmethod
    def set_origin_body(self, body: str) -> None:
        """Serve a different body from the origin from now on."""

    @abc.abstractmethod
    def hold_origin(self) -> None:
        """Make the origin hold its next response body until released."""

    @abc.abstractmethod
    def release_origin(self) -> None:
        """Let a held origin response go."""

    @abc.abstractmethod
    def set_mode(self, mode: str) -> None:
        """Switch the row's virtual host to another DaemonHtmlCache mode, the
        other knobs unchanged."""


def load_cases(path: str = CASES_PATH) -> dict:
    with open(path, encoding="utf-8") as f:
        return json.load(f)


def send_raw(
    address: str,
    port: int,
    method: str,
    path: str,
    host: str,
    headers: HeaderLines,
    timeout_s: float = 60,
) -> Response:
    """One HTTP/1.1 request as a provider's transport: the path verbatim,
    every header line sent as given (Host included, nothing added but
    Content-Length for no body), every received line kept as its own pair."""
    conn = http.client.HTTPConnection(address, port, timeout=timeout_s)
    try:
        conn.putrequest(method, path, skip_host=True, skip_accept_encoding=True)
        conn.putheader("Host", host)
        for name, value in headers:
            conn.putheader(name, value)
        conn.endheaders()
        r = conn.getresponse()
        body = r.read()
        return Response(r.status, list(r.getheaders()), body)
    finally:
        conn.close()


def load_provider(port: str, server_config) -> Optional["Provider"]:
    """The lane's provider, or None when its port is not in PROVIDER_PORTS.
    A listed port whose module is missing raises."""
    if port not in PROVIDER_PORTS:
        return None
    module = importlib.import_module(
        f"pagespeed_test_framework.daemon_html_provider_{port}"
    )
    provider = module.make_provider(server_config)
    if provider.port != port:
        raise AssertionError(f"the {port} provider reports port {provider.port}")
    return provider


def required_keys(live: dict) -> Set[str]:
    """What a provider must report applied for this row (see setup)."""
    out = {k for k in ("mode", "agent_optimize", "worker") if k in live}
    out |= set(live.get("vhost", {}))
    out |= {f"origin.{k}" for k in live.get("origin", {})}
    return out


def live_rows(cases: dict) -> List[dict]:
    return [r for r in cases["rows"] if "live" in r.get("runners", [])]


def skip_reason(row: dict, port: str) -> Optional[str]:
    """The one allowed skip: the row's ports leave this port out."""
    ports = row.get("ports")
    if ports is not None and port not in ports:
        return f"row {row['id']} is for {', '.join(ports)}, not {port}"
    return None


def lane_port(server_type: str) -> Optional[str]:
    """The port a lane's PAGESPEED_SERVER_TYPE names, or None for a server
    with no HTML cache port (envoy). "auto" is the Apache lane."""
    if server_type in ("apache", "auto", ""):
        return "apache"
    if server_type in ("nginx", "iis"):
        return server_type
    return None


# The lanes that allow no skip in the table: the reference lane runs every
# row whose ports include it, and the rows for other ports are left out of
# its collection (pytest reports them deselected), so its skip count is 0.
ZERO_SKIP_PORTS = frozenset(["apache"])


def lane_rows(rows: List[dict], port: Optional[str]) -> Tuple[List[dict], List[dict]]:
    """(rows the lane collects, rows it leaves out). Only a zero-skip lane
    leaves rows out: those whose ports exclude it. Every other lane collects
    the whole table and skips by skip_reason."""
    if port not in ZERO_SKIP_PORTS:
        return list(rows), []
    kept = [r for r in rows if skip_reason(r, port) is None]
    left = [r for r in rows if skip_reason(r, port) is not None]
    return kept, left


def skip_violations(port: str, skipped: Dict[str, str], rows_by_id: Dict[str, dict]) -> List[str]:
    """The skips a lane may not report, given {row id: skip reason}. On a
    zero-skip lane every skip is one. On another lane a skip is allowed only
    for a row whose ports exclude the lane, whatever reason it was given."""
    out: List[str] = []
    for rid, reason in sorted(skipped.items()):
        row = rows_by_id.get(rid)
        if port in ZERO_SKIP_PORTS:
            out.append(f"{rid}: skipped on the {port} lane, which allows no skip ({reason})")
        elif row is None or skip_reason(row, port) is None:
            out.append(f"{rid}: skipped on the {port} lane, but its ports include {port} ({reason})")
    return out


def validate_row(row: dict, bodies: Dict[str, str]) -> List[str]:
    """Every key the row uses is one this runner acts on. A key it does not
    know would otherwise be silently ignored."""
    errors: List[str] = []
    rid = row.get("id", "?")

    def unknown(where: str, obj: dict, allowed) -> None:
        if not isinstance(obj, dict):
            errors.append(f"{rid}: {where} is not an object")
            return
        for key in obj:
            if key not in allowed:
                errors.append(f"{rid}: unknown {where} key {key}")

    live = row.get("live")
    if live is None:
        return [f"{rid}: no live body"]
    unknown("live", live, LIVE_KEYS)
    if live.get("mode") not in MODES:
        errors.append(f"{rid}: mode {live.get('mode')}")
    for port in row.get("ports", []):
        if port not in PORTS:
            errors.append(f"{rid}: port {port}")
    unknown("vhost", live.get("vhost", {}), VHOST_KNOBS)
    unknown("origin", live.get("origin", {}), ORIGIN_KEYS)
    if live.get("origin", {}).get("body_ref") not in bodies:
        errors.append(f"{rid}: origin body_ref")
    steps = live.get("steps", [])
    if not steps:
        errors.append(f"{rid}: no steps")
    for i, step in enumerate(steps):
        where = f"step {i}"
        unknown(where, step, STEP_KEYS)
        unknown(f"{where} request", step.get("request", {}), REQUEST_KEYS)
        for op in step.get("pre", []):
            if not isinstance(op, dict) or len(op) != 1:
                errors.append(f"{rid}: {where} op is not a one-key object")
                continue
            name = next(iter(op))
            if name not in OPS:
                errors.append(f"{rid}: {where} unknown op {name}")
            if name == "origin_set" and op[name].get("body_ref") not in bodies:
                errors.append(f"{rid}: {where} origin_set body_ref")
            if name == "set_mode" and op[name].get("mode") not in MODES:
                errors.append(f"{rid}: {where} set_mode mode")
        expect = step.get("expect", {})
        unknown(f"{where} expect", expect, EXPECT_KEYS)
        if step.get("background"):
            for key in expect:
                if key in EXPECT_KEYS and key not in BACKGROUND_EXPECT_KEYS:
                    errors.append(
                        f"{rid}: {where} is a background step and cannot assert {key}"
                    )
        if not expect:
            errors.append(f"{rid}: {where} expects nothing")
        if "window" in step:
            errors += _validate_window(rid, where, step)
        for name, spec in expect.get("headers_exact", {}).items():
            unknown(f"{where} headers_exact {name}", spec, HEADER_EXPECT_KEYS)
            if "count" not in spec:
                errors.append(f"{rid}: {where} headers_exact {name} has no count")
        if "body_ref" in expect and expect["body_ref"] not in bodies:
            errors.append(f"{rid}: {where} body_ref")
        diff = expect.get("differential")
        if diff is not None:
            unknown(f"{where} differential", diff, {"against", "allow_vary_accept"})
            for mode in diff.get("against", []):
                if mode not in MODES:
                    errors.append(f"{rid}: {where} differential mode {mode}")
    return errors


def _validate_window(rid: str, where: str, step: dict) -> List[str]:
    errors: List[str] = []
    window = step["window"]
    if not isinstance(window, dict):
        return [f"{rid}: {where} window is not an object"]
    for key in window:
        if key not in WINDOW_KEYS:
            errors.append(f"{rid}: unknown {where} window key {key}")
    for key in WINDOW_KEYS - {"stale"}:
        value = window.get(key)
        if not isinstance(value, (int, float)) or value <= 0:
            errors.append(f"{rid}: {where} window {key} must be a positive number")
    stale = window.get("stale")
    if not isinstance(stale, dict) or set(stale) != WINDOW_STALE_KEYS:
        errors.append(f"{rid}: {where} window stale must name content_type")
    if step.get("background"):
        errors.append(f"{rid}: {where} a window step cannot be a background step")
    if not step.get("process"):
        errors.append(f"{rid}: {where} a window step names the process it watches")
    if not any(next(iter(op), None) in PURGE_OPS for op in step.get("pre", [])
               if isinstance(op, dict)):
        errors.append(f"{rid}: {where} a window step starts at a purge in its pre ops")
    expect = step.get("expect", {})
    for key in expect:
        if key not in BACKGROUND_EXPECT_KEYS:
            errors.append(f"{rid}: {where} a window step cannot assert {key}")
    if "content_type" not in expect:
        errors.append(f"{rid}: {where} a window step expects a content_type")
    elif isinstance(stale, dict) and media_type(str(stale.get("content_type", ""))) == media_type(
        expect["content_type"]
    ):
        errors.append(f"{rid}: {where} window stale and expect name the same content type")
    try:
        if window["observe_s"] <= window["stale_max_s"] + window["margin_s"]:
            errors.append(
                f"{rid}: {where} window observe_s must outlast stale_max_s plus margin_s"
            )
    except (KeyError, TypeError):
        pass
    return errors


def media_type(value: str) -> str:
    return value.split(";", 1)[0].strip().lower()


def _vary_without_accept(headers: HeaderLines) -> Tuple[HeaderLines, bool]:
    """The header lines with one `Accept` token taken out of the Vary
    lines (a Vary line left empty is dropped), and whether one was."""
    out: HeaderLines = []
    removed = False
    for name, value in headers:
        if name.lower() == "vary" and not removed:
            tokens = [t.strip() for t in value.split(",") if t.strip()]
            kept = []
            for t in tokens:
                if not removed and t.lower() == "accept":
                    removed = True
                    continue
                kept.append(t)
            if not kept:
                continue
            value = ", ".join(kept)
        out.append((name, value))
    return out, removed


def _header_multiset(headers: HeaderLines) -> List[Tuple[str, str]]:
    return sorted(
        (n.lower(), v) for n, v in headers if n.lower() not in VOLATILE_LINES
    )


@dataclass
class _Background:
    index: int
    step: dict
    thread: threading.Thread
    result: dict = field(default_factory=dict)


class RowRunner:
    """Drives one live row against a provider and collects every failure."""

    def __init__(
        self,
        provider: Provider,
        row: dict,
        bodies: Dict[str, str],
        sleep: Callable[[float], None] = time.sleep,
        clock: Callable[[], float] = time.monotonic,
        poll_s: float = 0.25,
    ) -> None:
        self.provider = provider
        self.row = row
        self.live = row["live"]
        self.bodies = bodies
        self.sleep = sleep
        self.clock = clock
        self.poll_s = poll_s
        self.failures: List[str] = []
        self.markdown_etag: Optional[str] = None
        self.mode = self.live["mode"]
        # When the last purge op returned (the runner's clock).
        self.purged_at: Optional[float] = None
        # One record per window step run: what the watched process sent.
        self.windows: List[dict] = []

    # ---- entry

    def run(self) -> List[str]:
        errors = validate_row(self.row, self.bodies)
        if errors:
            return errors
        try:
            applied = self.provider.setup(self.row["id"], self.live, self.bodies)
        except ProviderUnsupported as e:
            return [f"{self.row['id']}: provider cannot set up the row: {e}"]
        pending: List[_Background] = []
        try:
            missing = sorted(required_keys(self.live) - set(applied or ()))
            if missing:
                self.failures.append(
                    f"{self.row['id']}: the provider did not apply {', '.join(missing)}"
                )
                return self.failures
            for index, step in enumerate(self.live["steps"]):
                try:
                    if step.get("background"):
                        pending.append(self._start_background(index, step))
                        continue
                    self._run_step(index, step)
                except ProviderUnsupported as e:
                    self._fail(index, f"provider cannot run the step: {e}")
                # A background step is checked once the step after it ran.
                for bg in pending:
                    self._join(bg)
                pending = []
        finally:
            for bg in pending:
                self._join(bg)
            self.provider.teardown()
        return self.failures

    # ---- steps

    def _fail(self, index: int, what: str) -> None:
        self.failures.append(f"{self.row['id']} step {index}: {what}")

    def _request_headers(self, request: dict) -> HeaderLines:
        out: HeaderLines = []
        for name, value in request.get("headers", []):
            if MARKDOWN_ETAG in value:
                if self.markdown_etag is None:
                    raise ProviderUnsupported(
                        "$markdown_etag before any Markdown 200 in the row"
                    )
                value = value.replace(MARKDOWN_ETAG, self.markdown_etag)
            out.append((name, value))
        return out

    def _send(self, step: dict) -> Response:
        request = step["request"]
        return self.provider.request(
            request["method"],
            request["path"],
            request.get("host"),
            self._request_headers(request),
            step.get("process"),
        )

    def _run_pre(self, index: int, step: dict) -> None:
        request = step["request"]
        for op in step.get("pre", []):
            name, arg = next(iter(op.items()))
            if name == "sleep_s":
                self.sleep(float(arg["s"]))
            elif name == "wait_markdown":
                self._wait_markdown(index, request, arg)
            elif name == "purge_url":
                self.provider.purge_url(arg["url"], arg.get("form", "PURGE"))
                self.purged_at = self.clock()
            elif name == "purge_global":
                self.provider.purge_global(arg["via"])
                self.purged_at = self.clock()
            elif name == "origin_set":
                self.provider.set_origin_body(self.bodies[arg["body_ref"]])
            elif name == "hold_origin":
                self.provider.hold_origin()
            elif name == "release_origin":
                self.provider.release_origin()
            elif name == "set_mode":
                self.mode = arg["mode"]
                self.provider.set_mode(arg["mode"])

    def _wait_markdown(self, index: int, request: dict, arg: dict) -> None:
        timeout = float(arg.get("timeout_s", 30))
        expect_ready = arg.get("expect_ready", True)
        deadline = self.clock() + timeout
        while True:
            self.provider.settle()
            if self.provider.markdown_ready(request["path"], request.get("host")):
                if not expect_ready:
                    self._fail(index, "the Markdown copy became servable")
                return
            if self.clock() >= deadline:
                if expect_ready:
                    self._fail(
                        index, f"the Markdown copy not servable within {timeout:g} s"
                    )
                return
            self.sleep(self.poll_s)

    def _run_step(self, index: int, step: dict) -> None:
        # The step's counter delta covers its pre ops too: a commit a pre op
        # lets go (release_origin) is counted in the step that released it.
        expect = step["expect"]
        if "window" in step:
            self._run_window(index, step)
            return
        self.provider.settle()
        counters_before = self.provider.counters() if "counter_delta" in expect else None
        self._run_pre(index, step)
        self.provider.settle()
        origin_before = len(self.provider.origin_requests())
        response = self._send(step)
        self._check(index, step, response, counters_before, origin_before)

    def _run_window(self, index: int, step: dict) -> None:
        """Watches one process across a purge: the pre-purge entry (a
        response matching `stale`) may be served only for a request sent
        within stale_max_s + margin_s of the purge, never again once a
        response did not match it, and every other response meets
        `expect`. It stops at the first failure: one wrong answer is the
        finding, the ones after it add nothing."""
        window = step["window"]
        stale_type = media_type(window["stale"]["content_type"])
        self.provider.settle()
        self.purged_at = None
        self._run_pre(index, step)
        start = self.purged_at
        bound = float(window["stale_max_s"]) + float(window["margin_s"])
        record = {"step": index, "stale": 0, "miss": 0, "last_stale_s": None,
                  "first_miss_s": None}
        self.windows.append(record)
        while True:
            sent = self.clock() - start
            r = self._send(step)
            stale = r.status == 200 and any(
                media_type(v) == stale_type for v in r.lines("Content-Type"))
            if stale:
                record["stale"] += 1
                record["last_stale_s"] = sent
                if record["first_miss_s"] is not None:
                    self._fail(index, (
                        f"the pre-purge entry was served for a request sent "
                        f"{sent:.2f} s after the purge, after a miss at "
                        f"{record['first_miss_s']:.2f} s"))
                    return
                if sent > bound:
                    self._fail(index, (
                        f"the pre-purge entry was still served for a request sent "
                        f"{sent:.2f} s after the purge (allowed: {window['stale_max_s']:g} s "
                        f"poll interval + {window['margin_s']:g} s margin)"))
                    return
            else:
                record["miss"] += 1
                if record["first_miss_s"] is None:
                    record["first_miss_s"] = sent
                before = len(self.failures)
                self._check(index, step, r, None, 0)
                if len(self.failures) > before:
                    return
            if self.clock() - start >= float(window["observe_s"]):
                return
            self.sleep(float(window["every_s"]))

    def _start_background(self, index: int, step: dict) -> _Background:
        self._run_pre(index, step)
        holder: dict = {}
        origin_before = 0  # A background step asserts no origin state.

        def go() -> None:
            try:
                holder["response"] = self._send(step)
            except Exception as e:  # reported when joined
                holder["error"] = e

        thread = threading.Thread(target=go, daemon=True)
        bg = _Background(index, step, thread, holder)
        holder["origin_before"] = origin_before
        thread.start()
        return bg

    def _join(self, bg: _Background) -> None:
        bg.thread.join(timeout=120)
        if bg.thread.is_alive():
            self._fail(bg.index, "the background request never completed")
            return
        if "error" in bg.result:
            error = bg.result["error"]
            if isinstance(error, ProviderUnsupported):
                self._fail(bg.index, f"provider cannot run the step: {error}")
            else:
                self._fail(bg.index, f"the background request failed: {error!r}")
            return
        self._check(bg.index, bg.step, bg.result["response"], None,
                    bg.result["origin_before"])

    # ---- expectations

    def _check(
        self,
        index: int,
        step: dict,
        r: Response,
        counters_before: Optional[Dict[str, int]],
        origin_before: int,
    ) -> None:
        expect = step["expect"]
        request = step["request"]
        fail = lambda what: self._fail(index, what)  # noqa: E731
        if not isinstance(r.headers, list) or not all(
            isinstance(h, tuple)
            and len(h) == 2
            and isinstance(h[0], str)
            and isinstance(h[1], str)
            for h in r.headers
        ):
            fail("the provider returned header lines that are not a list of (name, value) pairs")
            return
        if not step.get("background") and (
            set(expect) - BACKGROUND_EXPECT_KEYS - {"differential"}
        ):
            self.provider.settle()

        if r.status == 200 and any(
            media_type(v) == "text/markdown" for v in r.lines("Content-Type")
        ):
            etags = r.lines("ETag")
            if len(etags) == 1:
                self.markdown_etag = etags[0]

        if "status" in expect and r.status != expect["status"]:
            fail(f"status {r.status}, expected {expect['status']}")
        if "content_type" in expect:
            want = expect["content_type"]
            got = r.lines("Content-Type")
            if len(got) != 1:
                fail(f"{len(got)} Content-Type lines, expected one ({want})")
            elif ";" in want and got[0] != want:
                fail(f"Content-Type {got[0]!r}, expected {want!r}")
            elif ";" not in want and media_type(got[0]) != media_type(want):
                fail(f"Content-Type {got[0]!r}, expected media type {want!r}")
        exact = expect.get("headers_exact", {})
        for name, spec in exact.items():
            lines = r.lines(name)
            if len(lines) != spec["count"]:
                fail(f"{len(lines)} {name} lines {lines!r}, expected {spec['count']}")
                continue
            for value in lines:
                if "value" in spec and value != spec["value"]:
                    fail(f"{name}: {value!r}, expected {spec['value']!r}")
                if "contains" in spec and spec["contains"] not in value:
                    fail(f"{name}: {value!r} does not contain {spec['contains']!r}")
        if expect.get("headers_only"):
            allowed = {n.lower() for n in exact} | TRANSPORT_LINES
            for name, value in r.headers:
                if name.lower() not in allowed:
                    fail(f"header {name}: {value} is outside the allowed set")
            for name in sorted(TRANSPORT_LINES):
                count = len(r.lines(name))
                if count > 1:
                    fail(f"{count} {name.title()} lines; a transport line may appear once")
            if r.lines("Transfer-Encoding") and r.lines("Content-Length"):
                fail("Transfer-Encoding beside Content-Length (RFC 9112 section 6.3)")
        for name in expect.get("headers_absent", []):
            if r.lines(name):
                fail(f"header {name} present: {r.lines(name)!r}")
        if "body_ref" in expect:
            want = self.bodies[expect["body_ref"]].encode("utf-8")
            if r.body != want:
                fail(f"body is not {expect['body_ref']} ({len(r.body)} bytes)")
        if "body_bytes" in expect and len(r.body) != expect["body_bytes"]:
            fail(f"{len(r.body)} body bytes, expected {expect['body_bytes']}")
        if expect.get("length_matches_body"):
            lengths = r.lines("Content-Length")
            if len(lengths) != 1 or lengths[0].strip() != str(len(r.body)):
                fail(f"Content-Length {lengths!r} for {len(r.body)} body bytes")
        if "stored" in expect or "stored_lifetime_s" in expect or "sidecar_absent" in expect:
            entry = self.provider.stored_entry(request["path"], request.get("host"))
            if "stored" in expect and (entry is not None) != expect["stored"]:
                fail("stored" if entry is not None else "not stored")
            if entry is not None and "stored_lifetime_s" in expect:
                if entry.lifetime_s != expect["stored_lifetime_s"]:
                    fail(
                        f"stored lifetime {entry.lifetime_s} s, expected "
                        f"{expect['stored_lifetime_s']} s"
                    )
            elif entry is None and "stored_lifetime_s" in expect:
                fail("no stored entry to read the lifetime of")
            if "sidecar_absent" in expect:
                if entry is None:
                    fail("no stored entry to read the header block of")
                else:
                    names = {n.lower() for n, _ in (entry.sidecar or [])}
                    for name in expect["sidecar_absent"]:
                        if name.lower() in names:
                            fail(f"the stored header block carries {name}")
        if "origin_saw" in expect or "origin_saw_absent" in expect:
            seen = self.provider.origin_requests()[origin_before:]
            for name, value in expect.get("origin_saw", {}).items():
                if not seen:
                    fail("the origin saw no request")
                    break
                got = [v for n, v in seen[-1] if n.lower() == name.lower()]
                if got != [value]:
                    fail(f"the origin saw {name} {got!r}, expected [{value!r}]")
            for name in expect.get("origin_saw_absent", []):
                for lines in seen:
                    got = [v for n, v in lines if n.lower() == name.lower()]
                    if got:
                        fail(f"the origin saw {name} {got!r}")
        if counters_before is not None:
            after = self.provider.counters()
            for name, delta in expect["counter_delta"].items():
                got = after.get(name, 0) - counters_before.get(name, 0)
                if got != delta:
                    fail(f"{name} moved by {got}, expected {delta}")
        if "differential" in expect:
            self._differential(index, step, r, expect["differential"])

    def _differential(self, index: int, step: dict, r: Response, diff: dict) -> None:
        primary = r.headers
        if diff.get("allow_vary_accept"):
            primary, removed = _vary_without_accept(primary)
            if not removed:
                self._fail(index, "serve-variants HTML carries no Vary: Accept")
        own_mode = self.mode
        try:
            for mode in diff["against"]:
                self.provider.set_mode(mode)
                other = self._send(step)
                other_headers = other.headers
                if diff.get("allow_vary_accept"):
                    # The other mode may vary on Accept for reasons of its
                    # own (another feature); only the token serve-variants
                    # adds is allowed to differ.
                    other_headers, _ = _vary_without_accept(other_headers)
                if other.status != r.status:
                    self._fail(index, f"{mode}: status {other.status} != {r.status}")
                if other.body != r.body:
                    self._fail(index, f"{mode}: the body differs")
                if _header_multiset(other_headers) != _header_multiset(primary):
                    self._fail(
                        index,
                        f"{mode}: headers differ: {other.headers!r} vs {r.headers!r}",
                    )
        finally:
            self.provider.set_mode(own_mode)


def run_row(
    provider: Provider,
    row: dict,
    bodies: Dict[str, str],
    sleep: Callable[[float], None] = time.sleep,
    clock: Callable[[], float] = time.monotonic,
) -> List[str]:
    """Runs one live row; returns its failures (empty when it passed).
    The caller skips first when skip_reason gives a reason."""
    return RowRunner(provider, row, bodies, sleep=sleep, clock=clock).run()
