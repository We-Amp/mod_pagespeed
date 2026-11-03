#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2026 We-Amp B.V.

"""DaemonHtmlCache report-only against a live server.

The lane serves two name-based virtual hosts that differ only in that
directive (lane fixture "daemon_html_report", on the port named by
PAGESPEED_RULE_PACKS_PORT; both have the optimizer daemon configured):

  daemon-html-off.example.com     DaemonHtmlCache off
  daemon-html-report.example.com  DaemonHtmlCache report-only

Report-only must leave every response byte-identical, its headers included, and
store nothing; only the counters and the decision log may differ. An optimizer
older than the policy reports "unavailable" instead, which is what the lane's
pinned optimizer does until the pin reaches it.
"""

import email.utils
import glob
import json
import os
import subprocess
import time
from typing import Dict, List, Tuple

import pytest

from pagespeed_test_framework import VhostClient

OFF_HOST = "daemon-html-off.example.com"
REPORT_HOST = "daemon-html-report.example.com"
FIXTURE_DIR = "/daemon_html"

# Headers each response legitimately has its own value of.
VOLATILE_HEADERS = {"date"}

PAGES = [
    f"{FIXTURE_DIR}/plain.html",
    f"{FIXTURE_DIR}/personal.html",
    # Larger than the scan window, so the capture's bound is on the path.
    f"{FIXTURE_DIR}/large.html",
    f"{FIXTURE_DIR}/addheader.html",
    f"{FIXTURE_DIR}/private.html",
    f"{FIXTURE_DIR}/setcookie.html",
    f"{FIXTURE_DIR}/varycookie.html",
    f"{FIXTURE_DIR}/plain.html?with=query",
    f"{FIXTURE_DIR}/absent.html",
    f"{FIXTURE_DIR}/upstream404.html",
    "/mod_pagespeed_example/index.html",
    "/mod_pagespeed_example/collapse_whitespace.html",
    "/mod_pagespeed_example/combine_css.html",
    "/mod_pagespeed_example/combine_javascript.html",
    "/mod_pagespeed_example/extend_cache.html",
    "/mod_pagespeed_example/inline_css.html",
    "/mod_pagespeed_example/inline_javascript.html",
    "/mod_pagespeed_example/remove_comments.html",
    "/mod_pagespeed_example/rewrite_css.html",
    "/mod_pagespeed_example/rewrite_images.html",
    "/mod_pagespeed_example/rewrite_javascript.html",
]


def _site(host: str) -> VhostClient:
    return VhostClient(host, "localhost", int(os.environ["PAGESPEED_RULE_PACKS_PORT"]))


# Validators no page here is newer than: a conditional GET may be answered 304.
NOT_MODIFIED_SINCE = email.utils.formatdate(time.time(), usegmt=True)

# Method, extra request headers, id.
REQUESTS = [
    ("GET", {"Accept-Encoding": "identity"}, "identity"),
    ("GET", {"Accept-Encoding": "gzip"}, "gzip"),
    ("GET", {"Accept-Encoding": "identity", "Cookie": "a=b"}, "cookie"),
    ("HEAD", {"Accept-Encoding": "identity"}, "head"),
    ("GET", {"Accept-Encoding": "identity", "If-Modified-Since": NOT_MODIFIED_SINCE}, "if-modified-since"),
    ("GET", {"Accept-Encoding": "identity", "Range": "bytes=0-99"}, "range"),
]


def _snapshot(
    site: VhostClient, method: str, path: str, headers: Dict[str, str]
) -> Tuple[int, bytes, List[Tuple[str, str]]]:
    """Status, body and header lines with the host name taken out."""
    host = site.vhost
    response = site.head(path, headers=headers) if method == "HEAD" else site.get(path, headers=headers)
    body = response.body.replace(host.encode(), b"HOST")
    # A page that names the host (the server's own error page) is longer by the
    # host names it holds; its length is compared without them.
    shrink = len(response.body) - len(body)
    lines = []
    # Every line as received, in order: report-only may not reorder or repeat one.
    for name, value in response.raw_headers:
        if name.lower() in VOLATILE_HEADERS:
            continue
        if name.lower() == "content-length" and value.strip().isdigit():
            if method == "HEAD" and response.status != 200:
                # No body to measure the host names in.
                continue
            value = str(int(value) - shrink)
        lines.append((name, value.replace(host, "HOST")))
    return response.status, body, lines


_STATS_ARGS: Dict[str, object] = {}


@pytest.fixture(autouse=True)
def _stats_args(server_config):
    if not server_config.stats_enabled:
        pytest.skip("Statistics not enabled (set PAGESPEED_STATS_ENABLED=1)")
    _STATS_ARGS["stats_path"] = server_config.stats_path
    # nginx admin endpoints do not work with ?PageSpeed=off.
    _STATS_ARGS["disable_pagespeed"] = not server_config.is_nginx


