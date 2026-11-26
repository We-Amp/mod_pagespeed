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

import threading
from typing import Dict, List, Optional

import pytest

from pagespeed_test_framework import daemon_html_conformance as conf
from pagespeed_test_framework.daemon_html_conformance import (
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

    def setup(self, row_id: str, live: dict, bodies: Dict[str, str]) -> None:
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

    def teardown(self) -> None:
        self.calls.append("teardown")
        if self.held is not None:
            self.held.set()

    def _serving(self) -> bool:
        return self.mode == "serve-variants" and self.agent_optimize

    def request(self, method, path, host, headers, process=None) -> Response:
        self.calls.append(f"request {method} {path}")
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
        if self.held is not None:
            held, self.held = self.held, None
            held.wait(timeout=30)
        lines = list(self.origin_headers) + list(self.vhost_add)
        if self._serving():
            lines.append(("Vary", "Accept"))
        if "differential_extra" in self.defects and self.mode == "serve-variants":
            lines.append(("X-Extra", "1"))
        if (
            self.mode == "serve-variants"
            and method == "GET"
            and self.origin_status == 200
            and "never_stores" not in self.defects
        ):
            self.entries[path] = StoredEntry(lifetime_s=300, sidecar=list(self.origin_headers))
            self.markdown[path] = BODIES["page_md"]
        body = b"" if method == "HEAD" else self.origin_body.encode()
        return Response(self.origin_status, lines, body)

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
        self._release = self.held

    def release_origin(self) -> None:
        self.calls.append("release_origin")
        self._release.set()

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
GET_MD = {"method": "GET", "path": "/page", "headers": [["Accept", "text/markdown"]]}


class TestTheFile:
    def test_has_live_rows(self):
        assert len(ROWS) > 100

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
                {"request": GET_PAGE, "pre": [{"release_origin": {}}], "expect": {"status": 200}},
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
                {"request": GET_PAGE, "pre": [{"release_origin": {}}], "expect": {"status": 200}},
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


class TestHeaderHelpers:
    def test_vary_without_accept_takes_one_token(self):
        lines = [("Vary", "Accept-Encoding, Accept"), ("X", "1"), ("Vary", "Accept")]
        assert conf._vary_without_accept(lines) == [
            ("Vary", "Accept-Encoding"), ("X", "1"), ("Vary", "Accept")]
        assert conf._vary_without_accept([("Vary", "Accept")]) == []


if __name__ == "__main__":
    raise SystemExit(pytest.main([__file__, "-v"]))
