# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2024-2026 We-Amp B.V.

"""Cache lifecycle tests against real-world sites.

Verifies MISS -> HIT transitions, Age header behavior, cache API entries,
and query-string cache key differentiation.
"""

import time

import pytest


def test_example_com_miss_to_hit(nginx):
    """example.com transitions from MISS to HIT."""
    resp1 = nginx.get("/example/")
    assert resp1.status_code == 200

    resp2 = nginx.poll_for_hit("/example/", timeout=20)
    assert resp2.headers.get("X-PageSpeed") == "HIT"


def test_httpbin_html_cacheable(nginx):
    """httpbin HTML response gets cached (module caches text/html)."""
    path = "/httpbin/html"
    resp1 = nginx.get(path)
    assert resp1.status_code == 200

    resp2 = nginx.poll_for_hit(path, timeout=30)
    assert resp2.headers.get("X-PageSpeed") == "HIT"


def test_hit_has_age_header(nginx):
    """HIT responses include an Age header."""
    resp = nginx.poll_for_hit("/example/", timeout=20)
    ps = resp.headers.get("X-PageSpeed")
    if ps != "HIT":
        pytest.skip("Cache not warm yet — could not get HIT for /example/")
    age = resp.headers.get("Age")
    assert age is not None, "Age header missing on HIT"
    assert int(age) >= 0


def test_cache_urls_api_has_entries(nginx, worker_api):
    """Cache URLs API shows entries after proxied requests."""
    nginx.poll_for_hit("/example/", timeout=20)
    time.sleep(0.5)

    data = worker_api.cache_urls()
    urls = data.get("urls", [])
    assert len(urls) > 0, "Cache URL list is empty after proxied requests"


def test_query_string_differentiates_cache_keys(nginx):
    """Different query strings produce distinct cache entries."""
    ts = int(time.time() * 1000)
    path_a = f"/httpbin/html?key={ts}_a"

    # Seed path_a
    nginx.get(path_a)

    # Wait for path_a to be cached
    resp_a = nginx.poll_for_hit(path_a, timeout=30)
    if resp_a.headers.get("X-PageSpeed") != "HIT":
        pytest.skip("Cache not warm for query-string test")

    # A fresh unique query string should be MISS
    path_c = f"/httpbin/html?key={ts}_c"
    resp_c = nginx.get(path_c)
    assert resp_c.headers.get("X-PageSpeed") == "MISS", (
        "Fresh query string should be MISS — cache keys may not include query strings"
    )


def test_error_response_not_cached(nginx):
    """4xx error responses are not cached (no X-PageSpeed or MISS)."""
    path = "/httpbin/status/404"
    for i in range(3):
        resp = nginx.get(path)
        if resp.status_code == 404:
            ps = resp.headers.get("X-PageSpeed")
            # Module doesn't process non-200 responses: either MISS or absent
            assert ps in ("MISS", None), (
                f"Request {i + 1}: 404 should not be cached, got X-PageSpeed: {ps}"
            )


def test_redirect_not_cached(nginx):
    """Redirect responses pass through and are not cached."""
    path = "/httpbin/status/302"
    resp = nginx.get(path, allow_redirects=False)
    assert resp.status_code == 302
    ps = resp.headers.get("X-PageSpeed")
    # Redirects should be MISS — module only caches 200 OK
    assert ps == "MISS" or ps is None, (
        f"302 redirect should not be cached, got X-PageSpeed: {ps}"
    )
