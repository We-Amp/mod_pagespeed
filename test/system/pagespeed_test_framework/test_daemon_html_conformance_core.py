#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2026 We-Amp B.V.

"""Unit tests for the HTML cache live runner core, against a fake provider.

No server runs. FakeProvider is an in-memory stand-in for a server port with
the HTML cache: it stores a cacheable HTML page, serves its Markdown copy
with the response header allowlist once the copy is ready, and answers 304
to the copy's entity tag. Its `defects` make it misbehave in one named way,
which is how these tests prove the runner notices.

Run with: python -m pytest -v test_daemon_html_conformance_core.py
"""

import json
import threading
from typing import Dict, List, Optional

import pytest

from pagespeed_test_framework import daemon_html_conformance as conf
from pagespeed_test_framework.daemon_html_conformance import (
    ORIGIN_KEYS,
    HeaderLines,
    Provider,
    ProviderUnsupported,
    Response,
    StoredEntry,
)

CASES = conf.load_cases()
BODIES = CASES["bodies"]
ROWS = {r["id"]: r for r in conf.live_rows(CASES)}

MARKDOWN_ETAG = 'W/"psm-0123456789abcdef"'


class FakeClock:
    """A clock that moves only when the runner sleeps."""

    def __init__(self) -> None:
        self.now = 0.0
        self.slept: List[float] = []

    def sleep(self, s: float) -> None:
        self.slept.append(s)
        self.now += s

    def __call__(self) -> float:
        return self.now


