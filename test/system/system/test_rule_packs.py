#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2026 We-Amp B.V.

"""The rule_packs filter against a live server.

A pack (install/mod_pagespeed_test/rule_packs/pack.json) lists two hosts. The
lane serves them as name-based virtual hosts on the port named by
PAGESPEED_RULE_PACKS_PORT (lane fixture "rule_packs"):

  rulepacks.example.com          rule_packs on, RulePackMode enforce
  rulepacks-report.example.com   rule_packs on, RulePackMode report
  rulepacks-unlisted.example.com rule_packs on, host not in the pack

Several packs on one host (lane fixture "rule_pack_multi", multi/a.json and
multi/b.json under the same directory):

  rulepacks-multi.example.com      two RulePackFile lines, a.json then b.json
  rulepacks-multi-dup.example.com  a.json, then dup.json with a's pack id

Where the lane runs the chunked slow-flushing origin (lane fixture
"flush_origin") the enforce host also serves it, to cut a page's head in two
flush windows. Where the lane lets the test runner read the server's
decision log (lane fixture "rule_pack_log", PAGESPEED_RULE_PACK_LOG_DIR),
the log lines are checked too.

The filter works on the response as the origin sent it, and HTML is not
cached, so every case checks the first response.
"""

import glob
import http.client
import json
import os
import re
import subprocess
import time
import uuid
from typing import Callable, Dict, List

import pytest

from pagespeed_test_framework import (
    PageSpeedClient,
    VhostClient,
    assert_stat_delta,
    require_status_ok,
)

PACK_DIR = "/mod_pagespeed_test/rule_packs"
DAEMON_CSS_PATH = "/daemon_lane/lane.css"
# How long the optimizer worker gets to optimize a stylesheet after it was recorded.
WORKER_DEADLINE_S = 60.0
ENFORCE_HOST = "rulepacks.example.com"
REPORT_HOST = "rulepacks-report.example.com"
UNLISTED_HOST = "rulepacks-unlisted.example.com"
# A plain virtual host (no rule packs) with in-place daemon serving on.
DAEMON_HOST = "daemon.example.com"
DAEMON_HTML_PATH = "/daemon_lane/lane.html"
SIGNED_HOST = "rulepacks-signed.example.com"
REQUIRE_HOST = "rulepacks-require.example.com"
MULTI_HOST = "rulepacks-multi.example.com"
MULTI_DUP_HOST = "rulepacks-multi-dup.example.com"
FILTER_OFF = "PageSpeedFilters=-rule_packs"

CANONICAL_RE = re.compile(r'<link rel="canonical" href="([^"]*)">')
TITLE_RE = re.compile(r"<title>([^<]*)</title>")
DESCRIPTION_RE = re.compile(r'<meta name="description" content="([^"]*)">')


def _run_id() -> str:
    """A query value no earlier request used (decision lines are deduplicated
    per process by host, path, rule and outcome)."""
    return uuid.uuid4().hex[:12]


@pytest.fixture
def pack_site(server_config) -> Callable[[str], VhostClient]:
    """Factory: a client for one named virtual host of the rule-pack lane."""
    raw = os.environ.get("PAGESPEED_RULE_PACKS_PORT")
    if not raw:
        pytest.fail("lane fixture rule_packs is provided but PAGESPEED_RULE_PACKS_PORT is not set")

    def _make(vhost: str) -> VhostClient:
        return VhostClient(vhost, proxy_host=server_config.host, proxy_port=int(raw))

    return _make


@pytest.fixture
def pack_stats(server_config, pack_site) -> Callable[[str], Dict[str, int]]:
    """Factory: a snapshot of the statistics of one virtual host of the pack lane.

    Read through the host the page was served by: where statistics are kept
    per virtual host (nginx), the counters of another host do not move.
    """
    if not server_config.stats_enabled:
        pytest.skip("Statistics not enabled (set PAGESPEED_STATS_ENABLED=1)")
    # nginx and Envoy admin endpoints do not work with ?PageSpeed=off.
    disable_pagespeed = not (server_config.is_nginx or server_config.server_type == "envoy")

    def _snapshot(host: str) -> Dict[str, int]:
        return pack_site(host).get_statistics(
            stats_path=server_config.stats_path, disable_pagespeed=disable_pagespeed
        )

    return _snapshot


def _fetch(site: VhostClient, path: str, query: str = "") -> str:
    """The body of `path` on `site`; 200 is required."""
    separator = "&" if "?" in path else "?"
    url = f"{path}{separator}{query}" if query else path
    response = site.get(url)
    require_status_ok(response, f"{site.vhost}{url}")
    return response.text


def _serve_css_until_daemon_served(site, pack_stats, host, counter="ipro_daemon_served"):
    """Fetch the lane's stylesheet on `host` until the daemon has served it.

    The worker needs a moment to optimize what the module recorded, so the
    first responses come from the module. Returns the response that the
    daemon served; fails when none was within WORKER_DEADLINE_S.
    """
    before = pack_stats(host)
    deadline = time.time() + WORKER_DEADLINE_S
    while time.time() < deadline:
        response = site.get(DAEMON_CSS_PATH)
        require_status_ok(response, f"{host}{DAEMON_CSS_PATH}")
        if pack_stats(host).get(counter, 0) - before.get(counter, 0) >= 1:
            return response
        time.sleep(1.0)
    pytest.fail(f"no stylesheet was served by the daemon ({counter}) within {WORKER_DEADLINE_S}s")


def _canonicals(html: str) -> List[str]:
    return CANONICAL_RE.findall(html)


def _link_entries(value: str) -> List[str]:
    """The link-values of a Link header field value (RFC 8288): a comma
    separates them only outside <...> and outside a quoted string."""
    entries: List[str] = []
    current: List[str] = []
    in_angle = in_quote = False
    chars = iter(value)
    for ch in chars:
        if in_quote:
            current.append(ch)
            if ch == "\\":
                current.append(next(chars, ""))
            elif ch == '"':
                in_quote = False
        elif in_angle:
            current.append(ch)
            in_angle = ch != ">"
        elif ch == ",":
            entries.append("".join(current).strip())
            current = []
        else:
            current.append(ch)
            in_quote = ch == '"'
            in_angle = ch == "<"
    entries.append("".join(current).strip())
    return [e for e in entries if e]


