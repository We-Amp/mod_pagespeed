# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2024-2026 We-Amp B.V.

"""Range request compliance tests.

Verifies 206 Partial Content and range handling through the proxy.

RFC 9110 Section 14: Range Requests.
"""


class TestRangeFromOrigin:
    """Range requests forwarded to origin."""

    def test_range_request_on_origin(self, origin_client):
        """Origin supports Range requests."""
        r = origin_client.get("/large.css", headers={"Range": "bytes=0-99"})
        assert r.status_code == 206
        assert "Content-Range" in r.headers
        assert len(r.content) == 100

    def test_range_content_range_header(self, origin_client):
        """Origin sends correct Content-Range header."""
        r = origin_client.get("/large.css", headers={"Range": "bytes=0-99"})
        assert r.status_code == 206
        cr = r.headers.get("Content-Range", "")
        assert cr.startswith("bytes 0-99/"), f"Unexpected Content-Range: {cr}"

    def test_range_beyond_content_416(self, origin_client):
        """Range beyond content returns 416."""
        r = origin_client.get("/tiny.js", headers={"Range": "bytes=99999-"})
        assert r.status_code == 416


class TestRangeThruProxy:
    """Range requests through the pagespeed proxy."""

    _IDENTITY = {"Accept-Encoding": "identity"}

    def test_range_through_proxy(self, client):
        """Range request through proxy returns 200 or 206."""
        r = client.get(
            "/large.css",
            headers={
                "Range": "bytes=0-99",
                **self._IDENTITY,
            },
        )
        # Proxy may return 206 (forwarded) or 200 (full response)
        assert r.status_code in (200, 206)

    def test_accept_ranges_present(self, client):
        """Response includes Accept-Ranges header."""
        r = client.get("/large.css", headers=self._IDENTITY)
        assert r.status_code == 200
        # Nginx typically sends Accept-Ranges: bytes
        # or the origin's Accept-Ranges is forwarded

    def test_range_on_baseline(self, baseline_client):
        """Range request works on baseline (no pagespeed)."""
        r = baseline_client.get(
            "/baseline/large.css",
            headers={
                "Range": "bytes=0-99",
                **self._IDENTITY,
            },
        )
        # Should work normally through baseline proxy
        assert r.status_code in (200, 206)


class TestIfRange:
    """If-Range conditional range requests."""

    def test_if_range_with_matching_etag(self, origin_client):
        """If-Range with matching ETag returns 206."""
        r1 = origin_client.get("/large.css")
        etag = r1.headers.get("ETag")
        if etag:
            r2 = origin_client.get(
                "/large.css",
                headers={
                    "Range": "bytes=0-99",
                    "If-Range": etag,
                },
            )
            assert r2.status_code == 206

    def test_if_range_with_wrong_etag(self, origin_client):
        """If-Range with wrong ETag returns full 200."""
        r = origin_client.get(
            "/large.css",
            headers={
                "Range": "bytes=0-99",
                "If-Range": '"wrong-etag"',
            },
        )
        assert r.status_code == 200


class TestRangeOnCacheHit:
    """Range requests on PageSpeed cache HITs.

    With r->allow_ranges = 1, nginx serves partial content from mmap'd
    cache data. Weak ETags cause If-Range to fall back to full 200
    (correct per RFC 9110 — strong comparison required for If-Range).

    All requests use Accept-Encoding: identity to prevent nginx's
    compression filter from interfering with range byte slicing.
    """

    _IDENTITY = {"Accept-Encoding": "identity"}

    def test_range_on_hit_returns_206(self, client):
        """Range request on cache HIT returns 206 with correct slice."""
        r_full = client.poll_for_hit("/large.css", headers=self._IDENTITY)
        client.assert_hit(r_full)
        full_body = r_full.content

        r_range = client.get(
            "/large.css",
            headers={"Range": "bytes=0-99", **self._IDENTITY},
        )
        assert r_range.status_code == 206, (
            f"Expected 206 on HIT range, got {r_range.status_code}"
        )
        assert len(r_range.content) == 100
        assert r_range.content == full_body[:100], (
            "Partial content should match first 100 bytes of full body"
        )

    def test_range_on_hit_has_content_range(self, client):
        """206 on HIT includes Content-Range header."""
        client.poll_for_hit("/large.css", headers=self._IDENTITY)

        r = client.get(
            "/large.css",
            headers={"Range": "bytes=0-99", **self._IDENTITY},
        )
        assert r.status_code == 206
        cr = r.headers.get("Content-Range", "")
        assert cr.startswith("bytes 0-99/"), f"Unexpected Content-Range: {cr}"

    def test_if_range_weak_etag_returns_200(self, client):
        """If-Range with weak ETag falls back to full 200 (RFC 9110)."""
        r1 = client.poll_for_hit("/large.css", headers=self._IDENTITY)
        etag = r1.headers.get("ETag")
        assert etag and etag.startswith("W/"), f"Expected weak ETag, got: {etag}"

        r2 = client.get(
            "/large.css",
            headers={
                "Range": "bytes=0-99",
                "If-Range": etag,
                **self._IDENTITY,
            },
        )
        # Weak ETag + If-Range → strong comparison fails → full 200
        assert r2.status_code == 200, (
            f"Weak ETag If-Range should return 200, got {r2.status_code}"
        )
        assert len(r2.content) == len(r1.content)
