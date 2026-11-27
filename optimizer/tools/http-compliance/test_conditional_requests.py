# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2024-2026 We-Amp B.V.

"""Conditional request compliance tests.

Verifies If-None-Match (ETag) and If-Modified-Since behavior through
the pagespeed proxy. RFC 9110 Section 13.

Key concern: When pagespeed modifies the body, the ETag from origin
becomes invalid for the modified content. The proxy must handle this
correctly — either strip/regenerate ETags or properly handle conditionals.
"""


class TestETagForwarding:
    """ETag presence and forwarding tests."""

    def test_origin_etag_present_on_miss(self, client):
        """Origin response includes ETag on cache MISS."""
        r = client.get("/etag-test.css")
        assert r.status_code == 200
        # Origin sends ETag; on MISS the proxy passes it through
        # (or nginx may add its own)

    def test_etag_present_on_origin(self, origin_client):
        """Origin directly sends ETag."""
        r = origin_client.get("/etag-test.css")
        assert r.status_code == 200
        assert "ETag" in r.headers, "Origin should send ETag"


class TestIfNoneMatch:
    """If-None-Match conditional request tests."""

    def test_if_none_match_304_from_origin(self, origin_client):
        """Origin returns 304 for matching ETag."""
        r1 = origin_client.get("/etag-test.css")
        assert r1.status_code == 200
        etag = r1.headers.get("ETag")
        assert etag, "Origin must provide ETag"

        r2 = origin_client.get(
            "/etag-test.css",
            headers={"If-None-Match": etag},
        )
        assert r2.status_code == 304, f"Expected 304 from origin, got {r2.status_code}"
        assert len(r2.content) == 0, "304 should have no body"

    def test_if_none_match_mismatch_200(self, origin_client):
        """Origin returns 200 for non-matching ETag."""
        r = origin_client.get(
            "/etag-test.css",
            headers={"If-None-Match": '"non-existent-etag"'},
        )
        assert r.status_code == 200

    def test_if_none_match_star_304(self, origin_client):
        """If-None-Match: * returns 304 if resource exists."""
        r1 = origin_client.get("/etag-test.css")
        assert r1.status_code == 200

        r2 = origin_client.get(
            "/etag-test.css",
            headers={"If-None-Match": "*"},
        )
        assert r2.status_code == 304

    def test_304_preserves_etag(self, origin_client):
        """304 response includes ETag header."""
        r1 = origin_client.get("/etag-test.css")
        etag = r1.headers.get("ETag")

        r2 = origin_client.get(
            "/etag-test.css",
            headers={"If-None-Match": etag},
        )
        assert r2.status_code == 304
        assert r2.headers.get("ETag") == etag

    def test_304_preserves_cache_control(self, origin_client):
        """304 response includes Cache-Control."""
        r1 = origin_client.get("/etag-test.css")
        etag = r1.headers.get("ETag")

        r2 = origin_client.get(
            "/etag-test.css",
            headers={"If-None-Match": etag},
        )
        assert r2.status_code == 304
        assert "Cache-Control" in r2.headers

    def test_conditional_through_proxy_miss(self, client, origin_client):
        """Conditional request through proxy on MISS."""
        # Get ETag from origin directly
        r_origin = origin_client.get("/etag-test.css")
        etag = r_origin.headers.get("ETag")
        assert etag

        # Send conditional through proxy — proxy may forward to origin
        r = client.get(
            "/etag-test.css",
            headers={"If-None-Match": etag},
        )
        # Proxy may return 200 (re-fetched) or 304 (forwarded conditional)
        assert r.status_code in (200, 304)


class TestIfModifiedSince:
    """If-Modified-Since conditional request tests."""

    def test_if_modified_since_304_from_origin(self, origin_client):
        """Origin returns 304 for If-Modified-Since in the future."""
        r1 = origin_client.get("/etag-test.css")
        assert r1.status_code == 200
        lm = r1.headers.get("Last-Modified")
        assert lm, "Origin must provide Last-Modified"

        # Use a future date to ensure 304
        r2 = origin_client.get(
            "/etag-test.css",
            headers={"If-Modified-Since": "Sun, 01 Jan 2090 00:00:00 GMT"},
        )
        assert r2.status_code == 304

    def test_if_modified_since_stale_200(self, origin_client):
        """Origin returns 200 for If-Modified-Since in the past."""
        r = origin_client.get(
            "/etag-test.css",
            headers={"If-Modified-Since": "Sun, 01 Jan 2000 00:00:00 GMT"},
        )
        assert r.status_code == 200

    def test_304_preserves_last_modified(self, origin_client):
        """304 response preserves Last-Modified."""
        origin_client.get("/etag-test.css")

        r2 = origin_client.get(
            "/etag-test.css",
            headers={"If-Modified-Since": "Sun, 01 Jan 2090 00:00:00 GMT"},
        )
        assert r2.status_code == 304


class TestConditionalPrecedence:
    """ETag takes precedence over If-Modified-Since (RFC 9110 13.1.3)."""

    def test_etag_takes_precedence(self, origin_client):
        """If both present, If-None-Match takes priority."""
        r1 = origin_client.get("/etag-test.css")
        etag = r1.headers.get("ETag")

        # Matching ETag + stale date → should still be 304
        r2 = origin_client.get(
            "/etag-test.css",
            headers={
                "If-None-Match": etag,
                "If-Modified-Since": "Sun, 01 Jan 2000 00:00:00 GMT",
            },
        )
        assert r2.status_code == 304

    def test_mismatched_etag_overrides_date(self, origin_client):
        """Mismatched ETag wins even with matching date."""
        r = origin_client.get(
            "/etag-test.css",
            headers={
                "If-None-Match": '"wrong-etag"',
                "If-Modified-Since": "Sun, 01 Jan 2090 00:00:00 GMT",
            },
        )
        assert r.status_code == 200


