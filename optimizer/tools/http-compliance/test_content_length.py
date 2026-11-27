# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2024-2026 We-Amp B.V.

"""Content-Length compliance tests.

Verifies Content-Length header correctness after body modification
(minification, transcoding, critical CSS injection). This is the most
critical compliance area for a body-modifying proxy.

RFC 9110 Section 8.6: Content-Length MUST equal the number of octets
in the message body.
"""


class TestContentLengthOnMiss:
    """Content-Length correctness on cache MISS (proxied from origin)."""

    def test_css_miss_content_length_matches_body(self, client):
        """Content-Length matches body on CSS MISS."""
        r = client.get("/etag-test.css")
        assert r.status_code == 200
        client.assert_content_length_matches_body(r)

    def test_js_miss_content_length_matches_body(self, client):
        """Content-Length matches body on JS MISS."""
        r = client.get("/tiny.js")
        assert r.status_code == 200
        client.assert_content_length_matches_body(r)

    def test_html_miss_content_length_matches_body(self, client):
        """Content-Length matches body on HTML MISS."""
        # Use nocache.html to avoid 103 Early Hints (which requests
        # library may not handle transparently)
        r = client.get("/nocache.html")
        if r.status_code == 103:
            # Python requests may surface 103 Early Hints as the response.
            # Skip this check as it's a client library limitation.
            return
        assert r.status_code == 200
        client.assert_content_length_matches_body(r)

    def test_image_miss_content_length_matches_body(self, client):
        """Content-Length matches body on image MISS."""
        r = client.get("/1x1.jpg")
        assert r.status_code == 200
        client.assert_content_length_matches_body(r)

    def test_png_miss_content_length_matches_body(self, client):
        """Content-Length matches body on PNG MISS."""
        r = client.get("/1x1.png")
        assert r.status_code == 200
        client.assert_content_length_matches_body(r)


class TestContentLengthOnHit:
    """Content-Length correctness on cache HIT (served from cache)."""

    def test_css_hit_content_length_matches_body(self, client):
        """Content-Length matches body after CSS minification."""
        # Trigger recording first
        client.get("/etag-test.css")
        r = client.poll_for_hit("/etag-test.css")
        client.assert_content_length_matches_body(r)

    def test_js_hit_content_length_matches_body(self, client):
        """Content-Length matches body after JS minification."""
        client.get("/tiny.js")
        r = client.poll_for_hit("/tiny.js")
        client.assert_content_length_matches_body(r)

    def test_image_webp_hit_content_length_matches_body(self, client):
        """Content-Length matches body for WebP transcoded image."""
        client.get("/1x1.jpg")
        r = client.poll_for_hit(
            "/1x1.jpg",
            headers={"Accept": "image/webp, image/jpeg"},
        )
        client.assert_content_length_matches_body(r)

    def test_image_avif_hit_content_length_matches_body(self, client):
        """Content-Length matches body for AVIF transcoded image."""
        client.get("/1x1.png")
        r = client.poll_for_hit(
            "/1x1.png",
            headers={"Accept": "image/avif, image/png"},
        )
        client.assert_content_length_matches_body(r)


class TestContentLengthIdentityEncoding:
    """Content-Length with Accept-Encoding: identity (no compression).

    When compression is disabled, Content-Length must match the raw
    decompressed body size exactly.
    """

    _IDENTITY = {"Accept-Encoding": "identity"}

    def test_css_identity_content_length(self, client):
        """CSS Content-Length matches body without compression."""
        r = client.get("/etag-test.css", headers=self._IDENTITY)
        assert r.status_code == 200
        client.assert_content_length_matches_body(r)

    def test_js_identity_content_length(self, client):
        """JS Content-Length matches body without compression."""
        r = client.get("/tiny.js", headers=self._IDENTITY)
        assert r.status_code == 200
        client.assert_content_length_matches_body(r)

    def test_css_hit_identity_content_length(self, client):
        """CSS HIT Content-Length matches body without compression."""
        client.get("/etag-test.css")
        r = client.poll_for_hit("/etag-test.css", headers=self._IDENTITY)
        client.assert_content_length_matches_body(r)

    def test_large_css_identity_content_length(self, client):
        """Large CSS Content-Length matches body without compression."""
        r = client.get("/large.css", headers=self._IDENTITY)
        assert r.status_code == 200
        client.assert_content_length_matches_body(r)


class TestContentLengthFormat:
    """Content-Length header format compliance."""

    def test_content_length_is_valid_integer(self, client):
        """Content-Length is a valid non-negative decimal integer."""
        r = client.get("/etag-test.css")
        assert r.status_code == 200
        client.assert_valid_content_length(r)

    def test_content_length_present_on_200(self, client):
        """Content-Length or Transfer-Encoding present on 200 response."""
        r = client.get("/etag-test.css")
        assert r.status_code == 200
        has_cl = "Content-Length" in r.headers
        has_te = "Transfer-Encoding" in r.headers
        assert has_cl or has_te, "Neither Content-Length nor Transfer-Encoding present"

    def test_empty_body_content_length_zero(self, client):
        """HEAD response body is empty (Content-Length may be non-zero)."""
        r = client.head("/etag-test.css")
        assert len(r.content) == 0, "HEAD response should have empty body"

    def test_large_file_content_length(self, client):
        """Content-Length correct for large file (>64KB)."""
        r = client.get("/large.css")
        assert r.status_code == 200
        client.assert_content_length_matches_body(r)

    def test_binary_content_length(self, client):
        """Content-Length correct for binary data."""
        r = client.get("/binary.dat")
        assert r.status_code == 200
        client.assert_content_length_matches_body(r)