def _link_targets(entries: List[str], rel: str) -> List[str]:
    """The targets of the link-values whose rel parameter has the token `rel`."""
    targets = []
    for entry in entries:
        match = re.match(r"<([^>]*)>(.*)$", entry)
        if match is None:
            continue
        rel_match = re.search(r'(?:^|;)\s*rel\s*=\s*(?:"([^"]*)"|([^;\s]+))', match.group(2), re.I)
        if rel_match is not None:
            tokens = (rel_match.group(1) or rel_match.group(2)).lower().split()
            if rel in tokens:
                targets.append(match.group(1))
    return targets


def _link_header(site: VhostClient, path: str, query: str = "") -> List[str]:
    """The link-values of every Link header of `path` on `site`."""
    separator = "&" if "?" in path else "?"
    url = f"{path}{separator}{query}" if query else path
    response = site.get(url)
    require_status_ok(response, f"{site.vhost}{url}")
    # (requests joins repeated headers with ", ".)
    return _link_entries(response.headers.get("Link", ""))


def _decision_lines(host: str, path_prefix: str) -> List[dict]:
    """The decision-log lines of `host` whose path starts with `path_prefix`."""
    log_dir = os.environ.get("PAGESPEED_RULE_PACK_LOG_DIR")
    if not log_dir:
        pytest.fail("lane fixture rule_pack_log is provided but PAGESPEED_RULE_PACK_LOG_DIR is not set")
    lines: List[dict] = []
    for name in sorted(glob.glob(os.path.join(log_dir, "packs", "*", "decisions-*.jsonl"))):
        try:
            with open(name, encoding="utf-8") as f:
                text = f.read()
        except PermissionError:
            # The server writes the log with mode 0640 as its own user.
            try:
                text = subprocess.run(
                    ["sudo", "-n", "cat", name], check=True, capture_output=True, text=True
                ).stdout
            except (OSError, subprocess.CalledProcessError):
                pytest.skip(f"decision log {name} is not readable by the test runner")
        for raw in text.splitlines():
            entry = json.loads(raw)  # every line must parse
            if entry.get("host") == host and entry.get("path", "").startswith(path_prefix):
                lines.append(entry)
    return lines


def _wait_for_decision_lines(host: str, path_prefix: str, timeout_s: float = 10.0) -> List[dict]:
    """The log is written as decisions are made; poll briefly for slow disks."""
    deadline = time.time() + timeout_s
    while True:
        lines = _decision_lines(host, path_prefix)
        if lines or time.time() >= deadline:
            return lines
        time.sleep(0.2)


@pytest.mark.requires_fixture("rule_packs")
class TestEnforce:
    """The enforce host puts the pack's head elements in the first response."""

    def test_missing_head_elements_are_inserted(self, pack_site, pack_stats):
        site = pack_site(ENFORCE_HOST)
        path = f"{PACK_DIR}/missing.html"
        before = pack_stats(ENFORCE_HOST)
        html = _fetch(site, path, f"run={_run_id()}")
        after = pack_stats(ENFORCE_HOST)

        assert _canonicals(html) == [f"https://{ENFORCE_HOST}{path}"], html
        assert TITLE_RE.findall(html) == [f"Title for {path}"], html
        assert DESCRIPTION_RE.findall(html) == [f"Description for {path}"], html
        # The inserted elements are in the head.
        assert html.index('rel="canonical"') < html.index("</head>"), html
        assert_stat_delta(before, after, "rule_pack_pages_evaluated", 1)
        assert_stat_delta(before, after, "rule_pack_pages_modified", 1)
        assert_stat_delta(before, after, "rule_pack_decisions_applied", 3)
        assert_stat_delta(before, after, "rule_pack_decisions_would_apply", 0)

    def test_a_page_with_everything_is_byte_identical_to_the_unfiltered_page(
        self, pack_site, pack_stats
    ):
        site = pack_site(ENFORCE_HOST)
        path = f"{PACK_DIR}/present.html"
        before = pack_stats(ENFORCE_HOST)
        filtered = _fetch(site, path)
        after = pack_stats(ENFORCE_HOST)
        unfiltered = _fetch(site, path, FILTER_OFF)

        assert filtered == unfiltered
        assert len(_canonicals(filtered)) == 1
        assert_stat_delta(before, after, "rule_pack_pages_evaluated", 1)
        assert_stat_delta(before, after, "rule_pack_pages_modified", 0)
        assert_stat_delta(before, after, "rule_pack_decisions_applied", 0)

    def test_a_canonical_only_in_the_body_is_replaced_by_one_in_the_head(
        self, pack_site, pack_stats
    ):
        # The head is decided when it closes, before the body canonical is
        # seen: the pack inserts its own, and removes the late one rather
        # than leave the page with two.
        site = pack_site(ENFORCE_HOST)
        path = f"{PACK_DIR}/body-canonical.html"
        before = pack_stats(ENFORCE_HOST)
        html = _fetch(site, path, f"run={_run_id()}")
        after = pack_stats(ENFORCE_HOST)

        assert _canonicals(html) == [f"https://{ENFORCE_HOST}{path}"], html
        assert "elsewhere" not in html, html
        assert html.index('rel="canonical"') < html.index("</head>"), html
        assert_stat_delta(before, after, "rule_pack_late_removed", 1)
        assert_stat_delta(before, after, "rule_pack_pages_modified", 1)

    def test_debug_names_the_rule_behind_each_insert(self, pack_site):
        site = pack_site(ENFORCE_HOST)
        html = _fetch(site, f"{PACK_DIR}/missing.html", f"run={_run_id()}&PageSpeedFilters=%2Bdebug")
        assert (
            "<!-- pagespeed-pack edge-seo@0.1.0 rule=canonical-keep action=insert mode=enforce -->"
            in html
        ), html

    @pytest.mark.requires_fixture("rule_pack_log")
    def test_the_decision_log_names_what_the_pack_did(self, pack_site):
        site = pack_site(ENFORCE_HOST)
        run = _run_id()
        path = f"{PACK_DIR}/missing.html"
        _fetch(site, path, f"run={run}")
        lines = _wait_for_decision_lines(ENFORCE_HOST, f"{path}?run={run}")

        canonical = [l for l in lines if l["rule"] == "canonical-keep"]
        assert len(canonical) == 1, lines
        entry = canonical[0]
        assert entry["pack"] == "edge-seo@0.1.0", entry
        assert entry["kind"] == "canonical", entry
        assert entry["mode"] == "enforce", entry
        assert entry["action"] == "insert", entry
        assert entry["after"] == f"https://{ENFORCE_HOST}{path}", entry


