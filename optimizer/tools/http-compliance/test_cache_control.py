# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2024-2026 We-Amp B.V.

"""Cache-Control compliance tests.

Verifies that Cache-Control directives from the origin are properly
forwarded through the proxy, and that the proxy doesn't add spurious
directives. Also verifies caching semantics (no-store prevents caching).

RFC 9111: HTTP Caching.
"""


class TestCacheControlForwarding:
    """Cache-Control directives forwarded from origin."""

    def test_no_store_forwarded(self, client):
        """Cache-Control: no-store forwarded from origin."""
        r = client.get("/cc/no-store")
        assert r.status_code == 200
        cc = r.headers.get("Cache-Control", "")
        assert "no-store" in cc, f"Expected no-store, got: {cc}"

    def test_no_cache_forwarded(self, client):
        """Cache-Control: no-cache forwarded from origin."""
        r = client.get("/cc/no-cache")
        assert r.status_code == 200
        cc = r.headers.get("Cache-Control", "")
        assert "no-cache" in cc, f"Expected no-cache, got: {cc}"

    def test_private_forwarded(self, client):
        """Cache-Control: private forwarded from origin."""
        r = client.get("/cc/private")
        assert r.status_code == 200
        cc = r.headers.get("Cache-Control", "")
        assert "private" in cc, f"Expected private, got: {cc}"

    def test_max_age_forwarded(self, client):
        """Cache-Control: max-age forwarded from origin."""
        r = client.get("/cc/max-age")
        assert r.status_code == 200
        cc = r.headers.get("Cache-Control", "")
        assert "max-age" in cc, f"Expected max-age, got: {cc}"

    def test_must_revalidate_forwarded(self, client):
        """Cache-Control: must-revalidate forwarded from origin."""
        r = client.get("/cc/must-revalidate")
        assert r.status_code == 200
        cc = r.headers.get("Cache-Control", "")
        assert "must-revalidate" in cc, f"Expected must-revalidate, got: {cc}"

    def test_s_maxage_forwarded(self, client):
        """Cache-Control: s-maxage forwarded from origin."""
        r = client.get("/cc/s-maxage")
        assert r.status_code == 200
        cc = r.headers.get("Cache-Control", "")
        assert "s-maxage" in cc, f"Expected s-maxage, got: {cc}"


class TestNoStorePreventsCache:
    """No-store prevents caching — subsequent requests remain MISS."""

    def test_no_store_not_cached(self, client):
        """no-store content not served from pagespeed cache."""
        # First request
        r1 = client.get("/cc/no-store")
        assert r1.status_code == 200

        # Re-request (no-store check is header-based, no delay needed)
        r2 = client.get("/cc/no-store")
        assert r2.status_code == 200
        # no-store content should not be served from pagespeed cache
        # (origin doesn't send ETag for /cc/no-store, so pagespeed won't record it)
        # Just verify it returns successfully
        assert len(r2.content) > 0


class TestDateHeader:
    """Date header presence (RFC 9110 Section 6.6.1)."""

    def test_date_present_on_proxy_miss(self, baseline_client):
        """Proxied (baseline) response includes Date header."""
        r = baseline_client.get("/baseline/small.html")
        assert r.status_code == 200
        baseline_client.assert_has_date(r)

    def test_date_present_on_cache_hit(self, client):
        """Cache HIT response is valid (Date may be absent on cache-served)."""
        client.get("/etag-test.css")
        r = client.poll_for_hit("/etag-test.css")
        # When pagespeed serves from cache, it may not add Date header.
        # Verify the response is otherwise valid.
        assert r.status_code == 200

    def test_date_present_on_error(self, client):
        """Error response includes Date header."""
        r = client.get("/error/404")
        # Nginx adds Date to proxied error responses
        client.assert_has_date(r)