class FakeProvider(Provider):
    port = "apache"

    def __init__(self, defects=(), port: str = "apache") -> None:
        self.port = port
        self.defects = set(defects)
        self.calls: List[str] = []

    # ---- Provider

    def setup(self, row_id: str, live: dict, bodies: Dict[str, str]):
        self.calls.append("setup")
        self.live = live
        self.mode = live["mode"]
        self.agent_optimize = live["agent_optimize"]
        origin = live["origin"]
        self.origin_status = origin["status"]
        self.origin_headers = [tuple(h) for h in origin["headers"]]
        self.origin_body = bodies[origin["body_ref"]]
        self.echo = origin.get("echo_conditionals", False)
        self.vhost_add = [tuple(h) for h in live.get("vhost", {}).get("header_add", [])]
        self.entries: Dict[str, StoredEntry] = {}
        self.markdown: Dict[str, str] = {}
        self.seen: List[HeaderLines] = []
        self.stats: Dict[str, int] = {}
        self.held: Optional[threading.Event] = None
        self.held_done: Optional[threading.Event] = None
        # Commits of answered requests not yet applied (late_commit).
        self.pending: List[str] = []
        self.paths: List[str] = []
        self.holding: List[tuple] = []
        self.ids: Dict[str, set] = {}
        applied = {"mode", "agent_optimize", "worker"}
        applied |= {f"origin.{k}" for k in ORIGIN_KEYS}
        applied |= {"header_add"}
        if "ignores_header_add" in self.defects:
            applied.discard("header_add")
            self.vhost_add = []
        return applied

    def teardown(self) -> None:
        self.calls.append("teardown")
        if self.held is not None:
            self.held.set()

    def _serving(self) -> bool:
        return self.mode == "serve-variants" and self.agent_optimize

    def request(self, method, path, host, headers, process=None) -> Response:
        # The runner passes the row's path and header lines verbatim.
        assert isinstance(path, str) and isinstance(headers, list), (path, headers)
        assert all(isinstance(h, tuple) and len(h) == 2 for h in headers), headers
        self.paths.append(path)
        self.calls.append(f"request {method} {path}")
        pid = {"a": "101", "b": "102"}.get(process, "100")
        if process == "b" and "same_process" in self.defects:
            pid = "101"
        if process is not None:
            self.ids.setdefault(process, set()).add(pid)
        accept = ", ".join(v for n, v in headers if n.lower() == "accept")
        inm = [v for n, v in headers if n.lower() == "if-none-match"]
        if self._serving() and "text/markdown" in accept and path in self.markdown:
            return self._markdown(method, path, inm)
        # To the origin, without this module's own entity tags.
        forwarded = [
            (n, v)
            for n, v in headers
            if not (n.lower() == "if-none-match" and ("psm-" in v or "psh-" in v))
        ]
        if "leak_conditionals" in self.defects:
            forwarded = list(headers)
        self.seen.append(forwarded)
        done = None
        if self.held is not None:
            held, self.held = self.held, None
            done = self.held_done
            if "never_holds" not in self.defects:
                self.holding.append((method, path))
            held.wait(timeout=30)
            if (method, path) in self.holding:
                self.holding.remove((method, path))
        lines = list(self.origin_headers) + list(self.vhost_add)
        if self._serving() and "no_vary_accept" not in self.defects:
            lines.append(("Vary", "Accept"))
        if "differential_extra" in self.defects and self.mode == "serve-variants":
            lines.append(("X-Extra", "1"))
        if (
            self.mode == "serve-variants"
            and method == "GET"
            and self.origin_status == 200
            and "never_stores" not in self.defects
        ):
            if "late_commit" in self.defects:
                self.pending.append(path)
            else:
                self._commit(path)
        body = b"" if method == "HEAD" else self.origin_body.encode()
        if "chunked_and_length" in self.defects:
            lines += [("Content-Length", str(len(body))), ("Transfer-Encoding", "chunked")]
        if done is not None:
            done.set()
        return Response(self.origin_status, lines, body)

    def _commit(self, path: str) -> None:
        self.entries[path] = StoredEntry(lifetime_s=300, sidecar=list(self.origin_headers))
        self.markdown[path] = BODIES["page_md"]
        self.stats["daemon_html_stored"] = self.stats.get("daemon_html_stored", 0) + 1

    def settle(self) -> None:
        self.calls.append("settle")
        for path in self.pending:
            self._commit(path)
        self.pending = []

    def _markdown(self, method, path, inm) -> Response:
        self.stats["daemon_markdown_served"] = self.stats.get("daemon_markdown_served", 0) + 1
        vary = [("Vary", "Accept")]
        if "duplicate_vary" in self.defects:
            vary.append(("Vary", "Accept"))
        if MARKDOWN_ETAG in inm:
            return Response(
                304,
                [("Date", "x"), ("ETag", MARKDOWN_ETAG), ("Cache-Control", "private")] + vary,
            )
        md = self.markdown[path].encode()
        lines = [
            ("Date", "x"),
            ("Content-Type", "text/markdown; charset=utf-8"),
            ("Content-Length", str(len(md))),
            ("Cache-Control", "private"),
        ] + vary + [
            ("X-Robots-Tag", "noindex"),
            ("Age", "0"),
            ("ETag", MARKDOWN_ETAG),
        ]
        if "extra_header" in self.defects:
            lines.append(("Strict-Transport-Security", "max-age=1"))
        return Response(200, lines, b"" if method == "HEAD" else md)

    def stored_entry(self, path, host) -> Optional[StoredEntry]:
        return self.entries.get(path)

    def counters(self) -> Dict[str, int]:
        return dict(self.stats)

    def origin_requests(self) -> List[HeaderLines]:
        return list(self.seen)

    def markdown_ready(self, path, host) -> bool:
        self.calls.append("markdown_ready")
        return path in self.markdown and "never_ready" not in self.defects

    def held_requests(self):
        return list(self.holding)

    def process_ids(self):
        out = {k: set(v) for k, v in self.ids.items()}
        if self.calls and any(c.startswith("purge") for c in self.calls):
            out["purge"] = {"102" if "purge_on_b" in self.defects else "100"}
        return out

    def purge_url(self, url, form) -> None:
        if "no_purge" in self.defects:
            raise ProviderUnsupported("this lane cannot purge")
        self.calls.append(f"purge_url {url} {form}")
        self.entries.clear()
        self.markdown.clear()

    def purge_global(self, via) -> None:
        self.calls.append(f"purge_global {via}")
        self.entries.clear()
        self.markdown.clear()

    def set_origin_body(self, body: str) -> None:
        self.origin_body = body

    def hold_origin(self) -> None:
        self.calls.append("hold_origin")
        self.held = threading.Event()
        self.held_done = threading.Event()
        self._release = self.held

    def release_origin(self) -> None:
        self.calls.append("release_origin")
        self._release.set()
        # The held response has been produced (and committed) on return.
        self.held_done.wait(timeout=30)

    def set_mode(self, mode: str) -> None:
        self.calls.append(f"set_mode {mode}")
        self.mode = mode


def run(row: dict, provider: Optional[FakeProvider] = None, clock=None) -> List[str]:
    clock = clock or FakeClock()
    return conf.run_row(provider or FakeProvider(), row, BODIES, sleep=clock.sleep, clock=clock)


def synthetic(steps, mode="serve-variants", **live) -> dict:
    body = {
        "mode": mode,
        "agent_optimize": True,
        "origin": {
            "status": 200,
            "headers": [
                ["Content-Type", "text/html; charset=utf-8"],
                ["Cache-Control", "max-age=300"],
            ],
            "body_ref": "page",
        },
        "steps": steps,
    }
    body.update(live)
    return {"id": "synthetic", "runners": ["live"], "why": "test", "live": body}


GET_PAGE = {"method": "GET", "path": "/page", "headers": []}
WAIT_HELD = {"wait_origin_held": {"method": "GET", "path": "/page", "timeout_s": 5}}
GET_MD = {"method": "GET", "path": "/page", "headers": [["Accept", "text/markdown"]]}