@pytest.mark.requires_fixture("rule_packs", "flush_origin", "rule_pack_no_link_header")
class TestEarlyFlushWithoutLinkHeaderRule:
    """A pack with no link_header rule never edits the response headers, so an
    origin's early flush of them is passed on at once."""

    NOLINK_HOST = "rulepacks-nolink.example.com"
    ROUTE = "/mod_pagespeed_test/flush_origin/link_header_late_body"

    def test_headers_go_out_at_the_flush(self, server_config):
        # The origin writes its headers and flushes, then sends the body
        # 0.3 s later. The lane runs a pool of server processes, each of which
        # holds its first response until it has read the pack, so try until
        # one answers fast.
        port = int(os.environ["PAGESPEED_RULE_PACKS_PORT"])
        header_waits = []
        for _ in range(48):
            conn = http.client.HTTPConnection(server_config.host, port, timeout=30)
            try:
                started = time.time()
                conn.request("GET", f"{self.ROUTE}?run={_run_id()}", headers={"Host": self.NOLINK_HOST})
                response = conn.getresponse()
                header_waits.append(time.time() - started)
                assert response.status == 200
                response.read()
            finally:
                conn.close()
            if header_waits[-1] < 0.25:
                break
        assert header_waits[-1] < 0.25, header_waits


