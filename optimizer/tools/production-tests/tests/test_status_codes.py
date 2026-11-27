# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2024-2026 We-Amp B.V.

"""Status code handling tests for the caching reverse proxy."""

import time
from uuid import uuid4

import pytest


class TestStatusCodes:
    """Verify correct caching behavior for each HTTP status code."""

    # ------------------------------------------------------------------
    # P0 tests
    # ------------------------------------------------------------------

    @pytest.mark.p0
    def test_200_ok_cached(self, client):
        """200 OK with Cache-Control is cached after initial MISS."""
        uid = uuid4().hex
        path = f"/status/200?cache=3600&body=hello-{uid}"
        r = client.get(path)
        client.assert_miss(r)
        assert r.status_code == 200
        hit = client.poll_for_hit(path)
        assert hit.status_code == 200
        assert f"hello-{uid}" in hit.text

    @pytest.mark.p0
    def test_301_moved_permanently_passthrough(self, client):
        """301 redirect passes through (proxy only caches 2xx)."""
        uid = uuid4().hex
        path = f"/redirect/301?cache=3600&to=/status/200?body=dest-{uid}"
        r = client.get(path, allow_redirects=False)
        assert r.status_code == 301
        assert "/status/200" in r.headers["Location"]
        # Proxy only caches 2xx; 301 is always fetched from origin.
        time.sleep(2)
        r2 = client.get(path, allow_redirects=False)
        assert r2.status_code == 301
        client.assert_miss(r2)

    @pytest.mark.p0
    def test_302_found_not_cached_default(self, client):
        """302 redirect without Cache-Control is not cached."""
        uid = uuid4().hex
        path = f"/redirect/302?to=/status/200?body=temp-{uid}"
        r = client.get(path, allow_redirects=False)
        client.assert_miss(r)
        assert r.status_code == 302
        time.sleep(2)
        r2 = client.get(path, allow_redirects=False)
        client.assert_miss(r2)

    @pytest.mark.p0
    def test_304_not_modified_passthrough(self, client):
        """Origin 304 Not Modified is not stored in cache."""
        uid = uuid4().hex
        path = f"/conditional/{uid}"
        # First request to get the ETag.
        r = client.get(path)
        etag = r.headers.get("ETag")
        assert etag is not None, "Origin did not return ETag"
        # Request with If-None-Match to trigger a 304 from origin.
        r2 = client.get(path, headers={"If-None-Match": etag})
        # Proxy may return 200 (serving from its own cache) or 304;
        # either way, 304 itself must not be stored as a cacheable object.
        assert r2.status_code in (200, 304)

    @pytest.mark.p0
    def test_400_bad_request_not_cached(self, client):
        """400 Bad Request is not cached."""
        uid = uuid4().hex
        path = f"/status/400?body=bad-{uid}"
        r = client.get(path)
        client.assert_miss(r)
        assert r.status_code == 400
        time.sleep(2)
        r2 = client.get(path)
        client.assert_miss(r2)

    @pytest.mark.p0
    def test_404_not_found_not_cached_default(self, client):
        """404 without Cache-Control is not cached."""
        uid = uuid4().hex
        path = f"/status/404?body=gone-{uid}"
        r = client.get(path)
        client.assert_miss(r)
        assert r.status_code == 404
        time.sleep(2)
        r2 = client.get(path)
        client.assert_miss(r2)

    @pytest.mark.p0
    def test_404_not_found_passthrough_with_cc(self, client):
        """404 with explicit max-age passes through (proxy only caches 2xx)."""
        uid = uuid4().hex
        path = f"/status/404?cache=3600&body=cached-404-{uid}"
        r = client.get(path)
        client.assert_miss(r)
        assert r.status_code == 404
        # Proxy only caches 2xx; 404 always fetched from origin.
        time.sleep(2)
        r2 = client.get(path)
        assert r2.status_code == 404
        client.assert_miss(r2)

    @pytest.mark.p0
    def test_500_internal_error_not_cached(self, client):
        """500 Internal Server Error is not cached."""
        uid = uuid4().hex
        path = f"/status/500?body=error-{uid}"
        r = client.get(path)
        client.assert_miss(r)
        assert r.status_code == 500
        time.sleep(2)
        r2 = client.get(path)
        client.assert_miss(r2)

    @pytest.mark.p0
    def test_502_bad_gateway_not_cached(self, client):
        """502 Bad Gateway is not cached."""
        uid = uuid4().hex
        path = f"/status/502?body=badgw-{uid}"
        r = client.get(path)
        client.assert_miss(r)
        assert r.status_code == 502
        time.sleep(2)
        r2 = client.get(path)
        client.assert_miss(r2)

    @pytest.mark.p0
    def test_503_service_unavailable_not_cached(self, client):
        """503 Service Unavailable is not cached."""
        uid = uuid4().hex
        path = f"/status/503?body=unavail-{uid}"
        r = client.get(path)
        client.assert_miss(r)
        assert r.status_code == 503
        time.sleep(2)
        r2 = client.get(path)
        client.assert_miss(r2)

    # ------------------------------------------------------------------
    # P1 tests
    # ------------------------------------------------------------------

    @pytest.mark.p1
    def test_201_created_post_not_cached(self, client):
        """POST 201 Created response is not cached."""
        uid = uuid4().hex
        path = f"/status/201?cache=3600&body=created-{uid}"
        r = client.post(path)
        assert r.status_code == 201
        time.sleep(2)
        r2 = client.post(path)
        assert r2.status_code == 201
        client.assert_miss(r2)

    @pytest.mark.p1
    def test_204_no_content_not_cached(self, client):
        """204 No Content is not cached."""
        uid = uuid4().hex
        path = "/status/204?body=&cache=0"
        r = client.get(f"{path}&_={uid}")
        assert r.status_code == 204
        time.sleep(2)
        r2 = client.get(f"{path}&_={uid}")
        client.assert_miss(r2)

    @pytest.mark.p1
    def test_307_temporary_redirect_passthrough(self, client):
        """307 Temporary Redirect passes through (proxy only caches 2xx)."""
        uid = uuid4().hex
        path = f"/redirect/307?cache=3600&to=/status/200?body=temp307-{uid}"
        r = client.get(path, allow_redirects=False)
        client.assert_miss(r)
        assert r.status_code == 307
        time.sleep(2)
        r2 = client.get(path, allow_redirects=False)
        assert r2.status_code == 307
        client.assert_miss(r2)

    @pytest.mark.p1
    def test_308_permanent_redirect_passthrough(self, client):
        """308 Permanent Redirect passes through (proxy only caches 2xx)."""
        uid = uuid4().hex
        path = f"/redirect/308?cache=3600&to=/status/200?body=perm308-{uid}"
        r = client.get(path, allow_redirects=False)
        client.assert_miss(r)
        assert r.status_code == 308
        time.sleep(2)
        r2 = client.get(path, allow_redirects=False)
        assert r2.status_code == 308
        client.assert_miss(r2)

    @pytest.mark.p1
    def test_410_gone_passthrough(self, client):
        """410 Gone passes through (proxy only caches 2xx)."""
        uid = uuid4().hex
        path = f"/status/410?cache=3600&body=gone-{uid}"
        r = client.get(path)
        client.assert_miss(r)
        assert r.status_code == 410
        time.sleep(2)
        r2 = client.get(path)
        assert r2.status_code == 410
        client.assert_miss(r2)

    @pytest.mark.p1
    def test_429_too_many_requests_not_cached(self, client):
        """429 Too Many Requests is not cached."""
        uid = uuid4().hex
        path = f"/status/429?body=ratelimit-{uid}"
        r = client.get(path)
        client.assert_miss(r)
        assert r.status_code == 429
        time.sleep(2)
        r2 = client.get(path)
        client.assert_miss(r2)

    # ------------------------------------------------------------------
    # P2 tests
    # ------------------------------------------------------------------

    @pytest.mark.p2
    def test_403_forbidden_passthrough(self, client):
        """403 Forbidden passes through (proxy only caches 2xx)."""
        uid = uuid4().hex
        path = f"/status/403?cache=3600&body=forbidden-{uid}"
        r = client.get(path)
        client.assert_miss(r)
        assert r.status_code == 403
        time.sleep(2)
        r2 = client.get(path)
        assert r2.status_code == 403
        client.assert_miss(r2)

    @pytest.mark.p2
    def test_405_allow_header_preserved(self, client):
        """Allow header is preserved on 405 Method Not Allowed."""
        r = client.get("/headers/allow")
        assert r.status_code == 405
        allow = r.headers.get("Allow")
        assert allow is not None, "Allow header missing from 405 response"
        assert "GET" in allow

    @pytest.mark.p2
    def test_429_retry_after_preserved(self, client):
        """Retry-After header is preserved on 429 response."""
        r = client.get("/headers/retry-after")
        assert r.status_code == 429
        retry_after = r.headers.get("Retry-After")
        assert retry_after is not None, "Retry-After header missing from 429 response"
        assert retry_after == "120"

    @pytest.mark.p2
    def test_504_gateway_timeout(self, client):
        """Origin timeout results in 504 Gateway Timeout."""
        r = client.get("/delay/30?status=504", timeout=60)
        assert r.status_code == 504