class TestTheFile:
    def test_has_live_rows(self):
        assert len(ROWS) > 100

    def test_pending_rows_are_held_back_and_counted_apart(self):
        pending = conf.pending_rows(CASES)
        assert all("pending" in r for r in pending)
        assert not any("pending" in r for r in ROWS.values())
        ids = {r["id"] for r in pending}
        assert ids.isdisjoint(ROWS)
        live = [r for r in CASES["rows"] if "live" in r.get("runners", [])]
        assert len(ROWS) + len(pending) == len(live)
        assert len(ROWS) == conf.LIVE_ROWS_THAT_RUN
        assert len(pending) == conf.LIVE_ROWS_PENDING
        print(f"live rows: {len(ROWS)} run, {len(pending)} pending")

    def test_the_mode_audit_reruns_every_markdown_row_in_serve_cached_html(self):
        audit = conf.mode_audit_rows(list(ROWS.values()))
        assert audit, "no serve-variants Markdown row to rerun"
        by_source = {r["mode_audit"]: r for r in audit}
        for rid, row in ROWS.items():
            live = row["live"]
            text = json.dumps(live)
            wanted = (live["mode"] == "serve-variants" and "text/markdown" in text
                      and "set_mode" not in text and "differential" not in text)
            assert (rid in by_source) == wanted, rid
            if wanted:
                copy_row = by_source[rid]
                assert copy_row["live"]["mode"] == "serve-cached-html"
                assert copy_row["id"] == rid + "-in-serve-cached-html"
                assert copy_row["live"]["steps"] == live["steps"]
                assert live["mode"] == "serve-variants"  # the table is untouched
                assert conf.validate_row(copy_row, BODIES) == []
        # Only an audit copy may name the mode.
        plain = dict(next(iter(by_source.values())))
        plain.pop("mode_audit")
        assert any("mode" in e for e in conf.validate_row(plain, BODIES))

    def test_every_live_row_uses_only_keys_the_runner_acts_on(self):
        errors = []
        for row in ROWS.values():
            errors += conf.validate_row(row, BODIES)
        assert errors == []

    def test_an_unknown_key_is_an_error_not_ignored(self):
        row = synthetic([{"request": GET_PAGE, "expect": {"status": 200, "colour": 1}}])
        assert run(row) == ["synthetic: unknown step 0 expect key colour"]


class TestRowsFromTheFile:
    @pytest.mark.parametrize(
        "row_id",
        [
            "live-inm-fresh-304",
            "live-head-markdown",
            "live-markdown-first-request-html-then-markdown",
            "live-kill-switch-off",
            "live-differential-agent-optimize-on",
        ],
    )
    def test_passes_on_a_well_behaved_provider(self, row_id):
        provider = FakeProvider()
        assert run(ROWS[row_id], provider) == []
        assert provider.calls[0] == "setup"
        assert provider.calls[-1] == "teardown"

    def test_the_304_is_asked_with_the_etag_of_the_last_markdown_200(self):
        provider = FakeProvider()
        assert run(ROWS["live-inm-fresh-304"], provider) == []
        # The 304 step got a 304, so the substituted tag reached the server.
        assert provider.stats["daemon_markdown_served"] == 2

    def test_a_duplicate_vary_line_fails_the_exact_count(self):
        failures = run(ROWS["live-inm-fresh-304"], FakeProvider({"duplicate_vary"}))
        assert any("2 Vary lines" in f for f in failures), failures

    def test_a_header_outside_the_allowlist_fails(self):
        failures = run(ROWS["live-head-markdown"], FakeProvider({"extra_header"}))
        assert any("Strict-Transport-Security" in f and "outside" in f for f in failures), failures

    def test_a_page_never_stored_fails(self):
        failures = run(ROWS["live-head-markdown"], FakeProvider({"never_stores"}))
        assert any("not stored" in f for f in failures), failures

    def test_a_copy_that_never_becomes_ready_fails_after_the_timeout(self):
        clock = FakeClock()
        failures = run(ROWS["live-head-markdown"], FakeProvider({"never_ready"}), clock)
        assert any("not servable within 30 s" in f for f in failures), failures
        assert clock.now >= 30

    def test_a_differential_difference_fails(self):
        failures = run(ROWS["live-differential-agent-optimize-on"], FakeProvider({"differential_extra"}))
        assert any("headers differ" in f for f in failures), failures

    def test_the_differential_restores_the_rows_mode(self):
        provider = FakeProvider()
        assert run(ROWS["live-differential-agent-optimize-on"], provider) == []
        assert provider.calls[-2] == "set_mode serve-variants"


class TestSkips:
    def test_only_a_port_outside_the_rows_ports_skips(self):
        row = {"id": "r", "ports": ["nginx"]}
        assert conf.skip_reason(row, "apache") == "row r is for nginx, not apache"
        assert conf.skip_reason(row, "nginx") is None
        assert conf.skip_reason({"id": "r"}, "iis") is None

    def test_a_provider_that_cannot_do_a_step_fails_the_row(self):
        row = synthetic(
            [{"request": GET_PAGE, "pre": [{"purge_url": {"url": "/page", "form": "PURGE"}}],
              "expect": {"status": 200}}]
        )
        provider = FakeProvider({"no_purge"})
        failures = run(row, provider)
        assert failures == ["synthetic step 0: provider cannot run the step: this lane cannot purge"]
        assert provider.calls[-1] == "teardown"


