#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2026 We-Amp B.V.
"""Streamed HTML rewriting on IIS.

The /aspnet application under the test site (setup_iis_full.ps1 copies
test/system/iis/aspnet/ to <web root>/aspnet and runs it in its own app
pool with managed runtime v4.0) serves stream.aspx: one HTML document
written in ten Response.Write + Response.Flush steps with 50 ms sleeps.
The module therefore sees several sends for a single rewritten response,
which is the path issue #314 hardens.
"""

import concurrent.futures
import os

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


def _assert_streamed_page(client: PageSpeedClient) -> None:
    """Fetch stream.aspx once and assert the rewrite contract."""
    response = client.get(STREAM_URL + "?PageSpeedFilters=+collapse_whitespace")
    assert_http_status(response, 200)

    text = response.text

    # Complete body: the closing marker is present and only the closing
    # tags follow it (whitespace-insensitive: collapse_whitespace may have
    # reflowed them).
    assert CLOSING_MARKER in text, "streamed page truncated before closing marker"
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
    @pytest.mark.skipif(
        os.environ.get("PAGESPEED_IIS_STREAM_STRESS") != "1",
        reason="enabled by the streamed-HTML fix",
    )
    def test_streamed_html_concurrent_fetches(self, server_config):
        """Concurrent fetches keep the streamed rewrite contract.

        20 clients x 10 fetches: overlapping multi-send responses are what
        issue #314 fixes; this is the proof. Gated behind
        PAGESPEED_IIS_STREAM_STRESS until that fix lands so the lane stays
        green; the fix PR removes the gate.
        """

        def fetch_all() -> None:
            worker = PageSpeedClient(
                host=server_config.host, port=server_config.port
            )
            for _ in range(STRESS_FETCHES_PER_CLIENT):
                _assert_streamed_page(worker)

        with concurrent.futures.ThreadPoolExecutor(
            max_workers=STRESS_CLIENTS
        ) as pool:
            futures = [pool.submit(fetch_all) for _ in range(STRESS_CLIENTS)]
            for future in futures:
                future.result()


if __name__ == "__main__":
    raise SystemExit(pytest.main([__file__, "-v"]))