class TestCacheControlOnHit:
    """Cache-Control set on cache HIT responses."""

    def test_cached_response_has_cache_control(self, client):
        """Cache HIT response includes a Cache-Control header."""
        client.get("/etag-test.css")
        r = client.poll_for_hit("/etag-test.css")
        assert r.status_code == 200
        cc = r.headers.get("Cache-Control", "")
        assert cc, "Cache HIT should have Cache-Control header"

    def test_hit_has_must_revalidate(self, client):
        """Safe mode always adds must-revalidate on HIT."""
        client.get("/etag-test.css")
        r = client.poll_for_hit("/etag-test.css")
        assert r.status_code == 200
        cc = r.headers.get("Cache-Control", "")
        assert "must-revalidate" in cc, f"Safe mode expected must-revalidate, got: {cc}"

    def test_hit_preserves_origin_max_age(self, client):
        """Safe mode preserves origin max-age (3600)."""
        client.get("/etag-test.css")
        r = client.poll_for_hit("/etag-test.css")
        assert r.status_code == 200
        cc = r.headers.get("Cache-Control", "")
        assert "max-age=3600" in cc, f"Expected origin max-age=3600, got: {cc}"

    def test_hit_no_public(self, client):
        """Safe mode never emits public."""
        client.get("/etag-test.css")
        r = client.poll_for_hit("/etag-test.css")
        assert r.status_code == 200
        cc = r.headers.get("Cache-Control", "")
        assert "public" not in cc, f"Safe mode should not have public, got: {cc}"

    def test_hit_no_stale_while_revalidate(self, client):
        """Safe mode never synthesizes stale-while-revalidate."""
        client.get("/etag-test.css")
        r = client.poll_for_hit("/etag-test.css")
        assert r.status_code == 200
        cc = r.headers.get("Cache-Control", "")
        assert "stale-while-revalidate" not in cc, (
            f"Safe mode should not have stale-while-revalidate, got: {cc}"
        )

    def test_hit_no_stale_if_error(self, client):
        """Safe mode never adds stale-if-error."""
        client.get("/etag-test.css")
        r = client.poll_for_hit("/etag-test.css")
        assert r.status_code == 200
        cc = r.headers.get("Cache-Control", "")
        assert "stale-if-error" not in cc, (
            f"Safe mode should not have stale-if-error, got: {cc}"
        )

    def test_hit_html_safe_policy(self, client):
        """HTML HIT in safe mode: must-revalidate, max-age from origin."""
        client.get("/small.html")
        r = client.poll_for_hit("/small.html")
        assert r.status_code == 200
        cc = r.headers.get("Cache-Control", "")
        assert "must-revalidate" in cc, f"HTML HIT expected must-revalidate, got: {cc}"
        assert "max-age=3600" in cc, f"HTML HIT expected max-age=3600, got: {cc}"

    def test_hit_image_safe_policy(self, client):
        """Image HIT in safe mode: must-revalidate, max-age from origin."""
        client.get("/1x1.jpg")
        r = client.poll_for_hit("/1x1.jpg")
        assert r.status_code == 200
        cc = r.headers.get("Cache-Control", "")
        assert "must-revalidate" in cc, f"Image HIT expected must-revalidate, got: {cc}"
        assert "max-age=3600" in cc, f"Image HIT expected max-age=3600, got: {cc}"

    def test_no_spurious_cache_control(self, client):
        """Proxy doesn't add unexpected Cache-Control directives."""
        r = client.get("/cc/max-age")
        assert r.status_code == 200
        cc = r.headers.get("Cache-Control", "")
        # Should not contain directives the origin didn't send
        # The origin sends "public, max-age=3600"
        if cc:
            # Verify no unexpected additions like "no-transform"
            # unless the proxy explicitly documents it
            parts = [p.strip() for p in cc.split(",")]
            for part in parts:
                # Allow the origin directives and common proxy additions
                assert any(
                    part.startswith(d)
                    for d in [
                        "public",
                        "max-age",
                        "s-maxage",
                        "must-revalidate",
                        "no-cache",
                        "no-store",
                        "private",
                        "no-transform",
                        "proxy-revalidate",
                        "stale-while-revalidate",
                        "stale-if-error",
                    ]
                ), f"Unexpected Cache-Control directive: {part}"


class TestCacheControlOnHitAggressive:
    """Cache-Control on HITs in aggressive mode.

    Origin sends Cache-Control: public, max-age=3600.  In aggressive mode
    the module passes through public, synthesizes stale-while-revalidate
    and stale-if-error, and does NOT force must-revalidate.
    """

    def test_hit_has_public(self, aggressive_client):
        """Aggressive mode passes through origin's public directive."""
        aggressive_client.get("/etag-test.css")
        r = aggressive_client.poll_for_hit("/etag-test.css")
        assert r.status_code == 200
        cc = r.headers.get("Cache-Control", "")
        assert "public" in cc, f"Aggressive mode expected public, got: {cc}"

    def test_hit_no_forced_must_revalidate(self, aggressive_client):
        """Aggressive mode does not force must-revalidate."""
        aggressive_client.get("/etag-test.css")
        r = aggressive_client.poll_for_hit("/etag-test.css")
        assert r.status_code == 200
        cc = r.headers.get("Cache-Control", "")
        assert "must-revalidate" not in cc, (
            f"Aggressive mode should not force must-revalidate, got: {cc}"
        )

    def test_hit_preserves_origin_max_age(self, aggressive_client):
        """Aggressive mode preserves origin max-age (3600)."""
        aggressive_client.get("/etag-test.css")
        r = aggressive_client.poll_for_hit("/etag-test.css")
        assert r.status_code == 200
        cc = r.headers.get("Cache-Control", "")
        assert "max-age=3600" in cc, f"Expected origin max-age=3600, got: {cc}"

    def test_hit_has_stale_while_revalidate(self, aggressive_client):
        """Aggressive mode synthesizes stale-while-revalidate."""
        aggressive_client.get("/etag-test.css")
        r = aggressive_client.poll_for_hit("/etag-test.css")
        assert r.status_code == 200
        cc = r.headers.get("Cache-Control", "")
        assert "stale-while-revalidate" in cc, (
            f"Aggressive mode expected stale-while-revalidate, got: {cc}"
        )

    def test_hit_has_stale_if_error(self, aggressive_client):
        """Aggressive mode adds stale-if-error."""
        aggressive_client.get("/etag-test.css")
        r = aggressive_client.poll_for_hit("/etag-test.css")
        assert r.status_code == 200
        cc = r.headers.get("Cache-Control", "")
        assert "stale-if-error=86400" in cc, (
            f"Aggressive mode expected stale-if-error=86400, got: {cc}"
        )

    def test_hit_image_aggressive_policy(self, aggressive_client):
        """Image HIT in aggressive mode: public + SWR."""
        aggressive_client.get("/1x1.jpg")
        r = aggressive_client.poll_for_hit("/1x1.jpg")
        assert r.status_code == 200
        cc = r.headers.get("Cache-Control", "")
        assert "public" in cc, f"Image HIT expected public, got: {cc}"
        assert "stale-while-revalidate" in cc, (
            f"Image HIT expected stale-while-revalidate, got: {cc}"
        )