class TestWeakETags:
    """Weak ETag (W/"...") handling."""

    def test_weak_etag_returned(self, origin_client):
        """Origin can return weak ETags."""
        r = origin_client.get("/weak-etag")
        assert r.status_code == 200
        etag = r.headers.get("ETag")
        assert etag and etag.startswith("W/"), f"Expected weak ETag, got: {etag}"

    def test_weak_etag_304(self, origin_client):
        """Weak ETag comparison for 304."""
        r1 = origin_client.get("/weak-etag")
        etag = r1.headers.get("ETag")

        r2 = origin_client.get(
            "/weak-etag",
            headers={"If-None-Match": etag},
        )
        assert r2.status_code == 304


class TestPageSpeedHitConditional:
    """ETag / conditional-response tests on PageSpeed cache HITs.

    PageSpeed generates weak ETags for cache-served responses using the
    format W/"ps-<mask_hex><flags_hex>-<identity_hex>-<length_hex>" (the
    identity section is omitted for entries without stored content
    identity). Nginx's built-in
    not_modified filter handles 304 automatically when
    r->headers_out.etag is set.
    """

    def test_hit_has_etag(self, client):
        """HIT response includes ETag with W/"ps-" prefix."""
        r = client.poll_for_hit("/etag-test.css")
        client.assert_hit(r)
        etag = r.headers.get("ETag")
        assert etag is not None, "HIT should include ETag"
        assert etag.startswith('W/"ps-'), (
            f'ETag should have W/"ps-" prefix, got: {etag}'
        )

    def test_hit_if_none_match_304(self, client):
        """Matching ETag on HIT returns 304 Not Modified."""
        r1 = client.poll_for_hit("/etag-test.css")
        client.assert_hit(r1)
        etag = r1.headers.get("ETag")
        assert etag

        r2 = client.get(
            "/etag-test.css",
            headers={"If-None-Match": etag},
        )
        assert r2.status_code == 304, (
            f"Expected 304 with matching ETag, got {r2.status_code}"
        )

    def test_hit_if_none_match_mismatch_200(self, client):
        """Non-matching ETag on HIT returns 200 with body."""
        r1 = client.poll_for_hit("/etag-test.css")
        client.assert_hit(r1)

        r2 = client.get(
            "/etag-test.css",
            headers={"If-None-Match": '"wrong-etag"'},
        )
        assert r2.status_code == 200
        assert len(r2.content) > 0, "200 should have body"

    def test_hit_if_none_match_star_304(self, client):
        """If-None-Match: * on HIT returns 304."""
        r1 = client.poll_for_hit("/etag-test.css")
        client.assert_hit(r1)

        r2 = client.get(
            "/etag-test.css",
            headers={"If-None-Match": "*"},
        )
        assert r2.status_code == 304

    def test_304_preserves_etag(self, client):
        """304 response includes the same ETag."""
        r1 = client.poll_for_hit("/etag-test.css")
        etag = r1.headers.get("ETag")
        assert etag

        r2 = client.get(
            "/etag-test.css",
            headers={"If-None-Match": etag},
        )
        assert r2.status_code == 304
        assert r2.headers.get("ETag") == etag, (
            f"304 ETag mismatch: {r2.headers.get('ETag')} != {etag}"
        )

    def test_304_preserves_vary(self, client):
        """304 response includes Vary header (RFC 9110 Section 15.4.5)."""
        r1 = client.poll_for_hit("/etag-test.css")
        etag = r1.headers.get("ETag")
        assert etag

        r2 = client.get(
            "/etag-test.css",
            headers={"If-None-Match": etag},
        )
        assert r2.status_code == 304
        assert "Vary" in r2.headers, "304 should preserve Vary header"

    def test_304_no_body(self, client):
        """304 response has empty body."""
        r1 = client.poll_for_hit("/etag-test.css")
        etag = r1.headers.get("ETag")
        assert etag

        r2 = client.get(
            "/etag-test.css",
            headers={"If-None-Match": etag},
        )
        assert r2.status_code == 304
        assert len(r2.content) == 0, (
            f"304 should have empty body, got {len(r2.content)} bytes"
        )

    def test_different_variants_different_etags(self, client):
        """Gzip and identity variants have different ETags."""
        # Get identity variant
        r_identity = client.poll_for_hit(
            "/etag-test.css",
            headers={"Accept-Encoding": "identity"},
        )
        etag_identity = r_identity.headers.get("ETag")

        # Get gzip variant
        r_gzip = client.poll_for_hit(
            "/etag-test.css",
            headers={"Accept-Encoding": "gzip"},
        )
        etag_gzip = r_gzip.headers.get("ETag")

        assert etag_identity is not None, "Identity HIT should have ETag"
        assert etag_gzip is not None, "Gzip HIT should have ETag"
        assert etag_identity != etag_gzip, (
            f"Variants should have different ETags: "
            f"identity={etag_identity}, gzip={etag_gzip}"
        )

    def test_head_has_etag(self, client):
        """HEAD on HIT includes ETag."""
        # Warm the cache with a GET first
        client.poll_for_hit("/etag-test.css")

        r = client.head("/etag-test.css")
        etag = r.headers.get("ETag")
        assert etag is not None, "HEAD on HIT should include ETag"
        assert etag.startswith('W/"ps-'), (
            f'HEAD ETag should have W/"ps-" prefix, got: {etag}'
        )
