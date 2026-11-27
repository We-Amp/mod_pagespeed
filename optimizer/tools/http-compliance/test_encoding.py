# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2024-2026 We-Amp B.V.

"""Content-Encoding compliance tests.

Verifies Accept-Encoding handling, gzip compression, and Vary:
Accept-Encoding correctness.

RFC 9110 Section 8.4: Content-Encoding.
RFC 9110 Section 12.5.3: Vary header.

Note: Both HIT (cache-served) and MISS (proxied) responses pass through
nginx's full output filter chain, including the gzip module. The HIT path
calls ngx_http_output_filter() which runs gzip/brotli compression.
Gzip tests use the /baseline/ path (pagespeed disabled) to verify nginx's
gzip behavior independently of the pagespeed module.
"""


class TestGzipEncoding:
    """Accept-Encoding: gzip handling through nginx.

    Note: The `requests` library auto-decompresses gzip responses.
    When Content-Encoding: gzip is on the wire, r.content is already
    decompressed. We verify gzip behavior by checking that the
    decompressed content is valid.
    """

    def test_gzip_on_baseline_html(self, baseline_client):
        """Baseline (no pagespeed) serves valid HTML with gzip accepted."""
        r = baseline_client.get(
            "/baseline/small.html", headers={"Accept-Encoding": "gzip"}
        )
        assert r.status_code == 200
        # requests auto-decompresses gzip; verify body is valid HTML
        body = r.content.decode("utf-8", errors="replace")
        assert "<html>" in body.lower() or "<!doctype" in body.lower()

    def test_gzip_on_baseline_css(self, baseline_client):
        """Baseline compresses large CSS (requests decompresses)."""
        r = baseline_client.get(
            "/baseline/large.css", headers={"Accept-Encoding": "gzip"}
        )
        assert r.status_code == 200
        # requests auto-decompresses; verify body is valid CSS
        body = r.content.decode("utf-8", errors="replace")
        assert ".class-0" in body
        assert len(r.content) > 60000

    def test_no_encoding_when_not_accepted(self, baseline_client):
        """No Content-Encoding when Accept-Encoding: identity."""
        r = baseline_client.get(
            "/baseline/small.html", headers={"Accept-Encoding": "identity"}
        )
        assert r.status_code == 200
        ce = r.headers.get("Content-Encoding", "")
        assert ce != "gzip", "Should not gzip without Accept-Encoding: gzip"

    def test_no_double_compression(self, baseline_client):
        """Content not double-compressed (body is valid after decompression)."""
        r = baseline_client.get(
            "/baseline/large.css", headers={"Accept-Encoding": "gzip"}
        )
        assert r.status_code == 200
        # If double-compressed, requests would decompress once and the
        # body would still be gzip data. Check it's valid CSS.
        body = r.content.decode("utf-8", errors="replace")
        assert ".class-0" in body

    def test_pagespeed_miss_may_gzip(self, client):
        """On pagespeed MISS (proxied), response is valid."""
        r = client.get("/nocache.html", headers={"Accept-Encoding": "gzip"})
        # May get 103 or 200
        if r.status_code == 200:
            assert len(r.content) > 0


class TestContentLengthWithEncoding:
    """Content-Length sanity when Content-Encoding is present."""

    def test_compressed_content_length_smaller_than_body(self, baseline_client):
        """When gzip'd, Content-Length (wire size) < decompressed body."""
        r = baseline_client.get(
            "/baseline/large.css", headers={"Accept-Encoding": "gzip"}
        )
        assert r.status_code == 200
        ce = r.headers.get("Content-Encoding", "")
        if "gzip" in ce:
            cl = r.headers.get("Content-Length")
            if cl is not None:
                assert int(cl) < len(r.content), (
                    f"Compressed Content-Length {cl} should be smaller "
                    f"than decompressed body {len(r.content)}"
                )