class TestOps:
    def test_wait_markdown_expect_ready_false_fails_when_the_copy_is_ready(self):
        row = synthetic(
            [
                {"request": GET_PAGE, "expect": {"stored": True}},
                {"request": GET_MD, "pre": [{"wait_markdown": {"timeout_s": 5, "expect_ready": False}}],
                 "expect": {"status": 200}},
            ]
        )
        assert run(row) == ["synthetic step 1: the Markdown copy became servable"]

    def test_wait_markdown_expect_ready_false_passes_when_it_stays_unready(self):
        clock = FakeClock()
        row = synthetic(
            [{"request": GET_MD, "pre": [{"wait_markdown": {"timeout_s": 5, "expect_ready": False}}],
              "expect": {"status": 200, "content_type": "text/html"}}]
        )
        assert run(row, FakeProvider(), clock) == []
        assert clock.now >= 5

    def test_sleep_goes_through_the_runners_clock(self):
        clock = FakeClock()
        row = synthetic([{"request": GET_PAGE, "pre": [{"sleep_s": {"s": 6}}], "expect": {"status": 200}}])
        assert run(row, FakeProvider(), clock) == []
        assert clock.slept == [6.0]

    def test_a_background_step_is_checked_after_the_next_step(self):
        row = synthetic(
            [
                {"request": GET_PAGE, "pre": [{"hold_origin": {}}], "background": True,
                 "expect": {"status": 200, "content_type": "text/html"}},
                {"request": GET_PAGE, "pre": [WAIT_HELD, {"release_origin": {}}], "expect": {"status": 200}},
            ]
        )
        provider = FakeProvider()
        assert run(row, provider) == []
        assert provider.calls.index("hold_origin") < provider.calls.index("release_origin")

    def test_a_failing_background_step_is_reported_with_its_own_index(self):
        row = synthetic(
            [
                {"request": GET_PAGE, "pre": [{"hold_origin": {}}], "background": True,
                 "expect": {"status": 500}},
                {"request": GET_PAGE, "pre": [WAIT_HELD, {"release_origin": {}}], "expect": {"status": 200}},
            ]
        )
        assert run(row) == ["synthetic step 0: status 200, expected 500"]

    def test_counter_delta(self):
        row = synthetic(
            [
                {"request": GET_PAGE, "expect": {"stored": True}},
                {"request": GET_MD, "expect": {"counter_delta": {"daemon_markdown_served": 2}}},
            ]
        )
        assert run(row) == ["synthetic step 1: daemon_markdown_served moved by 1, expected 2"]

    def test_origin_saw_and_origin_saw_absent(self):
        psm = ["If-None-Match", MARKDOWN_ETAG]
        row = synthetic(
            [
                {"request": {"method": "GET", "path": "/page", "headers": [psm]},
                 "expect": {"origin_saw_absent": ["If-None-Match"]}},
                {"request": {"method": "GET", "path": "/page", "headers": [["If-None-Match", 'W/"v1"']]},
                 "expect": {"origin_saw": {"If-None-Match": 'W/"v1"'}}},
            ]
        )
        assert run(row) == []
        failures = run(row, FakeProvider({"leak_conditionals"}))
        assert len(failures) == 1 and "the origin saw If-None-Match" in failures[0], failures

    def test_markdown_etag_before_any_markdown_200_fails(self):
        row = synthetic(
            [{"request": {"method": "GET", "path": "/page", "headers": [["If-None-Match", "$markdown_etag"]]},
              "expect": {"status": 200}}]
        )
        failures = run(row)
        assert len(failures) == 1 and "$markdown_etag" in failures[0], failures

    def test_content_type_without_parameters_compares_the_media_type(self):
        row = synthetic([{"request": GET_PAGE, "expect": {"content_type": "text/html"}}])
        assert run(row) == []
        row = synthetic([{"request": GET_PAGE, "expect": {"content_type": "text/html; charset=latin1"}}])
        assert len(run(row)) == 1

    def test_stored_lifetime_and_sidecar_absent(self):
        row = synthetic(
            [{"request": GET_PAGE,
              "expect": {"stored": True, "stored_lifetime_s": 60, "sidecar_absent": ["Cache-Control"]}}]
        )
        assert run(row) == [
            "synthetic step 0: stored lifetime 300 s, expected 60 s",
            "synthetic step 0: the stored header block carries Cache-Control",
        ]


