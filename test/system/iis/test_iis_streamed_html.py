#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2026 We-Amp B.V.
"""Streamed HTML rewriting on IIS.

The /aspnet application under the test site (setup_iis_full.ps1 copies
test/system/iis/aspnet/ to <web root>/aspnet and runs it in its own app
pool with managed runtime v4.0) serves stream.aspx: one HTML document
written in ten Response.Write + Response.Flush steps with 50 ms sleeps.
The module therefore sees several sends for a single rewritten response,
which is the path issue #314 hardens. Every step ends with an ordered marker
paragraph (c0 .. c9); ?padKb=N pads each step with an N KB comment so the
rewriter flushes before the page is complete.
"""

import concurrent.futures
import re

import pytest

from pagespeed_test_framework import PageSpeedClient, assert_http_status

STREAM_URL = "/aspnet/stream.aspx"
CLOSING_MARKER = "<!-- stream-end -->"
# stream.aspx writes runs of 40+ spaces; collapse_whitespace (not a core
# filter, so the request adds it) must reduce them to at most one space.
LONG_WHITESPACE_RUN = " " * 24
# Enough parallel clients and fetches to overlap sends across worker threads.
STRESS_CLIENTS = 20
STRESS_FETCHES_PER_CLIENT = 10
# Each of the ten steps carries this much padding in the concurrent test:
# above the rewriter's 100 KB flush limit, so output is produced while the
# page is still being sent.
STRESS_PAD_KB = 128
STEP_MARKERS = [f"c{i}" for i in range(10)]
MARKER_PATTERN = re.compile(r'<p id="(c\d+)">')


def _assert_streamed_page(client: PageSpeedClient, pad_kb: int = 0) -> None:
    """Fetch stream.aspx once and assert the rewrite contract."""
    query = "?PageSpeedFilters=+collapse_whitespace"
    if pad_kb:
        query += f"&padKb={pad_kb}"
    response = client.get(STREAM_URL + query)
    assert_http_status(response, 200)

    text = response.text

    # Complete body: the closing marker is present and only the closing
    # tags follow it (whitespace-insensitive: collapse_whitespace may have
    # reflowed them).
    assert CLOSING_MARKER in text, "streamed page truncated before closing marker"
    # Every step arrived once and in order.
    assert MARKER_PATTERN.findall(text) == STEP_MARKERS, (
        "step markers missing, repeated or out of order"
    )
    tail = text.rsplit(CLOSING_MARKER, 1)[1]
    assert "".join(tail.split()) == "</body></html>", (
        f"unexpected content after closing marker: {tail!r}"
    )

    # Rewritten: the module stamped the response...
    assert response.header("X-Page-Speed") or response.header("X-Mod-Pagespeed"), (
        "streamed response lacks the PageSpeed version header"
    )
    # ...and collapse_whitespace removed the long whitespace runs.
    assert LONG_WHITESPACE_RUN not in text, "long whitespace runs survived"

    content_type = response.header("Content-Type")
    assert content_type.lower().startswith("text/html"), (
        f"unexpected Content-Type: {content_type!r}"
    )

    # Rewritten HTML is not cacheable; accept the same directive set the
    # beacon and statistics tests accept.
    cache_control = response.header("Cache-Control")
    assert cache_control, "rewritten HTML should carry a Cache-Control header"
    assert any(
        directive in cache_control.lower()
        for directive in ("private", "max-age=0", "no-cache")
    ), f"unexpected Cache-Control on rewritten HTML: {cache_control!r}"


class TestStreamedHtml:
    @pytest.mark.iis_only
    @pytest.mark.requires_fixture("aspnet_stream")
    def test_streamed_html_single_fetch(self, client: PageSpeedClient):
        """One fetch: the streamed page is rewritten completely."""
        _assert_streamed_page(client)

    @pytest.mark.iis_only
    @pytest.mark.requires_fixture("aspnet_stream")
    @pytest.mark.slow
    def test_streamed_html_concurrent_fetches(self, server_config):
        """Concurrent fetches keep the streamed rewrite contract.

        20 clients x 10 fetches, each page padded to about 1.3 MB: output is
        produced by the rewriter while the application is still sending, and
        overlapping multi-send responses multiply the chances of two threads
        touching one response, which is what issue #314 fixes. Every fetch
        must carry the markers c0 to c9 exactly once and in order.
        """

        def fetch_all() -> None:
            worker = PageSpeedClient(
                host=server_config.host, port=server_config.port
            )
            for _ in range(STRESS_FETCHES_PER_CLIENT):
                _assert_streamed_page(worker, pad_kb=STRESS_PAD_KB)

        with concurrent.futures.ThreadPoolExecutor(
            max_workers=STRESS_CLIENTS
        ) as pool:
            futures = [pool.submit(fetch_all) for _ in range(STRESS_CLIENTS)]
            for future in futures:
                future.result()


if __name__ == "__main__":
    raise SystemExit(pytest.main([__file__, "-v"]))