@pytest.mark.requires_fixture("rule_packs", "flush_origin")
class TestLinkHeader:
    """link_header: match keeps the Link canonical header in line with the
    canonical the rule decides on, and only changes what conflicts."""

    ROUTE = "/mod_pagespeed_test/flush_origin/link_header"
    PRELOAD = "</mod_pagespeed_test/rule_packs/app.css>; rel=preload; as=style"
    PRECONNECT = "<https://cdn.example.net>; rel=preconnect"

    def test_a_conflicting_canonical_is_replaced_and_the_other_entries_survive(
        self, pack_site, pack_stats
    ):
        site = pack_site(ENFORCE_HOST)
        before = pack_stats(ENFORCE_HOST)
        entries = _link_header(site, self.ROUTE, f"run={_run_id()}")
        after = pack_stats(ENFORCE_HOST)

        assert _link_targets(entries, "canonical") == [f"https://{ENFORCE_HOST}{self.ROUTE}"], entries
        assert self.PRELOAD in entries, entries
        assert self.PRECONNECT in entries, entries
        assert not any("wrong.example.com" in e for e in entries), entries
        assert_stat_delta(before, after, "rule_pack_headers_set", 1)
        assert_stat_delta(before, after, "rule_pack_headers_flushed", 0)

    def test_a_flush_before_the_first_body_byte_does_not_send_the_origin_header(
        self, pack_site, pack_stats
    ):
        # The origin writes its headers, then the body 0.3 s later. The proxy
        # relays the headers with a flush at once; that flush reaches the
        # module before it has parsed anything and used to send the headers
        # as the origin wrote them, ahead of the repair (the counter still
        # said the header was set).
        site = pack_site(ENFORCE_HOST)
        route = f"{self.ROUTE}_late_body"
        for _ in range(3):
            before = pack_stats(ENFORCE_HOST)
            entries = _link_header(site, route, f"run={_run_id()}")
            after = pack_stats(ENFORCE_HOST)

            assert _link_targets(entries, "canonical") == [f"https://{ENFORCE_HOST}{route}"], entries
            assert self.PRELOAD in entries, entries
            assert self.PRECONNECT in entries, entries
            assert_stat_delta(before, after, "rule_pack_headers_set", 1)
            assert_stat_delta(before, after, "rule_pack_headers_flushed", 0)

    def test_a_body_that_is_not_html_still_streams_on_a_pack_host(self, server_config):
        # The hold on an early flush ends once the body is known: a text/html
        # response that turns out not to be HTML must not wait for its end.
        conn = http.client.HTTPConnection(
            server_config.host, int(os.environ["PAGESPEED_RULE_PACKS_PORT"]), timeout=30
        )
        try:
            started = time.time()
            conn.request(
                "GET",
                f"/mod_pagespeed_test/flush_origin/plain_text_html?run={_run_id()}",
                headers={"Host": ENFORCE_HOST},
            )
            response = conn.getresponse()
            assert response.status == 200
            first = response.read1(65536)
            first_at = time.time() - started
            rest = response.read()
            total = time.time() - started
        finally:
            conn.close()
        assert first.startswith(b"plain text chunk 0"), first
        assert rest.endswith(b"plain text chunk 2\n"), rest
        assert total - first_at >= 0.5, (first_at, total)

    def test_an_early_flush_and_an_empty_body_keep_the_origin_headers(self, pack_site):
        site = pack_site(ENFORCE_HOST)
        response = site.get(
            f"/mod_pagespeed_test/flush_origin/early_flush_empty_body?run={_run_id()}"
        )
        require_status_ok(response, "early_flush_empty_body")
        assert response.text == "", response.text
        assert response.headers.get("X-Origin-Header") == "kept", response.headers
        assert "origin_cookie=1" in response.headers.get("Set-Cookie", ""), response.headers
        assert self.PRELOAD in _link_entries(response.headers.get("Link", "")), response.headers

    def test_the_document_and_the_header_agree(self, pack_site):
        site = pack_site(ENFORCE_HOST)
        run = _run_id()
        entries = _link_header(site, self.ROUTE, f"run={run}")
        html = _fetch(site, self.ROUTE, f"run={run}")
        assert _canonicals(html) == _link_targets(entries, "canonical"), (entries, html)

    def test_apache_head_carries_the_origin_link_header_not_the_edited_one(
        self, pack_site, server_config
    ):
        # A pack edits the Link header while it parses the HTML body, and a
        # HEAD response has no body to parse. So on Apache a HEAD keeps the
        # origin's Link header while the GET of the same URL gets the edited
        # one (docs/rule-packs.md, "HEAD requests on Apache").
        if server_config.server_type != "apache":
            pytest.skip("the HEAD behaviour pinned here is the Apache one")
        site = pack_site(ENFORCE_HOST)
        run = _run_id()
        url = f"{self.ROUTE}?run={run}"
        get = site.get(url)
        require_status_ok(get, f"GET {ENFORCE_HOST}{url}")
        head = site.head(url)
        require_status_ok(head, f"HEAD {ENFORCE_HOST}{url}")

        get_entries = _link_entries(get.headers.get("Link", ""))
        head_entries = _link_entries(head.headers.get("Link", ""))
        assert _link_targets(get_entries, "canonical") == [f"https://{ENFORCE_HOST}{self.ROUTE}"], get_entries
        assert _link_targets(head_entries, "canonical") == ["https://wrong.example.com/stale"], head_entries
        # The other entries are the origin's on both.
        assert self.PRELOAD in head_entries and self.PRECONNECT in head_entries, head_entries
        assert self.PRELOAD in get_entries and self.PRECONNECT in get_entries, get_entries

        # Observed: the module removes the origin's Content-Length on HTML it
        # rewrites, so the HEAD reply carries none (not "0"); the GET reply
        # carries the length of the body it sends.
        assert head.headers.get("Content-Length") is None, head.headers
        assert get.headers.get("Content-Length") == str(len(get.text.encode())), get.headers

    def test_the_report_host_leaves_the_header_as_the_origin_sent_it(self, pack_site, pack_stats):
        site = pack_site(REPORT_HOST)
        before = pack_stats(REPORT_HOST)
        filtered = _link_header(site, self.ROUTE, f"run={_run_id()}")
        after = pack_stats(REPORT_HOST)
        unfiltered = _link_header(site, self.ROUTE, FILTER_OFF)

        assert filtered == unfiltered
        assert _link_targets(filtered, "canonical") == ["https://wrong.example.com/stale"], filtered
        assert_stat_delta(before, after, "rule_pack_headers_set", 0)
        assert_stat_delta(before, after, "rule_pack_decisions_would_apply", 2)

    @pytest.mark.requires_fixture("rule_pack_log")
    def test_the_decision_log_has_a_line_for_the_header(self, pack_site):
        site = pack_site(ENFORCE_HOST)
        run = _run_id()
        _link_header(site, self.ROUTE, f"run={run}")
        lines = _wait_for_decision_lines(ENFORCE_HOST, f"{self.ROUTE}?run={run}")

        header = [l for l in lines if l["rule"] == "canonical-link-repair" and l.get("header")]
        assert len(header) == 1, lines
        assert header[0]["action"] == "replace", header[0]
        assert header[0]["reason"] == "header_conflict", header[0]
        assert header[0]["defect"] == "canonical-header-conflict", header[0]
        assert header[0]["after"] == f"https://{ENFORCE_HOST}{self.ROUTE}", header[0]
        # The document has its own line for the same rule.
        assert [l for l in lines if l["rule"] == "canonical-link-repair" and not l.get("header")], lines


@pytest.mark.requires_fixture("rule_packs")
class TestReport:
    """The report host leaves every page alone and says what it would do."""

    def test_output_is_byte_identical_to_the_unfiltered_page(self, pack_site, pack_stats):
        site = pack_site(REPORT_HOST)
        path = f"{PACK_DIR}/missing.html"
        before = pack_stats(REPORT_HOST)
        filtered = _fetch(site, path, f"run={_run_id()}")
        after = pack_stats(REPORT_HOST)
        unfiltered = _fetch(site, path, FILTER_OFF)

        assert filtered == unfiltered
        assert _canonicals(filtered) == []
        assert_stat_delta(before, after, "rule_pack_pages_modified", 0)
        assert_stat_delta(before, after, "rule_pack_pages_would_modify", 1)
        assert_stat_delta(before, after, "rule_pack_decisions_applied", 0)
        assert_stat_delta(before, after, "rule_pack_decisions_would_apply", 3)

    @pytest.mark.requires_fixture("rule_pack_log")
    def test_the_decision_log_records_the_decision_as_report(self, pack_site):
        site = pack_site(REPORT_HOST)
        run = _run_id()
        path = f"{PACK_DIR}/missing.html"
        _fetch(site, path, f"run={run}")
        lines = _wait_for_decision_lines(REPORT_HOST, f"{path}?run={run}")

        canonical = [l for l in lines if l["rule"] == "canonical-keep"]
        assert len(canonical) == 1, lines
        assert canonical[0]["mode"] == "report", canonical[0]
        assert canonical[0]["action"] == "insert", canonical[0]

    def test_debug_says_what_would_change(self, pack_site):
        site = pack_site(REPORT_HOST)
        html = _fetch(site, f"{PACK_DIR}/missing.html", f"run={_run_id()}&PageSpeedFilters=%2Bdebug")
        assert (
            "<!-- pagespeed-pack edge-seo@0.1.0 rule=canonical-keep would=insert mode=report -->"
            in html
        ), html
        assert _canonicals(html) == []


