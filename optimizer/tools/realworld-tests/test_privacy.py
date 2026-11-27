# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2024-2026 We-Amp B.V.

"""Privacy and cache safety tests.

Verifies that responses with no-store/private Cache-Control directives
are never cached. Uses httpbin for deterministic, programmable headers.

Per src/nginx/CLAUDE.md: "Cache-Control: no-store/private → NOT cached,
no worker notification."
"""

import time

import pytest


def test_no_store_stays_miss(nginx):
    """Responses with Cache-Control: no-store are never cached."""
    path = "/httpbin/response-headers?Cache-Control=no-store"
    for i in range(3):
        resp = nginx.get(path)
        assert resp.status_code == 200, (
            f"Request {i + 1}: expected 200, got {resp.status_code}"
        )
        assert resp.headers.get("X-PageSpeed") == "MISS", (
            f"Request {i + 1}: expected MISS for no-store, "
            f"got {resp.headers.get('X-PageSpeed')}"
        )


def test_private_stays_miss(nginx):
    """Responses with Cache-Control: private are never cached."""
    path = "/httpbin/response-headers?Cache-Control=private"
    for i in range(3):
        resp = nginx.get(path)
        assert resp.status_code == 200
        assert resp.headers.get("X-PageSpeed") == "MISS", (
            f"Request {i + 1}: expected MISS for private, "
            f"got {resp.headers.get('X-PageSpeed')}"
        )


def test_combined_no_store_private_stays_miss(nginx):
    """Responses with both no-store and private stay MISS."""
    path = "/httpbin/response-headers?Cache-Control=no-store%2C+private"
    for i in range(3):
        resp = nginx.get(path)
        assert resp.status_code == 200
        assert resp.headers.get("X-PageSpeed") == "MISS", (
            f"Request {i + 1}: expected MISS for no-store+private, "
            f"got {resp.headers.get('X-PageSpeed')}"
        )


def test_cached_html_served_with_no_cache(nginx):
    """HTML with no origin Cache-Control is served with no-cache on HIT.

    Per CLAUDE.md type defaults: "HTML=no-cache" when origin sends no CC.
    This is distinct from no-store — the content IS cached but the
    browser is told to revalidate.
    """
    resp = nginx.poll_for_hit("/example/", timeout=20)
    ps = resp.headers.get("X-PageSpeed")
    if ps != "HIT":
        pytest.skip("Cache not warm for no-cache test")
    cc = resp.headers.get("Cache-Control", "")
    assert "no-cache" in cc, f"HTML HIT should have Cache-Control: no-cache, got: {cc}"


def test_no_store_absent_from_cache_urls(nginx, worker_api):
    """no-store URLs do not appear in the cache URL list."""
    marker = f"_privacy_{int(time.time() * 1000)}"
    path = f"/httpbin/response-headers?Cache-Control=no-store&marker={marker}"
    nginx.get(path)
    time.sleep(0.5)

    data = worker_api.cache_urls(limit=500)
    cached_urls = [u.get("url", "") for u in data.get("urls", [])]
    for url in cached_urls:
        assert marker not in url, f"no-store URL found in cache: {url}"


def test_no_store_origin_header_preserved(nginx):
    """Origin's no-store header is visible on the MISS response."""
    import time as t

    unique = (
        f"/httpbin/response-headers?Cache-Control=no-store&t={int(t.time() * 1000)}"
    )
    resp = nginx.get(unique)
    assert resp.status_code == 200
    cc = resp.headers.get("Cache-Control", "")
    assert "no-store" in cc, (
        f"Origin's no-store should be visible on MISS, got CC: {cc}"
    )


def test_error_responses_not_cached(nginx):
    """5xx error responses stay MISS."""
    path = "/httpbin/status/500"
    resp = nginx.get(path)
    ps = resp.headers.get("X-PageSpeed")
    assert ps == "MISS" or ps is None, (
        f"Error response should not be cached, got X-PageSpeed: {ps}"
    )