class TestSettle:
    def test_a_store_committed_after_the_answer_is_seen_by_stored_false(self):
        # The fake stores every page; with late_commit the entry exists only
        # once settle() ran, so a stored:false check must settle first.
        row = synthetic([{"request": GET_PAGE, "expect": {"stored": False}}])
        failures = run(row, FakeProvider({"late_commit"}))
        assert failures == ["synthetic step 0: stored"], failures

    def test_a_late_commit_still_passes_a_correct_row(self):
        assert run(ROWS["live-head-markdown"], FakeProvider({"late_commit"})) == []

    def test_settle_comes_before_every_volume_and_counter_read(self):
        provider = FakeProvider()
        row = synthetic([{"request": GET_PAGE, "expect": {"stored": True,
                          "counter_delta": {"daemon_html_stored": 1}}}])
        assert run(row, provider) == []
        calls = provider.calls
        request = calls.index("request GET /page")
        assert "settle" in calls[:request], calls
        assert "settle" in calls[request:], calls


class TestStepCounters:
    def test_counter_delta_covers_the_pre_ops(self):
        # The background request commits while step 1's pre releases it; the
        # delta of step 1 must include that commit.
        row = synthetic(
            [
                {"request": GET_PAGE, "pre": [{"hold_origin": {}}], "background": True,
                 "expect": {"status": 200}},
                {"request": GET_MD, "pre": [WAIT_HELD, {"release_origin": {}}],
                 "expect": {"counter_delta": {"daemon_html_stored": 1}}},
            ]
        )
        assert run(row) == []

    @pytest.mark.parametrize("key,value", [
        ("counter_delta", {"daemon_html_stored": 1}),
        ("stored", True),
        ("stored_lifetime_s", 300),
        ("sidecar_absent", ["X"]),
        ("origin_saw", {"X": "1"}),
        ("origin_saw_absent", ["X"]),
        ("differential", {"against": ["off"]}),
    ])
    def test_a_background_step_may_not_assert_state(self, key, value):
        row = synthetic(
            [
                {"request": GET_PAGE, "background": True, "expect": {"status": 200, key: value}},
                {"request": GET_PAGE, "expect": {"status": 200}},
            ]
        )
        assert conf.validate_row(row, BODIES) == [
            f"synthetic: step 0 is a background step and cannot assert {key}"]


class LaggingProvider(FakeProvider):
    """Process "b" keeps serving the Markdown copy for `lag_s` after a
    purge (by the runner's clock), as a process that has not polled yet
    would; with `flap`, it serves it again once after its first miss."""

    def __init__(self, clock, lag_s: float, flap: bool = False, **kw) -> None:
        super().__init__(**kw)
        self.clock = clock
        self.lag_s = lag_s
        self.flap = flap
        self.purged_at: Optional[float] = None
        self.stale_copy: Dict[str, str] = {}
        self.missed = False
        self.processes: List[Optional[str]] = []

    def _stale_purge(self) -> None:
        self.stale_copy = dict(self.markdown)
        self.purged_at = self.clock()

    def purge_url(self, url, form) -> None:
        self._stale_purge()
        super().purge_url(url, form)

    def purge_global(self, via) -> None:
        self._stale_purge()
        super().purge_global(via)

    def request(self, method, path, host, headers, process=None) -> Response:
        self.processes.append(process)
        accept = ", ".join(v for n, v in headers if n.lower() == "accept")
        if process == "b" and self.purged_at is not None and "text/markdown" in accept \
                and path in self.stale_copy:
            lagging = self.clock() - self.purged_at < self.lag_s
            if lagging or (self.flap and self.missed):
                self.flap = self.flap and not self.missed
                self.markdown[path] = self.stale_copy[path]
                try:
                    return self._markdown(method, path, [])
                finally:
                    del self.markdown[path]
            self.missed = True
        return super().request(method, path, host, headers, process)


HEAD_MD = {"method": "HEAD", "path": "/page", "headers": [["Accept", "text/markdown"]]}


def window_row(purge=None, **window) -> dict:
    w = {"stale_max_s": 3, "margin_s": 2, "observe_s": 10, "every_s": 0.25,
         "stale": {"content_type": "text/markdown; charset=utf-8"}}
    w.update(window)
    return synthetic([
        {"request": GET_PAGE, "expect": {"stored": True}},
        {"request": HEAD_MD, "process": "b", "pre": [{"wait_markdown": {"timeout_s": 5}}],
         "expect": {"status": 200, "content_type": "text/markdown; charset=utf-8"}},
        {"request": HEAD_MD, "process": "b",
         "pre": [purge or {"purge_url": {"url": "/page", "form": "PURGE"}}],
         "window": w, "expect": {"status": 200, "content_type": "text/html"}},
    ])


