# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2024-2026 We-Amp B.V.

"""Cache-Control directive compliance tests for the caching reverse proxy."""

import time
from uuid import uuid4

import pytest


class TestCacheControl:
    """Verify correct proxy behavior for all Cache-Control directives."""

    # ------------------------------------------------------------------
    # P0: Hard gate for production
    # ------------------------------------------------------------------

    @pytest.mark.p0
    def test_max_age_respected(self, client):
        """Content with max-age=3600 is cached and served as HIT."""
        path = f"/status/200?cache=3600&body=maxage-{uuid4()}"
        r = client.get(path)
        assert r.status_code == 200
        hit = client.poll_for_hit(path)
        client.assert_hit(hit)

    @pytest.mark.p0
    def test_no_store_not_cached(self, client):
        """Responses with no-store must never be served from cache."""
        client.assert_not_cached("/cc/no-store")

    @pytest.mark.p0
    def test_private_not_cached(self, client):
        """Shared cache must not store responses marked private."""
        client.assert_not_cached("/cc/private")

    @pytest.mark.p0
    def test_no_cache_requires_revalidation(self, client):
        """No-cache content must be revalidated on every request."""
        path = f"/cc/no-cache?k={uuid4()}"
        # Every request should be a MISS because the proxy must
        # revalidate with the origin each time.
        for _ in range(3):
            r = client.get(path)
            client.assert_miss(r)
            assert r.status_code == 200

    @pytest.mark.p0
    def test_no_transform_original_format_only(self, client):
        """No-transform forbids content modification; no WebP/AVIF variants."""
        path = "/cc/no-transform"
        # First request to populate the cache
        client.get(path)
        time.sleep(2)
        # Request with Accept: image/webp -- should still get original
        r = client.get(path, headers={"Accept": "image/webp,*/*"})
        ct = r.headers.get("Content-Type", "")
        assert "webp" not in ct, f"no-transform resource was transformed to {ct}"
        assert "avif" not in ct, f"no-transform resource was transformed to {ct}"

    @pytest.mark.p0
    def test_age_header_present_on_hit(self, client):
        """HIT responses must include the Age header."""
        path = f"/status/200?cache=3600&body=age-{uuid4()}"
        client.get(path)
        hit = client.poll_for_hit(path)
        client.assert_hit(hit)
        age = hit.headers.get("Age")
        assert age is not None, "Age header missing on HIT response"
        assert int(age) >= 0

    @pytest.mark.p0
    def test_age_header_increments(self, client):
        """Age header value increases between subsequent HIT responses."""
        path = f"/status/200?cache=3600&body=ageinc-{uuid4()}"
        client.get(path)
        hit1 = client.poll_for_hit(path)
        client.assert_hit(hit1)
        age1 = int(hit1.headers.get("Age", 0))

        time.sleep(3)

        hit2 = client.get(path)
        client.assert_hit(hit2)
        age2 = int(hit2.headers.get("Age", 0))
        assert age2 > age1, f"Age did not increase: {age1} -> {age2}"

    @pytest.mark.p0
    def test_stale_response_not_served_with_must_revalidate(self, client):
        """Must-revalidate prevents serving stale content after max-age expires."""
        path = f"/cc/must-revalidate?max_age=1&k={uuid4()}"
        r = client.get(path)
        assert r.status_code == 200

        # Wait past max-age
        time.sleep(3)

        # After expiry, the proxy must revalidate rather than serve stale
        r2 = client.get(path)
        assert r2.status_code == 200
        # Should be MISS (revalidated) or a fresh response, not a stale HIT
        xps = r2.headers.get("X-PageSpeed")
        # A compliant proxy either revalidates (MISS) or at minimum does
        # not serve stale content -- accept MISS or HIT with Age <= max_age
        if xps == "HIT":
            age = int(r2.headers.get("Age", 0))
            assert age <= 1, f"Stale HIT served with must-revalidate: Age={age}"

    @pytest.mark.p0
    def test_s_maxage_overrides_max_age(self, client):
        """Shared cache uses s-maxage (3600) over max-age (1)."""
        path = f"/cc/s-maxage?s=3600&m=1&k={uuid4()}"
        r = client.get(path)
        assert r.status_code == 200

        # Wait past max-age=1 but within s-maxage=3600
        time.sleep(3)

        # The shared cache should still serve from cache since s-maxage
        # is the controlling directive for shared caches
        hit = client.poll_for_hit(path)
        client.assert_hit(hit)

    @pytest.mark.p0
    def test_max_age_zero_triggers_revalidation(self, client):
        """max-age=0, must-revalidate forces revalidation on every request."""
        path = f"/cc/max-age-zero?k={uuid4()}"
        # First request: MISS (cache is cold).
        r = client.get(path)
        assert r.status_code == 200
        client.assert_miss(r)

        # Wait so the proxy recognizes the content as stale (Age > 0).
        time.sleep(2)

        # Subsequent requests should revalidate (MISS) because
        # max-age=0 + must-revalidate means the content is immediately
        # stale and must not be served without revalidation.
        for _ in range(2):
            r = client.get(path)
            assert r.status_code == 200
            client.assert_miss(r)
            time.sleep(1)

    # ------------------------------------------------------------------
    # P1: Soft gate, review exceptions
    # ------------------------------------------------------------------

    @pytest.mark.p1
    def test_immutable_long_cache(self, client):
        """Immutable content is cached and stays cached without revalidation."""
        path = f"/cc/immutable?max_age=31536000&k={uuid4()}"
        r = client.get(path)
        assert r.status_code == 200
        hit = client.poll_for_hit(path)
        client.assert_hit(hit)
        # Second request still a HIT
        r2 = client.get(path)
        client.assert_hit(r2)

    @pytest.mark.p1
    def test_no_cache_and_max_age_interaction(self, client):
        """no-cache overrides max-age: both present, no-cache wins."""
        # Use /status with custom headers to combine no-cache + max-age
        import json

        custom_headers = json.dumps(
            [{"name": "Cache-Control", "value": "no-cache, max-age=3600"}]
        )
        path = f"/status/200?body=ncma-{uuid4()}&headers={custom_headers}"
        for _ in range(3):
            r = client.get(path)
            assert r.status_code == 200
            # no-cache should force revalidation despite max-age
            client.assert_miss(r)

    @pytest.mark.p1
    def test_request_no_cache_ignored_by_cdn(self, client):
        """CDN ignores client Cache-Control: no-cache (RFC 9111 Section 3.5)."""
        path = f"/status/200?cache=3600&body=reqnc-{uuid4()}"
        # Populate cache
        client.get(path)
        hit = client.poll_for_hit(path)
        client.assert_hit(hit)

        # Client sends no-cache -- CDN/shared cache MAY ignore this per
        # RFC 9111 Section 3.5 and continues to serve from cache.
        r = client.get(
            path,
            headers={"Cache-Control": "no-cache"},
        )
        assert r.status_code == 200
        client.assert_hit(r)

    @pytest.mark.p1
    def test_request_no_store_ignored_by_cdn(self, client):
        """CDN ignores client Cache-Control: no-store (RFC 9111 Section 3.5)."""
        path = f"/status/200?cache=3600&body=reqns-{uuid4()}"
        # Populate cache
        client.get(path)
        hit = client.poll_for_hit(path)
        client.assert_hit(hit)

        # Client sends no-store -- CDN/shared cache MAY ignore this per
        # RFC 9111 Section 3.5 and continues to serve from cache.
        r = client.get(
            path,
            headers={"Cache-Control": "no-store"},
        )
        assert r.status_code == 200
        client.assert_hit(r)

    @pytest.mark.p1
    def test_swr_synthesis(self, client):
        """If proxy synthesizes stale-while-revalidate, it appears in response CC."""
        path = f"/status/200?cache=3600&body=swr-{uuid4()}"
        client.get(path)
        hit = client.poll_for_hit(path)
        client.assert_hit(hit)
        cc = hit.headers.get("Cache-Control", "")
        if "stale-while-revalidate" in cc:
            # Directive present: verify it has a numeric value
            assert "stale-while-revalidate=" in cc, (
                f"stale-while-revalidate present but malformed: {cc}"
            )
        else:
            # If proxy does not synthesize SWR, that is acceptable -- skip
            pytest.skip("Proxy does not synthesize stale-while-revalidate")

    # ------------------------------------------------------------------
    # P2: Rare scenarios, completeness
    # ------------------------------------------------------------------

    @pytest.mark.p2
    def test_request_max_age_zero_ignored_by_cdn(self, client):
        """CDN ignores client Cache-Control: max-age=0 (RFC 9111 Section 3.5)."""
        path = f"/status/200?cache=3600&body=rma0-{uuid4()}"
        # Populate cache
        client.get(path)
        hit = client.poll_for_hit(path)
        client.assert_hit(hit)

        # Client sends max-age=0 -- CDN/shared cache MAY ignore this per
        # RFC 9111 Section 3.5 and continues to serve from cache.
        r = client.get(
            path,
            headers={"Cache-Control": "max-age=0"},
        )
        assert r.status_code == 200
        client.assert_hit(r)

    @pytest.mark.p2
    def test_request_only_if_cached_ignored_by_cdn(self, client):
        """CDN ignores client only-if-cached; proxies to origin normally."""
        path = f"/status/200?cache=3600&body=oic-{uuid4()}"
        # CDN/shared cache MAY ignore only-if-cached per RFC 9111 Section 3.5.
        # The proxy forwards to origin and returns the origin response.
        r = client.get(
            path,
            headers={"Cache-Control": "only-if-cached"},
        )
        assert r.status_code == 200, (
            f"Expected 200 (CDN ignores only-if-cached), got {r.status_code}"
        )

    @pytest.mark.p2
    def test_cache_control_public_cached(self, client):
        """Responses with public, max-age=3600 are cached normally."""
        path = f"/cc/public?max_age=3600&k={uuid4()}"
        r = client.get(path)
        assert r.status_code == 200
        hit = client.poll_for_hit(path)
        client.assert_hit(hit)