@pytest.mark.requires_fixture("rule_packs", "rule_pack_log")
class TestAdminPage:
    """The read-only Rule packs page of the admin site shows what the pack
    decided, on the virtual host the request hit.

    The decision log is shared by all server processes, so whichever process
    answers shows the row: one fetch each, no retry.
    """

    def _admin_url(self, server_config, query: str) -> str:
        params = [] if (server_config.is_nginx or server_config.server_type == "envoy") else ["PageSpeed=off"]
        params.append(query)
        return f"{server_config.admin_path}/rule_packs?" + "&".join(params)

    def test_the_page_shows_the_decision(self, pack_site, server_config):
        site = pack_site(REPORT_HOST)
        run = _run_id()
        path = f"{PACK_DIR}/missing.html"
        _fetch(site, path, f"run={run}")
        _wait_for_decision_lines(REPORT_HOST, f"{path}?run={run}")

        response = site.get(self._admin_url(server_config, "rule=canonical-keep&limit=1000"))
        require_status_ok(response, "rule_packs page")
        html = response.text
        assert "<h1>Rule packs</h1>" in html, html
        assert "edge-seo@0.1.0" in html, html
        assert run in html, html
        assert "canonical-keep" in html, html
        assert "report" in html, html

        filtered = site.get(self._admin_url(server_config, "rule=no-such-rule"))
        require_status_ok(filtered, "rule_packs page, filtered")
        assert run not in filtered.text, filtered.text

    def test_the_raw_form_is_plain_text(self, pack_site, server_config):
        site = pack_site(REPORT_HOST)
        run = _run_id()
        path = f"{PACK_DIR}/missing.html"
        _fetch(site, path, f"run={run}")
        _wait_for_decision_lines(REPORT_HOST, f"{path}?run={run}")

        response = site.get(self._admin_url(server_config, "raw=1&rule=canonical-keep&limit=1000"))
        require_status_ok(response, "rule_packs raw")
        assert response.headers.get("Content-Type", "").startswith("text/plain"), response.headers
        assert run in response.text, response.text


@pytest.mark.requires_fixture("rule_packs", "rule_pack_signing")
class TestSignedPacks:
    """RulePackRequireSigned on: a pack signed with the trusted key is used,
    an unsigned one is refused and the filter is off for the host.

    Provided by the Apache and nginx lanes (a server block or virtual host per
    host). The IIS lane does not provide rule_pack_signing: RulePackTrustKey
    and RulePackRequireSigned are server-scoped, so on IIS they can only be set
    once for the site's pagespeed.config (a "host:" match block refuses them),
    and one site cannot both trust a key for one host and serve an unsigned
    pack to another.

    Counters move once per child process, so the load and refusal counters are
    checked as absolute values.
    """

    def test_a_pack_signed_with_a_trusted_key_is_used(self, pack_site, pack_stats):
        site = pack_site(SIGNED_HOST)
        path = f"{PACK_DIR}/missing.html"
        html = _fetch(site, path, f"run={_run_id()}")
        after = pack_stats(SIGNED_HOST)

        # The pack's canonical template names the plain enforce host.
        assert _canonicals(html) == [f"https://{ENFORCE_HOST}{path}"], html
        assert after.get("rule_pack_loads_signed", 0) >= 1, after

    def test_an_unsigned_pack_is_refused_and_the_filter_is_off(self, pack_site, pack_stats):
        site = pack_site(REQUIRE_HOST)
        path = f"{PACK_DIR}/missing.html"
        before = pack_stats(REQUIRE_HOST)
        filtered = _fetch(site, path, f"run={_run_id()}")
        after = pack_stats(REQUIRE_HOST)
        unfiltered = _fetch(site, path, FILTER_OFF)

        assert filtered == unfiltered
        assert _canonicals(filtered) == []
        assert after.get("rule_pack_refused_unsigned", 0) >= 1, after
        assert_stat_delta(before, after, "rule_pack_pages_evaluated", 0)
        # The require host is not listed by the unsigned pack: only a refusal
        # (which turns the filter off before the host check) keeps this at 0.
        assert_stat_delta(before, after, "rule_pack_pages_skipped_host_not_listed", 0)


@pytest.mark.requires_fixture("rule_packs")
class TestUnlistedHost:
    def test_a_host_the_pack_does_not_list_is_left_alone(self, pack_site, pack_stats):
        site = pack_site(UNLISTED_HOST)
        path = f"{PACK_DIR}/missing.html"
        before = pack_stats(UNLISTED_HOST)
        filtered = _fetch(site, path)
        after = pack_stats(UNLISTED_HOST)
        unfiltered = _fetch(site, path, FILTER_OFF)

        assert filtered == unfiltered
        assert _canonicals(filtered) == []
        assert_stat_delta(before, after, "rule_pack_pages_skipped_host_not_listed", 1)
        assert_stat_delta(before, after, "rule_pack_pages_evaluated", 0)


class TestOffByDefault:
    """No pack file and no filter on the primary host: asking for it does nothing."""

    def test_the_filter_needs_a_pack_file(self, client: PageSpeedClient, test_root: str, stats_snapshot):
        before = stats_snapshot()
        response = client.get(f"{test_root}/rule_packs/missing.html?PageSpeedFilters=%2Brule_packs")
        require_status_ok(response, "primary missing.html with +rule_packs")
        after = stats_snapshot()

        assert _canonicals(response.text) == [], response.text
        assert_stat_delta(before, after, "rule_pack_pages_evaluated", 0)