class TestWindow:
    def test_a_process_that_catches_up_within_the_interval_passes(self):
        clock = FakeClock()
        provider = LaggingProvider(clock, lag_s=2.9)
        runner = conf.RowRunner(provider, window_row(), BODIES, sleep=clock.sleep, clock=clock)
        assert runner.run() == []
        record = runner.windows[0]
        assert record["stale"] > 0 and record["miss"] > 0
        assert record["last_stale_s"] < 3 <= record["first_miss_s"] + 0.25
        # It kept watching until observe_s, every request on process b.
        assert clock.now >= 10
        assert set(provider.processes[2:]) == {"b"}

    def test_the_margin_is_allowed(self):
        clock = FakeClock()
        assert run(window_row(), LaggingProvider(clock, lag_s=4.9), clock) == []

    def test_a_process_serving_past_the_interval_and_margin_fails(self):
        clock = FakeClock()
        failures = run(window_row(), LaggingProvider(clock, lag_s=5.6), clock)
        assert failures == ["synthetic step 2: the pre-purge entry was still served for a "
                            "request sent 5.25 s after the purge (allowed: 3 s poll interval "
                            "+ 2 s margin)"], failures

    def test_a_process_that_never_catches_up_fails(self):
        clock = FakeClock()
        failures = run(window_row(), LaggingProvider(clock, lag_s=1e9), clock)
        assert len(failures) == 1 and "still served" in failures[0], failures

    def test_a_stale_answer_after_a_miss_fails(self):
        clock = FakeClock()
        failures = run(window_row(), LaggingProvider(clock, lag_s=1.0, flap=True), clock)
        assert len(failures) == 1, failures
        assert "after a miss at 1.00 s" in failures[0], failures

    def test_a_global_purge_starts_the_window_too(self):
        clock = FakeClock()
        row = window_row(purge={"purge_global": {"via": "admin"}})
        assert run(row, LaggingProvider(clock, lag_s=2.0), clock) == []

    def test_a_miss_is_checked_against_expect(self):
        clock = FakeClock()
        row = window_row()
        row["live"]["steps"][2]["expect"]["status"] = 404
        failures = run(row, LaggingProvider(clock, lag_s=1.0), clock)
        assert failures == ["synthetic step 2: status 200, expected 404"], failures

    @pytest.mark.parametrize("change,error", [
        (lambda s: s.pop("process"), "a window step names the process it watches"),
        (lambda s: s.update(pre=[]), "a window step starts at a purge in its pre ops"),
        (lambda s: s.update(background=True), "a window step cannot be a background step"),
        (lambda s: s["expect"].update(stored=True), "a window step cannot assert stored"),
        (lambda s: s["window"].update(observe_s=5), "window observe_s must outlast"),
        (lambda s: s["window"].update(margin_s=0), "window margin_s must be a positive number"),
        (lambda s: s["window"].update(extra=1), "unknown step 2 window key extra"),
        (lambda s: s["window"]["stale"].update(content_type="text/html"),
         "window stale and expect name the same content type"),
    ])
    def test_validation(self, change, error):
        row = window_row()
        change(row["live"]["steps"][2])
        errors = conf.validate_row(row, BODIES)
        assert any(error in e for e in errors), errors

    def test_the_files_window_rows_bound_the_poll_interval_they_configure(self):
        rows = [r for r in ROWS.values()
                if any("window" in s for s in r["live"]["steps"])]
        assert {r["id"] for r in rows} >= {"live-race-cross-process-window-url",
                                          "live-race-cross-process-window-global"}
        for row in rows:
            poll = row["live"]["vhost"]["poll_interval_s"]
            for step in row["live"]["steps"]:
                if "window" in step:
                    assert step["window"]["stale_max_s"] == poll, row["id"]
                    assert step["process"] == "b", row["id"]


class TestHeldOrigin:
    def _row(self):
        return synthetic([
            {"request": GET_PAGE, "pre": [{"hold_origin": {}}], "background": True,
             "expect": {"status": 200}},
            {"request": GET_MD, "pre": [WAIT_HELD, {"purge_url": {"url": "/page"}},
                                        {"release_origin": {}}],
             "expect": {"status": 200}},
        ])

    def test_the_purge_waits_for_the_held_request(self):
        provider = FakeProvider()
        assert run(self._row(), provider) == []
        # The held GET reached the origin before the purge went out.
        assert provider.calls.index("request GET /page") < provider.calls.index(
            "purge_url /page PURGE")

    def test_an_origin_that_never_holds_fails_at_the_deadline(self):
        clock = FakeClock()
        failures = run(self._row(), FakeProvider({"never_holds"}), clock)
        assert "synthetic step 1: the origin never held GET /page within 5 s" in failures, failures
        assert clock.now >= 5

    @pytest.mark.parametrize("pre", [
        [{"purge_url": {"url": "/page"}}, {"release_origin": {}}],
        [{"purge_url": {"url": "/page"}}, WAIT_HELD, {"release_origin": {}}],
        [{"release_origin": {}}, WAIT_HELD],
    ])
    def test_a_release_without_a_wait_first_is_invalid(self, pre):
        row = self._row()
        row["live"]["steps"][1]["pre"] = pre
        errors = conf.validate_row(row, BODIES)
        assert any("without waiting for the held request first" in e for e in errors), errors