def _stats(site: VhostClient) -> Dict[str, int]:
    return {
        name: value
        for name, value in site.get_statistics(**_STATS_ARGS).items()
        if name.startswith("daemon_html_")
    }


def _delta(before: Dict[str, int], after: Dict[str, int], name: str) -> int:
    return after.get(name, 0) - before.get(name, 0)


def _policy_available() -> bool:
    site = _site(REPORT_HOST)
    before = _stats(site)
    site.get(f"{FIXTURE_DIR}/plain.html?probe={time.time()}")
    return _delta(before, _stats(site), "daemon_html_unavailable") == 0


def _decision_lines(host: str) -> List[dict]:
    log_dir = os.environ.get("PAGESPEED_RULE_PACK_LOG_DIR")
    if not log_dir:
        pytest.fail("PAGESPEED_RULE_PACK_LOG_DIR is not set")
    lines: List[dict] = []
    pattern = os.path.join(log_dir, "daemon-html", "decisions-*.jsonl")
    names = glob.glob(pattern)
    if not names:
        # The directory is the server's; a runner may not be able to list it.
        listed = subprocess.run(
            ["sudo", "-n", "sh", "-c", f"ls {pattern} 2>/dev/null"], capture_output=True, text=True
        )
        names = listed.stdout.split()
    for name in sorted(names):
        try:
            with open(name, encoding="utf-8") as f:
                text = f.read()
        except PermissionError:
            try:
                text = subprocess.run(
                    ["sudo", "-n", "cat", name], check=True, capture_output=True, text=True
                ).stdout
            except (OSError, subprocess.CalledProcessError):
                pytest.skip(f"decision log {name} is not readable by the test runner")
        for raw in text.splitlines():
            entry = json.loads(raw)  # every line must parse
            if entry.get("host") == host:
                lines.append(entry)
    return lines


def _wait_for_line(host: str, path: str) -> dict:
    deadline = time.time() + 5
    while True:
        for entry in _decision_lines(host):
            if entry.get("path") == path:
                return entry
        if time.time() > deadline:
            pytest.fail(f"no decision line for {host}{path}")
        time.sleep(0.1)


@pytest.mark.requires_fixture("daemon_html_report")
class TestReportOnlyIsInert:
    @pytest.mark.parametrize("path", PAGES)
    @pytest.mark.parametrize(
        "method,headers",
        [(method, headers) for method, headers, _ in REQUESTS],
        ids=[request_id for _, _, request_id in REQUESTS],
    )
    def test_off_and_report_only_are_byte_identical(self, path: str, method: str, headers: Dict[str, str]):
        request = dict(headers)
        request["X-PSA-Blocking-Rewrite"] = "psatest"
        off = _site(OFF_HOST)
        report = _site(REPORT_HOST)
        # The two hosts have separate cache entries, so both are given time to
        # settle on the same rewrites; a real difference never settles.
        for _ in range(10):
            left = _snapshot(off, method, path, request)
            right = _snapshot(report, method, path, request)
            if left == right:
                break
            time.sleep(0.5)
        assert left[0] == right[0], f"{path}: status differs"
        assert left[2] == right[2], f"{path}: headers differ"
        assert left[1] == right[1], f"{path}: body differs"

    def test_the_off_host_reports_nothing(self):
        site = _site(OFF_HOST)
        before = _stats(site)
        site.get(f"{FIXTURE_DIR}/plain.html")
        after = _stats(site)
        for name in ("daemon_html_evaluated", "daemon_html_unavailable"):
            assert _delta(before, after, name) == 0, name
        assert _decision_lines(OFF_HOST) == []

    # Only a 200 that carries a body is evaluated, on both servers: not a HEAD,
    # not the server's own 404, and not a 404 HTML page passed on from an
    # upstream (nginx proxies upstream404.html to the off host's 404; on
    # Apache it is a plain 404). Neither counter moves, with or without the
    # policy.
    @pytest.mark.parametrize(
        "method,page",
        [
            ("HEAD", "plain.html"),
            ("GET", "absent.html"),
            ("GET", "upstream404.html"),
        ],
    )
    def test_only_a_200_with_a_body_is_evaluated(self, method: str, page: str):
        site = _site(REPORT_HOST)
        path = f"{FIXTURE_DIR}/{page}"
        before = _stats(site)
        response = site.head(path) if method == "HEAD" else site.get(path)
        assert response.status == (200 if method == "HEAD" else 404)
        after = _stats(site)
        for name in ("daemon_html_evaluated", "daemon_html_unavailable"):
            assert _delta(before, after, name) == 0, name