@pytest.mark.requires_fixture("rule_packs", "flush_origin")
class TestSplitHead:
    """A head cut in two flush windows: inserts still work, edits of what was
    already sent fail closed."""

    ROUTE = "/mod_pagespeed_test/flush_origin/split_head"

    def test_an_insert_into_the_open_head_succeeds(self, pack_site, pack_stats):
        site = pack_site(ENFORCE_HOST)
        before = pack_stats(ENFORCE_HOST)
        html = _fetch(site, self.ROUTE, f"run={_run_id()}")
        after = pack_stats(ENFORCE_HOST)

        assert _canonicals(html) == [f"https://{ENFORCE_HOST}{self.ROUTE}"], html
        assert html.index('rel="canonical"') < html.index("</head>"), html
        assert_stat_delta(before, after, "rule_pack_pages_modified", 1)
        assert_stat_delta(before, after, "rule_pack_pages_skipped_not_rewritable", 0)

    def test_a_repair_of_an_element_already_sent_degrades_to_not_rewritable(
        self, pack_site, pack_stats
    ):
        site = pack_site(ENFORCE_HOST)
        before = pack_stats(ENFORCE_HOST)
        html = _fetch(site, self.ROUTE, f"run={_run_id()}&canonical=wrong")
        after = pack_stats(ENFORCE_HOST)

        # The head is as the origin sent it: both conflicting canonicals stay
        # and nothing was added.
        assert _canonicals(html) == [
            "https://wrong.example.com/a",
            "https://wrong.example.com/b",
        ], html
        assert_stat_delta(before, after, "rule_pack_pages_modified", 0)
        assert_stat_delta(before, after, "rule_pack_pages_skipped_not_rewritable", 1)

    def test_the_link_header_is_skipped_once_the_headers_went_out(self, pack_site, pack_stats):
        site = pack_site(ENFORCE_HOST)
        before = pack_stats(ENFORCE_HOST)
        entries = _link_header(site, self.ROUTE, f"run={_run_id()}")
        after = pack_stats(ENFORCE_HOST)

        # The origin's conflicting canonical is still there: the headers were
        # sent with the first window, before the head closed.
        assert _link_targets(entries, "canonical") == ["https://wrong.example.com/split"], entries
        assert_stat_delta(before, after, "rule_pack_headers_flushed", 1)
        assert_stat_delta(before, after, "rule_pack_headers_set", 0)
        # ... while the insert into the open head still works.
        assert_stat_delta(before, after, "rule_pack_pages_modified", 1)

    @pytest.mark.requires_fixture("rule_pack_log")
    def test_the_decision_log_says_why_the_header_was_skipped(self, pack_site):
        site = pack_site(ENFORCE_HOST)
        run = _run_id()
        _fetch(site, self.ROUTE, f"run={run}")
        lines = _wait_for_decision_lines(ENFORCE_HOST, f"{self.ROUTE}?run={run}")

        header = [l for l in lines if l["rule"] == "canonical-repair" and l.get("header")]
        assert len(header) == 1, lines
        assert header[0]["action"] == "none", header[0]
        assert header[0]["reason"] == "headers_flushed", header[0]


@pytest.mark.requires_fixture("rule_packs", "daemon_arm")
class TestDaemonDecline:
    """HTML comes from the classic chain on every host, pack or not: the
    daemon serves no HTML and the module records none into it. Stylesheets
    are still served by the daemon.

    Needs a lane with the optimizer daemon configured, and its worker running,
    on rulepacks.example.com and daemon.example.com (lane fixture
    "daemon_arm"); lanes without it skip these cases.
    """

    def test_html_comes_from_the_classic_chain_with_the_pack_applied(self, pack_site, pack_stats):
        site = pack_site(ENFORCE_HOST)
        path = f"{PACK_DIR}/missing.html"
        # Once so the record arm sees the document (it must not keep it); had
        # the daemon stored HTML for this URL, the next responses would come
        # from it, without the pack.
        _fetch(site, path)
        # Positive control: a stylesheet on this host gets daemon-served, so
        # the worker has had time to record and the HTML below is declined on
        # purpose, not for want of a worker.
        _serve_css_until_daemon_served(site, pack_stats, ENFORCE_HOST)
        before = pack_stats(ENFORCE_HOST)
        for _ in range(3):
            response = site.get(path)
            require_status_ok(response, f"{ENFORCE_HOST}{path}")
            assert _canonicals(response.text) == [f"https://{ENFORCE_HOST}{path}"], response.text
            # The daemon's hit leg adds Age and never the module's version header.
            assert not response.header("Age"), dict(response.headers)
            assert response.header("X-Mod-Pagespeed") or response.header("X-Page-Speed"), dict(
                response.headers
            )
        after = pack_stats(ENFORCE_HOST)

        assert_stat_delta(before, after, "ipro_daemon_served", 0)
        assert_stat_delta(before, after, "ipro_daemon_served_other", 0)
        assert after.get("ipro_daemon_fallthrough", 0) - before.get("ipro_daemon_fallthrough", 0) >= 3

    def test_html_comes_from_the_classic_chain_on_a_host_without_packs(self, pack_site, pack_stats):
        site = pack_site(DAEMON_HOST)
        # Once so the record arm sees the document (it must not keep it).
        _fetch(site, DAEMON_HTML_PATH)
        # Positive control: the daemon serves a stylesheet on this host, so
        # the worker is up and has had time to record; the HTML below is not
        # served by it by design, not for want of a worker.
        _serve_css_until_daemon_served(site, pack_stats, DAEMON_HOST)
        before = pack_stats(DAEMON_HOST)
        for _ in range(3):
            # No cache-buster here: a query makes the URL key-unsafe for the
            # daemon, which would then decline it for that reason instead of
            # the HTML rule. PageSpeed parameters are stripped before the
            # daemon sees the URL, so its key is the one recorded above.
            # remove_comments is the module's own HTML rewriting: a copy
            # served from the daemon's store would still carry the comment.
            response = site.get(f"{DAEMON_HTML_PATH}?PageSpeedFilters=%2Bremove_comments")
            require_status_ok(response, f"{DAEMON_HOST}{DAEMON_HTML_PATH}")
            assert "daemon lane page" in response.text, response.text
            assert "lane-comment" not in response.text, response.text
            assert not response.header("Age"), dict(response.headers)
            assert response.header("X-Mod-Pagespeed") or response.header("X-Page-Speed"), dict(
                response.headers
            )
        after = pack_stats(DAEMON_HOST)

        assert_stat_delta(before, after, "ipro_daemon_served", 0)
        assert_stat_delta(before, after, "ipro_daemon_served_other", 0)
        assert after.get("ipro_daemon_fallthrough", 0) - before.get("ipro_daemon_fallthrough", 0) >= 3

    def test_a_stylesheet_is_still_served_by_the_daemon(self, pack_site, pack_stats):
        site = pack_site(ENFORCE_HOST)
        _serve_css_until_daemon_served(site, pack_stats, ENFORCE_HOST, "ipro_daemon_served_css")