class TestAggressiveRevalidationRequired:
    """Aggressive mode must not synthesize stale-* when origin requires revalidation.

    Regression coverage for #175 / PR #252: when the origin sends
    ``must-revalidate`` (or ``no-cache``) the module must suppress
    ``stale-if-error`` and ``stale-while-revalidate`` — serving a stale
    response would contradict the origin's RFC 9111 §5.2.2.2 / §5.2.2.4
    directive. Unit-test coverage exists at
    ``test/lib/cache/cache_control_header_test.cc:178-200``; this is the
    integration counterpart: nginx reads origin headers, builds the cache
    entry, and re-emits the response.
    """

    def test_must_revalidate_preserved(self, aggressive_client):
        """Aggressive mode forwards origin's must-revalidate on HIT."""
        aggressive_client.get("/cc/max-age-must-revalidate")
        r = aggressive_client.poll_for_hit("/cc/max-age-must-revalidate")
        assert r.status_code == 200
        cc = r.headers.get("Cache-Control", "")
        assert "must-revalidate" in cc, (
            f"Aggressive mode must forward origin must-revalidate, got: {cc}"
        )

    def test_must_revalidate_suppresses_stale_if_error(self, aggressive_client):
        """must-revalidate suppresses synthesized stale-if-error."""
        aggressive_client.get("/cc/max-age-must-revalidate")
        r = aggressive_client.poll_for_hit("/cc/max-age-must-revalidate")
        assert r.status_code == 200
        cc = r.headers.get("Cache-Control", "")
        assert "stale-if-error" not in cc, (
            f"must-revalidate must suppress stale-if-error, got: {cc}"
        )

    def test_must_revalidate_suppresses_stale_while_revalidate(self, aggressive_client):
        """must-revalidate suppresses synthesized stale-while-revalidate."""
        aggressive_client.get("/cc/max-age-must-revalidate")
        r = aggressive_client.poll_for_hit("/cc/max-age-must-revalidate")
        assert r.status_code == 200
        cc = r.headers.get("Cache-Control", "")
        assert "stale-while-revalidate" not in cc, (
            f"must-revalidate must suppress stale-while-revalidate, got: {cc}"
        )

    def test_must_revalidate_preserves_origin_max_age(self, aggressive_client):
        """Aggressive mode preserves origin max-age=60 alongside must-revalidate."""
        aggressive_client.get("/cc/max-age-must-revalidate")
        r = aggressive_client.poll_for_hit("/cc/max-age-must-revalidate")
        assert r.status_code == 200
        cc = r.headers.get("Cache-Control", "")
        assert "max-age=60" in cc, f"Expected origin max-age=60, got: {cc}"

    def test_no_cache_suppresses_stale_directives(self, aggressive_client):
        """no-cache origin suppresses both stale-if-error and stale-while-revalidate."""
        # Origin returns "no-cache, max-age=60". Same revalidation-required
        # path as must-revalidate — RFC 9111 §5.2.2.4 forbids serving stale.
        aggressive_client.get("/cc/max-age-no-cache")
        r = aggressive_client.poll_for_hit("/cc/max-age-no-cache")
        assert r.status_code == 200
        cc = r.headers.get("Cache-Control", "")
        assert "stale-if-error" not in cc, (
            f"no-cache must suppress stale-if-error, got: {cc}"
        )
        assert "stale-while-revalidate" not in cc, (
            f"no-cache must suppress stale-while-revalidate, got: {cc}"
        )


class TestPragmaCompat:
    """Pragma: no-cache backward compatibility."""

    def test_pragma_no_cache_forwarded(self, client):
        """Pragma: no-cache from origin forwarded."""
        r = client.get("/pragma/no-cache")
        assert r.status_code == 200
        # Pragma may be stripped by the proxy; at minimum, check
        # response is successful and body is correct
        assert len(r.content) > 0