@pytest.mark.requires_fixture("daemon_html_report")
class TestDecisions:
    @pytest.fixture(autouse=True)
    def _needs_policy(self):
        if not _policy_available():
            pytest.skip("the installed optimizer library predates the HTML cache policy")

    def test_a_plain_page_would_store(self):
        site = _site(REPORT_HOST)
        before = _stats(site)
        path = f"{FIXTURE_DIR}/plain.html"
        assert site.get(path).status == 200
        after = _stats(site)
        assert _delta(before, after, "daemon_html_evaluated") == 1
        assert _delta(before, after, "daemon_html_would_store") == 1, after
        assert _delta(before, after, "daemon_html_classify_failed") == 0
        entry = _wait_for_line(REPORT_HOST, path)
        assert entry["mode"] == "report-only"
        assert entry["action"] == "would_store"
        assert "reason" not in entry

    def test_the_lifetime_from_the_expires_stage_is_seen(self):
        site = _site(REPORT_HOST)
        path = f"{FIXTURE_DIR}/expires.html"
        assert site.get(path).status == 200
        entry = _wait_for_line(REPORT_HOST, path)
        assert entry["action"] == "would_store"
        assert entry["lifetime_s"] == 600

    def test_a_personalisation_match_is_a_refusal(self):
        site = _site(REPORT_HOST)
        before = _stats(site)
        path = f"{FIXTURE_DIR}/personal.html"
        assert site.get(path).status == 200
        after = _stats(site)
        assert _delta(before, after, "daemon_html_would_store") == 0
        assert _delta(before, after, "daemon_html_refused_personalisation_signal") == 1
        entry = _wait_for_line(REPORT_HOST, path)
        assert entry["action"] == "refuse"
        assert entry["reason"] == "personalisation_signal"
        assert entry["personalisation"] == "meta_csrf_token"

    def test_a_cookie_request_is_refused(self):
        site = _site(REPORT_HOST)
        before = _stats(site)
        assert site.get(f"{FIXTURE_DIR}/cookie.html", headers={"Cookie": "a=b"}).status == 200
        after = _stats(site)
        assert _delta(before, after, "daemon_html_refused_cookie_request") == 1
        entry = _wait_for_line(REPORT_HOST, f"{FIXTURE_DIR}/cookie.html")
        assert entry["request"] == "cookie"

    def test_a_query_is_refused_and_never_logged(self):
        site = _site(REPORT_HOST)
        before = _stats(site)
        path = f"{FIXTURE_DIR}/query.html"
        assert site.get(f"{path}?token=do-not-log-this").status == 200
        after = _stats(site)
        assert _delta(before, after, "daemon_html_refused_url") == 1
        entry = _wait_for_line(REPORT_HOST, path)
        assert entry["reason"] == "url"
        assert "do-not-log-this" not in json.dumps(_decision_lines(REPORT_HOST))

    def test_a_large_body_is_scanned_only_in_its_window(self):
        site = _site(REPORT_HOST)
        path = f"{FIXTURE_DIR}/large.html"
        response = site.get(path)
        assert response.status == 200
        entry = _wait_for_line(REPORT_HOST, path)
        # The marker past the window is not seen.
        assert entry["action"] == "would_store", entry
        assert entry["size"] > 64 * 1024

    def test_no_value_reaches_the_log(self):
        site = _site(REPORT_HOST)
        site.get(f"{FIXTURE_DIR}/secret.html", headers={"Cookie": "session=do-not-log-this"})
        _wait_for_line(REPORT_HOST, f"{FIXTURE_DIR}/secret.html")
        assert "do-not-log-this" not in json.dumps(_decision_lines(REPORT_HOST))

    # A line the server's header stage adds (nginx add_header, Apache
    # mod_headers) is part of what the policy reads on both servers.
    @pytest.mark.parametrize(
        "page,reason",
        [
            ("addheader.html", "headers:x-lane-added"),
            ("private.html", "private"),
            ("setcookie.html", "set_cookie"),
            # Apache's late read compares Vary as a set of tokens, so a token
            # mod_headers appends counts there as it does on nginx.
            ("varycookie.html", "vary:cookie"),
        ],
    )
    def test_a_header_added_by_the_server(self, page: str, reason: str):
        site = _site(REPORT_HOST)
        path = f"{FIXTURE_DIR}/{page}"
        response = site.get(path)
        assert response.status == 200
        entry = _wait_for_line(REPORT_HOST, path)
        assert entry["action"] == "refuse", entry
        assert entry["reason"] == reason, entry


if __name__ == "__main__":
    raise SystemExit(pytest.main([__file__, "-v"]))