@pytest.mark.requires_fixture("daemon_arm")
class TestDaemonServing:
    """The lane's daemon serves: on a plain daemon-enabled virtual host a
    stylesheet is recorded, optimized by the worker, and then served from the
    daemon's volume rather than by the module's own in-place path.
    """

    def test_a_stylesheet_is_served_by_the_daemon(self, pack_site, pack_stats):
        site = pack_site(DAEMON_HOST)
        response = _serve_css_until_daemon_served(site, pack_stats, DAEMON_HOST)
        # The daemon's hit leg adds Age and never the module's version header.
        assert response.header("Age") is not None, dict(response.headers)
        assert not response.header("X-Mod-Pagespeed") and not response.header("X-Page-Speed"), dict(
            response.headers
        )


def _decision_lines_by_pack(host: str, path_prefix: str) -> Dict[str, List[dict]]:
    """The decision-log lines of `host` under `path_prefix`, by the name of the
    pack's directory under <LogDir>/packs/ (and the file each came from)."""
    log_dir = os.environ.get("PAGESPEED_RULE_PACK_LOG_DIR")
    if not log_dir:
        pytest.fail("lane fixture rule_pack_log is provided but PAGESPEED_RULE_PACK_LOG_DIR is not set")
    found: Dict[str, List[dict]] = {}
    for name in sorted(glob.glob(os.path.join(log_dir, "packs", "*", "decisions-*.jsonl"))):
        assert re.fullmatch(r"decisions-\d{4}-\d{2}-\d{2}\.jsonl", os.path.basename(name)), name
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
        pack_dir = os.path.basename(os.path.dirname(name))
        for raw in text.splitlines():
            entry = json.loads(raw)
            if entry.get("host") == host and entry.get("path", "").startswith(path_prefix):
                found.setdefault(pack_dir, []).append(entry)
    return found


@pytest.mark.requires_fixture("rule_packs", "rule_pack_multi")
class TestSeveralPacksOnOneHost:
    """Two RulePackFile lines on one virtual host: both packs run, in order,
    and the first one owns a kind both of them carry.

    a.json (id edge-seo) has a canonical rule; b.json (id fixture-second) has
    title and description rules and a canonical rule of its own.
    """

    def test_packs_with_different_kinds_both_apply_on_the_first_request(self, pack_site):
        site = pack_site(MULTI_HOST)
        path = f"{PACK_DIR}/missing.html"
        html = _fetch(site, path, f"run={_run_id()}")

        assert _canonicals(html) == [f"https://{MULTI_HOST}{path}"], html
        assert TITLE_RE.findall(html) == [f"Second title for {path}"], html
        assert DESCRIPTION_RE.findall(html) == [f"Second description for {path}"], html
        assert html.index('rel="canonical"') < html.index("</head>"), html

    def test_the_first_pack_wins_a_kind_both_carry(self, pack_site, pack_stats):
        site = pack_site(MULTI_HOST)
        path = f"{PACK_DIR}/missing.html"
        before = pack_stats(MULTI_HOST)
        html = _fetch(site, path, f"run={_run_id()}")
        after = pack_stats(MULTI_HOST)

        # b.json's canonical rule (second.example.com) is held off the page.
        assert _canonicals(html) == [f"https://{MULTI_HOST}{path}"], html
        assert "second.example.com" not in html, html
        assert_stat_delta(before, after, "rule_pack_decisions_claimed", 1)
        assert_stat_delta(before, after, "rule_pack_decisions_applied", 3)

    @pytest.mark.requires_fixture("rule_pack_log")
    def test_every_pack_writes_its_own_decision_log(self, pack_site):
        site = pack_site(MULTI_HOST)
        run = _run_id()
        path = f"{PACK_DIR}/missing.html"
        _fetch(site, path, f"run={run}")
        prefix = f"{path}?run={run}"
        deadline = time.time() + 10.0
        while True:
            by_pack = _decision_lines_by_pack(MULTI_HOST, prefix)
            if {"edge-seo", "fixture-second"} <= set(by_pack) or time.time() >= deadline:
                break
            time.sleep(0.2)

        assert {"edge-seo", "fixture-second"} <= set(by_pack), by_pack
        assert {l["rule"] for l in by_pack["edge-seo"]} == {"multi-a-canonical"}, by_pack
        assert {l["rule"] for l in by_pack["fixture-second"]} == {
            "multi-b-title",
            "multi-b-description",
            "multi-b-canonical",
        }, by_pack
        claimed = [l for l in by_pack["fixture-second"] if l["rule"] == "multi-b-canonical"]
        assert len(claimed) == 1, claimed
        assert claimed[0]["action"] == "none", claimed[0]
        assert claimed[0]["reason"] == "claimed", claimed[0]
        assert claimed[0]["claimed_by"] == "edge-seo", claimed[0]
        # Only a claimed line names an owner.
        assert all("claimed_by" not in l for l in by_pack["edge-seo"]), by_pack

    def test_a_second_file_with_the_first_ones_pack_id_is_refused(self, pack_site, pack_stats):
        site = pack_site(MULTI_DUP_HOST)
        path = f"{PACK_DIR}/missing.html"
        html = _fetch(site, path, f"run={_run_id()}")
        after = pack_stats(MULTI_DUP_HOST)

        # The first pack acts; the second file's title rule never does.
        assert _canonicals(html) == [f"https://{MULTI_HOST}{path}"], html
        assert TITLE_RE.findall(html) == [], html
        assert "Duplicate title" not in html, html
        # Moves once per process and file version.
        assert after.get("rule_pack_refused_duplicate_id", 0) >= 1, after

    def test_the_admin_json_lists_both_packs_in_order(self, pack_site, server_config):
        site = pack_site(MULTI_HOST)
        params = [] if (server_config.is_nginx or server_config.server_type == "envoy") else ["PageSpeed=off"]
        url = f"{server_config.admin_path}/rule_packs?" + "&".join(params + ["json=1"])
        path = f"{PACK_DIR}/missing.html"

        # The configuration block is the same in every process; a pack is
        # listed with its id once the process that answers has loaded it, so
        # drive pages through the host until the answering process has.
        deadline = time.time() + 15.0
        status = {}
        while time.time() < deadline:
            _fetch(site, path, f"run={_run_id()}")
            response = site.get(url)
            require_status_ok(response, "rule_packs json")
            assert response.headers.get("Content-Type", "").startswith("application/json"), response.headers
            status = json.loads(response.text)
            if [p["id"] for p in status["packs"]] == ["edge-seo", "fixture-second"]:
                break
            time.sleep(0.5)

        assert status["schema"] == 1, status
        config = status["configuration"]
        assert config["filter_enabled"] is True, config
        assert [f.rsplit("/", 1)[-1] for f in config["rule_pack_files"]] == ["a.json", "b.json"], config
        assert config["rule_pack_mode"] == "enforce", config
        assert config["rule_pack_require_signed"] is False, config
        assert [p["id"] for p in status["packs"]] == ["edge-seo", "fixture-second"], status
        assert [p["position"] for p in status["packs"]] == [0, 1], status
        assert [p["file"] for p in status["packs"]] == config["rule_pack_files"], status
        assert all(p["state"] == "loaded" for p in status["packs"]), status
        assert all(len(p["sha256"]) == 64 for p in status["packs"]), status
        assert "rule_pack_decisions_claimed" in (status["counters"] or {}), status