class TestProcesses:
    def _row(self):
        return window_row()

    def test_distinct_processes_pass(self):
        clock = FakeClock()
        assert run(self._row(), LaggingProvider(clock, lag_s=1.0), clock) == []

    def test_one_process_behind_two_names_fails(self):
        clock = FakeClock()
        row = self._row()
        row["live"]["steps"][0]["process"] = "a"
        failures = run(row, LaggingProvider(clock, lag_s=1.0, defects={"same_process"}), clock)
        assert failures == ["synthetic: processes a and b are the same server process (101)"], failures

    def test_a_purge_answered_by_the_watched_process_fails(self):
        clock = FakeClock()
        failures = run(self._row(), LaggingProvider(clock, lag_s=1.0, defects={"purge_on_b"}), clock)
        assert failures == ["synthetic: the purge was answered by the watched process b (['102'])"], failures

    def test_a_name_answered_by_two_processes_fails(self):
        class Drifting(LaggingProvider):
            def process_ids(self):
                out = super().process_ids()
                out["b"] = {"102", "103"}
                return out

        clock = FakeClock()
        failures = run(self._row(), Drifting(clock, lag_s=1.0), clock)
        assert failures == ["synthetic: process b was answered by 2 server processes "
                            "['102', '103'], expected exactly one"], failures

    def test_a_transport_error_in_a_window_is_a_clear_failure(self):
        class Resetting(LaggingProvider):
            def request(self, method, path, host, headers, process=None):
                if self.purged_at is not None and process == "b":
                    raise ConnectionResetError("reset")
                return super().request(method, path, host, headers, process)

        clock = FakeClock()
        failures = run(self._row(), Resetting(clock, lag_s=1.0), clock)
        assert failures == ["synthetic step 2: the request sent 0.00 s after the purge failed "
                            "in transport: ConnectionResetError('reset')"], failures


class TestLaneSkips:
    ROWS_BY_ID = {
        "all": {"id": "all"},
        "apache-only": {"id": "apache-only", "ports": ["apache"]},
        "nginx-only": {"id": "nginx-only", "ports": ["nginx"]},
    }

    def test_the_apache_lane_collects_only_its_rows(self):
        kept, left = conf.lane_rows(list(self.ROWS_BY_ID.values()), "apache")
        assert [r["id"] for r in kept] == ["all", "apache-only"]
        assert [r["id"] for r in left] == ["nginx-only"]

    def test_other_lanes_collect_the_whole_table(self):
        for port in ("nginx", "iis", None):
            kept, left = conf.lane_rows(list(self.ROWS_BY_ID.values()), port)
            assert len(kept) == 3 and left == []

    def test_any_skip_on_the_apache_lane_is_a_violation(self):
        out = conf.skip_violations("apache", {"all": "x"}, self.ROWS_BY_ID)
        assert out == ["all: skipped on the apache lane, which allows no skip (x)"]

    def test_nginx_may_skip_only_rows_for_other_ports(self):
        assert conf.skip_violations("nginx", {"apache-only": "r"}, self.ROWS_BY_ID) == []
        assert conf.skip_violations("nginx", {"all": "r", "nginx-only": "r"}, self.ROWS_BY_ID) == [
            "all: skipped on the nginx lane, but its ports include nginx (r)",
            "nginx-only: skipped on the nginx lane, but its ports include nginx (r)",
        ]

    def test_lane_port(self):
        assert conf.lane_port("auto") == "apache"
        assert conf.lane_port("apache") == "apache"
        assert conf.lane_port("nginx") == "nginx"
        assert conf.lane_port("iis") == "iis"
        assert conf.lane_port("envoy") is None

    def test_every_apache_lane_row_runs_on_the_apache_provider_port(self):
        kept, _ = conf.lane_rows(list(ROWS.values()), "apache")
        assert all(conf.skip_reason(r, "apache") is None for r in kept)


class TestDifferential:
    def test_allow_vary_accept_requires_the_token(self):
        failures = run(ROWS["live-differential-agent-optimize-on"], FakeProvider({"no_vary_accept"}))
        assert any("carries no Vary: Accept" in f for f in failures), failures