class TestVaryAcceptEncoding:
    """Vary: Accept-Encoding header."""

    def test_vary_accept_encoding_on_baseline(self, baseline_client):
        """Baseline includes Vary: Accept-Encoding when gzip enabled."""
        r = baseline_client.get(
            "/baseline/large.css", headers={"Accept-Encoding": "gzip"}
        )
        assert r.status_code == 200
        # nginx's gzip_vary on adds Vary: Accept-Encoding
        vary = r.headers.get("Vary", "")
        if r.headers.get("Content-Encoding") == "gzip":
            assert "Accept-Encoding" in vary

    def test_vary_not_duplicated(self, baseline_client):
        """Vary: Accept-Encoding not duplicated."""
        r = baseline_client.get(
            "/baseline/large.css", headers={"Accept-Encoding": "gzip"}
        )
        vary = r.headers.get("Vary", "")
        if vary:
            parts = [v.strip().lower() for v in vary.split(",")]
            ae_count = parts.count("accept-encoding")
            assert ae_count <= 1, f"Duplicate Accept-Encoding in Vary: {vary}"


class TestImageEncodingBypass:
    """Images should not be double-compressed."""

    def test_image_not_gzipped(self, client):
        """Image responses not gzip-compressed (already compressed)."""
        r = client.get("/1x1.jpg", headers={"Accept-Encoding": "gzip"})
        assert r.status_code == 200
        ce = r.headers.get("Content-Encoding", "")
        assert ce != "gzip", "JPEG should not be gzip-compressed"

    def test_png_not_gzipped(self, client):
        """PNG responses not gzip-compressed."""
        r = client.get("/1x1.png", headers={"Accept-Encoding": "gzip"})
        assert r.status_code == 200
        ce = r.headers.get("Content-Encoding", "")
        assert ce != "gzip", "PNG should not be gzip-compressed"


class TestBrotliEncoding:
    """Accept-Encoding: br handling through nginx.

    Tests dynamic brotli compression via ngx_http_brotli_filter_module.
    The requests library with the brotli package auto-decompresses br
    responses, so r.content is always decompressed.
    """

    def test_brotli_on_baseline_css(self, baseline_client):
        """Baseline compresses large CSS with brotli."""
        r = baseline_client.get(
            "/baseline/large.css", headers={"Accept-Encoding": "br"}
        )
        assert r.status_code == 200
        assert r.headers.get("Content-Encoding") == "br"
        body = r.content.decode("utf-8", errors="replace")
        assert ".class-0" in body

    def test_no_brotli_when_not_accepted(self, baseline_client):
        """No brotli when client sends Accept-Encoding: identity."""
        r = baseline_client.get(
            "/baseline/large.css", headers={"Accept-Encoding": "identity"}
        )
        assert r.status_code == 200
        ce = r.headers.get("Content-Encoding", "")
        assert ce != "br"

    def test_brotli_on_pagespeed_miss(self, client):
        """PageSpeed MISS with brotli-capable client gets valid response."""
        r = client.get("/nocache.html", headers={"Accept-Encoding": "br"})
        if r.status_code == 200:
            assert len(r.content) > 0

    def test_brotli_on_pagespeed_hit(self, client):
        """PageSpeed HIT serves pre-compressed brotli variant."""
        r = client.poll_for_hit("/large.css", headers={"Accept-Encoding": "br"})
        assert r.headers.get("Content-Encoding") == "br"
        body = r.content.decode("utf-8", errors="replace")
        assert ".class-0" in body

    def test_no_double_compression_brotli(self, client):
        """Pre-compressed brotli variant not double-compressed."""
        r = client.poll_for_hit("/large.css", headers={"Accept-Encoding": "br"})
        assert r.status_code == 200
        # Content-Encoding must be exactly "br", not "br, br"
        ce = r.headers.get("Content-Encoding", "")
        assert ce == "br", f"Expected 'br', got '{ce}'"
        body = r.content.decode("utf-8", errors="replace")
        assert ".class-0" in body

    def test_vary_on_brotli_baseline(self, baseline_client):
        """Dynamically brotli-compressed baseline includes Vary."""
        r = baseline_client.get(
            "/baseline/large.css", headers={"Accept-Encoding": "br"}
        )
        assert r.status_code == 200
        if r.headers.get("Content-Encoding") == "br":
            vary = r.headers.get("Vary", "")
            assert "Accept-Encoding" in vary

    def test_vary_on_brotli_hit(self, client):
        """Pre-compressed brotli HIT includes Vary: Accept-Encoding."""
        r = client.poll_for_hit("/large.css", headers={"Accept-Encoding": "br"})
        vary = r.headers.get("Vary", "")
        assert "Accept-Encoding" in vary