@pytest.mark.requires_fixture("rule_packs")
class TestPackListLimit:
    """At most eight RulePackFile lines in one scope: a ninth makes the
    configuration test fail (apachectl -t, nginx -t).

    The check runs against a copy of the lane's configuration with one more
    virtual host / server added, so the lane's own server is untouched. IIS
    and Envoy are covered by their configuration unit tests.
    """

    LIMIT = 8

    @staticmethod
    def _apache_main_config(ctl: str):
        """The main configuration file of the Apache this rig runs (from
        `<ctl> -V`: HTTPD_ROOT and SERVER_CONFIG_FILE), or None."""
        try:
            out = subprocess.run(
                ["sudo", "-n", ctl, "-V"], capture_output=True, text=True, timeout=60
            ).stdout
        except (OSError, subprocess.SubprocessError):
            return None
        root = re.search(r'-D HTTPD_ROOT="([^"]*)"', out)
        conf = re.search(r'-D SERVER_CONFIG_FILE="([^"]*)"', out)
        if not conf:
            return None
        path = conf.group(1)
        if not os.path.isabs(path):
            if not root:
                return None
            path = os.path.join(root.group(1), path)
        return path if os.path.exists(path) else None

    def _check(self, server_config, count: int):
        files = [f"/nonexistent/packs/limit-{i}.json" for i in range(count)]
        tmp = None
        try:
            if server_config.is_nginx:
                binary = os.environ.get("PAGESPEED_NGINX_BINARY")
                conf = os.environ.get("PAGESPEED_NGINX_CONFIG_FILE")
                if not binary or not conf or not os.path.exists(conf):
                    pytest.skip("the nginx binary / configuration of the lane are not known")
                conf_dir = os.path.dirname(conf)
                server = (
                    "    server {\n        listen 65098;\n        server_name limit.example.com;\n"
                    "        pagespeed on;\n"
                    + "".join(f"        pagespeed RulePackFile {f};\n" for f in files)
                    + "    }\n"
                )
                text = open(conf, encoding="utf-8").read()
                head, sep, rest = text.partition("    server {")
                assert sep, "no server block in the lane configuration"
                tmp = os.path.join(conf_dir, f"limit-check-{os.getpid()}.conf")
                with open(tmp, "w", encoding="utf-8") as f:
                    f.write(head + server + sep + rest)
                command = [binary, "-t", "-p", conf_dir, "-c", tmp]
            elif server_config.server_type == "apache":
                ctl = os.environ.get("PAGESPEED_APACHE_CTL", "apache2ctl")
                main_conf = self._apache_main_config(ctl)
                if main_conf is None:
                    pytest.skip("the main Apache configuration file of this rig is not known")
                tmp = f"/tmp/limit-check-{os.getpid()}.conf"
                with open(tmp, "w", encoding="utf-8") as f:
                    f.write(f"Include {main_conf}\n")
                    f.write("<VirtualHost *:65098>\n  ServerName limit.example.com\n")
                    for name in files:
                        f.write(f"  ModPagespeedRulePackFile {name}\n")
                    f.write("</VirtualHost>\n")
                command = ["sudo", "-n", ctl, "-t", "-f", tmp]
            else:
                pytest.skip("configuration syntax check is driven for Apache and nginx only")
            return subprocess.run(command, capture_output=True, text=True, timeout=60)
        except FileNotFoundError:
            pytest.skip("the server binary needed for the syntax check is not available")
        finally:
            if tmp and os.path.exists(tmp):
                os.unlink(tmp)

    def test_eight_lines_in_one_scope_are_accepted(self, server_config):
        result = self._check(server_config, self.LIMIT)
        output = result.stdout + result.stderr
        if "a password is required" in output:
            pytest.skip("passwordless sudo is required to run the syntax check")
        assert result.returncode == 0, output

    def test_a_ninth_line_fails_the_configuration_test(self, server_config):
        result = self._check(server_config, self.LIMIT + 1)
        output = result.stdout + result.stderr
        if "a password is required" in output:
            pytest.skip("passwordless sudo is required to run the syntax check")
        assert result.returncode != 0, output
        assert "at most 8" in output, output


if __name__ == "__main__":
    raise SystemExit(pytest.main([__file__, "-v"]))