class TestRawLines:
    def test_the_provider_must_return_header_lines_not_a_mapping(self):
        class MappingProvider(FakeProvider):
            def request(self, *a, **k):
                r = super().request(*a, **k)
                return Response(r.status, dict(r.headers), r.body)

        failures = run(synthetic([{"request": GET_PAGE, "expect": {"status": 200}}]), MappingProvider())
        assert failures == ["synthetic step 0: the provider returned header lines that are "
                            "not a list of (name, value) pairs"], failures

    def test_the_path_reaches_the_provider_byte_for_byte(self):
        provider = FakeProvider()
        path = "//page/%2F/./../%u0041::$DATA"
        row = synthetic([{"request": {"method": "GET", "path": path, "headers": []},
                          "expect": {"status": 200}}])
        assert run(row, provider) == []
        assert provider.paths == [path]

    def test_send_raw_keeps_every_line_and_the_path(self):
        import socket
        seen = {}
        srv = socket.socket()
        srv.bind(("127.0.0.1", 0))
        srv.listen(1)

        def serve():
            conn, _ = srv.accept()
            data = b""
            while b"\r\n\r\n" not in data:
                data += conn.recv(4096)
            seen["line"] = data.split(b"\r\n", 1)[0]
            seen["headers"] = data.split(b"\r\n")[1:]
            conn.sendall(b"HTTP/1.1 200 OK\r\nVary: Accept\r\nVary: Accept\r\n"
                         b"Content-Length: 2\r\nConnection: close\r\n\r\nok")
            conn.close()

        t = threading.Thread(target=serve, daemon=True)
        t.start()
        r = conf.send_raw("127.0.0.1", srv.getsockname()[1], "GET", "//a/%2F/../b",
                          "www.example.test", [("Accept", "text/markdown"), ("Accept", "text/html")])
        t.join()
        srv.close()
        assert seen["line"] == b"GET //a/%2F/../b HTTP/1.1"
        assert b"Accept: text/markdown" in seen["headers"]
        assert b"Accept: text/html" in seen["headers"]
        assert b"Host: www.example.test" in seen["headers"]
        assert r.status == 200 and r.body == b"ok"
        assert r.lines("Vary") == ["Accept", "Accept"]


class TestHeadersOnly:
    def _row(self, extra):
        return synthetic([{"request": GET_PAGE, "expect": {
            "headers_exact": {"Content-Type": {"count": 1}, "Cache-Control": {"count": 1},
                              "Vary": {"count": 1}, "Content-Length": {"count": 1}},
            "headers_only": True}}])

    def test_transfer_encoding_beside_content_length_fails(self):
        failures = run(self._row(None), FakeProvider({"chunked_and_length"}))
        assert any("Transfer-Encoding beside Content-Length" in f for f in failures), failures

    def test_a_repeated_transport_line_fails(self):
        class TwoDates(FakeProvider):
            def request(self, *a, **k):
                r = super().request(*a, **k)
                return Response(r.status, r.headers + [("Date", "a"), ("Date", "b")], r.body)

        row = synthetic([{"request": GET_PAGE, "expect": {
            "headers_exact": {"Content-Type": {"count": 1}, "Cache-Control": {"count": 1},
                              "Vary": {"count": 1}},
            "headers_only": True}}])
        failures = run(row, TwoDates())
        assert failures == ["synthetic step 0: 2 Date lines; a transport line may appear once"], failures


class TestAppliedKnobs:
    def test_a_knob_the_provider_did_not_apply_fails_the_row(self):
        failures = run(ROWS["live-markdown-first-request-html-then-markdown"],
                       FakeProvider({"ignores_header_add"}))
        assert failures == ["live-markdown-first-request-html-then-markdown: the provider did "
                            "not apply header_add"], failures

    def test_setup_must_report_what_it_applied(self):
        class Silent(FakeProvider):
            def setup(self, *a):
                super().setup(*a)
                return None

        failures = run(synthetic([{"request": GET_PAGE, "expect": {"status": 200}}]), Silent())
        assert failures == ["synthetic: the provider did not apply agent_optimize, mode, "
                            "origin.body_ref, origin.headers, origin.status"], failures


class TestProviderPorts:
    def test_every_provider_module_has_its_port_listed_and_back(self):
        import os
        here = os.path.dirname(os.path.abspath(conf.__file__))
        modules = {f[len("daemon_html_provider_"):-3] for f in os.listdir(here)
                   if f.startswith("daemon_html_provider_") and f.endswith(".py")}
        assert modules == set(conf.PROVIDER_PORTS)
        assert set(conf.PROVIDER_PORTS) <= set(conf.PORTS)

    def test_a_port_without_a_provider_is_skipped(self, monkeypatch):
        monkeypatch.setattr(conf, "PROVIDER_PORTS", frozenset())
        assert conf.load_provider("iis", object()) is None

    def test_a_listed_port_whose_provider_is_missing_fails(self, monkeypatch):
        monkeypatch.setattr(conf, "PROVIDER_PORTS", frozenset(["no-such-port"]))
        with pytest.raises(ModuleNotFoundError):
            conf.load_provider("no-such-port", object())


class TestHeaderHelpers:
    def test_vary_without_accept_takes_one_token(self):
        lines = [("Vary", "Accept-Encoding, Accept"), ("X", "1"), ("Vary", "Accept")]
        assert conf._vary_without_accept(lines) == (
            [("Vary", "Accept-Encoding"), ("X", "1"), ("Vary", "Accept")], True)
        assert conf._vary_without_accept([("Vary", "Accept")]) == ([], True)
        assert conf._vary_without_accept([("Vary", "Cookie")]) == ([("Vary", "Cookie")], False)


if __name__ == "__main__":
    raise SystemExit(pytest.main([__file__, "-v"]))
