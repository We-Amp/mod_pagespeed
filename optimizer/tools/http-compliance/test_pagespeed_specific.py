# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2024-2026 We-Amp B.V.

"""PageSpeed-specific HTTP compliance tests.

Tests unique to the pagespeed module: X-PageSpeed header, body integrity
after modification, format magic bytes, disallow patterns, Save-Data
variants, and Early Hints.
"""

import re


class TestXPageSpeedHeader:
    """X-PageSpeed header presence and values."""

    def test_miss_on_cold_cache(self, client):
        """X-PageSpeed: MISS on first request (cold cache)."""
        r = client.get("/nocache.html")
        if r.status_code == 103:
            return  # requests library limitation with Early Hints
        assert r.status_code == 200
        xs = r.headers.get("X-PageSpeed")
        assert xs in ("MISS", "HIT"), f"Expected MISS or HIT, got: {xs}"

    def test_hit_on_warm_cache(self, client):
        """X-PageSpeed: HIT after cache is warm."""
        # Trigger recording
        client.get("/etag-test.css")
        r = client.poll_for_hit("/etag-test.css")
        client.assert_hit(r)

    def test_no_x_pagespeed_on_error(self, client):
        """X-PageSpeed absent on error responses."""
        r = client.get("/error/404")
        xs = r.headers.get("X-PageSpeed")
        assert xs is None, f"Error should not have X-PageSpeed: {xs}"

    def test_no_x_pagespeed_on_post(self, client):
        """X-PageSpeed absent on POST responses."""
        r = client.post("/echo", data=b"test")
        xs = r.headers.get("X-PageSpeed")
        assert xs is None


class TestBodyIntegrity:
    """Body integrity after pagespeed modification."""

    def test_css_minified_is_valid(self, client):
        """Minified CSS body is syntactically valid (not truncated)."""
        client.get("/etag-test.css")
        r = client.poll_for_hit("/etag-test.css")
        if r.headers.get("X-PageSpeed") == "HIT":
            body = r.content.decode("utf-8", errors="replace")
            # Should contain CSS properties
            assert "color" in body or "margin" in body or "padding" in body

    def test_js_minified_is_valid(self, client):
        """Minified JS body is syntactically valid."""
        client.get("/tiny.js")
        r = client.poll_for_hit("/tiny.js")
        if r.headers.get("X-PageSpeed") == "HIT":
            body = r.content.decode("utf-8", errors="replace")
            # Should still contain function name
            assert "hello" in body

    def test_html_not_truncated(self, client):
        """HTML body is complete (not truncated)."""
        r = client.get("/nocache.html")
        if r.status_code == 103:
            return  # requests library limitation with Early Hints
        assert r.status_code == 200
        body = r.content.decode("utf-8", errors="replace")
        assert "</html>" in body.lower(), "HTML should end with </html>"

    def test_original_body_unchanged_on_miss(self, client, origin_client):
        """Body unchanged on MISS (proxied directly from origin)."""
        r_proxy = client.get("/binary.dat")
        r_origin = origin_client.get("/binary.dat")
        assert r_proxy.status_code == 200
        assert r_origin.status_code == 200
        assert r_proxy.content == r_origin.content, (
            "Binary data should pass through unchanged"
        )

    def test_binary_data_not_corrupted(self, client):
        """Binary data passes through without corruption."""
        r = client.get("/binary.dat")
        assert r.status_code == 200
        assert b"BINARY_TEST_DATA" in r.content


class TestImageMagicBytes:
    """Transcoded images have correct magic bytes."""

    def test_webp_magic_bytes(self, client):
        """WebP response has RIFF/WEBP magic bytes."""
        client.get("/1x1.jpg")
        r = client.poll_for_hit(
            "/1x1.jpg",
            headers={"Accept": "image/webp"},
        )
        if r.headers.get("X-PageSpeed") == "HIT":
            ct = r.headers.get("Content-Type", "")
            if ct == "image/webp":
                assert r.content[:4] == b"RIFF", "WebP should start with RIFF"
                assert r.content[8:12] == b"WEBP", "WebP RIFF type should be WEBP"

    def test_avif_magic_bytes(self, client):
        """AVIF response has ftyp box."""
        client.get("/1x1.png")
        r = client.poll_for_hit(
            "/1x1.png",
            headers={"Accept": "image/avif"},
        )
        if r.headers.get("X-PageSpeed") == "HIT":
            ct = r.headers.get("Content-Type", "")
            if ct == "image/avif":
                assert r.content[4:8] == b"ftyp", "AVIF should have ftyp box"

    def test_jpeg_magic_bytes_on_original(self, client):
        """Original JPEG has SOI marker."""
        r = client.get("/1x1.jpg", headers={"Accept": "image/jpeg"})
        assert r.status_code == 200
        assert r.content[:2] == b"\xff\xd8", "JPEG should start with FFD8"

    def test_gif_magic_bytes(self, client):
        """GIF has correct magic bytes."""
        r = client.get("/1x1.gif")
        assert r.status_code == 200
        assert r.content[:3] == b"GIF", "GIF should start with GIF"

    def test_png_magic_bytes(self, client):
        """PNG has correct magic bytes."""
        r = client.get("/1x1.png", headers={"Accept": "image/png"})
        assert r.status_code == 200
        assert r.content[:4] == b"\x89PNG", "PNG should start with \\x89PNG"


class TestSaveDataVariant:
    """Save-Data header creates different variants."""

    def test_save_data_accepted(self, client):
        """Request with Save-Data: on returns successfully."""
        r = client.get("/1x1.jpg", headers={"Save-Data": "on"})
        assert r.status_code == 200

    def test_save_data_webp(self, client):
        """Save-Data + WebP returns valid response."""
        r = client.get(
            "/1x1.jpg",
            headers={
                "Save-Data": "on",
                "Accept": "image/webp",
            },
        )
        assert r.status_code == 200
        assert len(r.content) > 0


class TestBaselineComparison:
    """Compare pagespeed vs baseline behavior."""

    def test_baseline_no_x_pagespeed(self, baseline_client):
        """Baseline (pagespeed off) has no X-PageSpeed header."""
        r = baseline_client.get("/baseline/small.html")
        assert r.status_code == 200
        xs = r.headers.get("X-PageSpeed")
        assert xs is None, f"Baseline should not have X-PageSpeed: {xs}"

    def test_baseline_same_status(self, client, baseline_client):
        """Pagespeed and baseline return same status codes."""
        r_ps = client.get("/nocache.html")
        r_bl = baseline_client.get("/baseline/nocache.html")
        if r_ps.status_code == 103:
            return  # requests library limitation with Early Hints
        assert r_ps.status_code == r_bl.status_code

    def test_baseline_same_content_type(self, client, baseline_client):
        """Pagespeed and baseline return same Content-Type."""
        r_ps = client.get("/nocache.html")
        r_bl = baseline_client.get("/baseline/nocache.html")
        # Both should be text/html
        ps_ct = r_ps.headers.get("Content-Type", "")
        bl_ct = r_bl.headers.get("Content-Type", "")
        assert ps_ct.split(";")[0] == bl_ct.split(";")[0]


class TestSvgPassthrough:
    """SVG images should pass through unchanged (not raster-transcoded)."""

    def test_svg_content_type_preserved(self, client):
        """SVG requests preserve Content-Type: image/svg+xml."""
        r = client.get("/logo.svg")
        assert r.status_code == 200
        ct = r.headers.get("Content-Type", "")
        assert "svg" in ct.lower(), f"SVG Content-Type lost: {ct}"

    def test_svg_body_unchanged(self, client, origin_client):
        """SVG body bytes are identical to origin."""
        r_proxy = client.get("/logo.svg")
        r_origin = origin_client.get("/logo.svg")
        assert r_proxy.status_code == 200
        assert r_origin.status_code == 200
        assert r_proxy.content == r_origin.content, (
            "SVG must pass through byte-for-byte unchanged"
        )


class TestBinaryPassthrough:
    """Non-web content types pass through unchanged."""

    def test_woff2_passthrough(self, client, origin_client):
        """WOFF2 font files pass through unchanged."""
        r_proxy = client.get("/font.woff2")
        r_origin = origin_client.get("/font.woff2")
        assert r_proxy.status_code == 200, (
            f"WOFF2 proxy should return 200, got {r_proxy.status_code}"
        )
        assert r_origin.status_code == 200, (
            f"WOFF2 origin should return 200, got {r_origin.status_code}"
        )
        assert r_proxy.content == r_origin.content, (
            "WOFF2 font data should pass through unchanged"
        )

    def test_pdf_passthrough(self, client, origin_client):
        """PDF files pass through unchanged."""
        r_proxy = client.get("/document.pdf")
        r_origin = origin_client.get("/document.pdf")
        assert r_proxy.status_code == 200, (
            f"PDF proxy should return 200, got {r_proxy.status_code}"
        )
        assert r_origin.status_code == 200, (
            f"PDF origin should return 200, got {r_origin.status_code}"
        )
        assert r_proxy.content == r_origin.content, (
            "PDF data should pass through unchanged"
        )


class TestLargeFileGracefulDegradation:
    """Files exceeding size limits should be served unmodified, not error."""

    def test_large_html_served_unmodified(self, client):
        """HTML larger than max-html-size should serve 200, not error."""
        r = client.get("/large.html")
        assert r.status_code == 200, (
            f"Large HTML should return 200, got {r.status_code}"
        )
        body = r.content.decode("utf-8", errors="replace")
        assert "</html>" in body.lower(), "Large HTML should be complete"


class TestAsyncCssLoader:
    """The CSP-safe async-CSS loader served at its content-addressed path.

    This is the only coverage of the nginx synthetic-response block that serves
    the loader over real HTTP (the C++/.NET tests assert the bytes, not the
    nginx buffer/header/finalize path). The loader is served at
    /pagespeed_static/async_css.<hash>.js, where <hash> is a compile-time digest
    of the loader JS; it is served UNCONDITIONALLY at preaccess (not gated on a
    page injecting it), so this path MUST return 200 with the loader bytes.

    The path's hash is not hardcoded: the `async_css_loader_path` fixture
    recomputes it from the worker header (single source of truth), so this also
    asserts the content-addressing end-to-end — a wrong path 404s loudly.
    """

    def test_loader_served_200_js(self, client, async_css_loader_path):
        """GET the loader path: 200, JS content-type, CSP-safe body."""
        # The path is content-addressed (has a hex hash segment).
        assert re.fullmatch(
            r"/pagespeed_static/async_css\.[0-9a-f]+\.js", async_css_loader_path
        ), f"loader path is not content-addressed: {async_css_loader_path}"
        r = client.get(async_css_loader_path)
        assert r.status_code == 200, (
            f"Loader must serve 200, got {r.status_code}"
        )
        assert "javascript" in r.headers.get("Content-Type", ""), (
            f"Loader must be JS, got {r.headers.get('Content-Type')}"
        )
        assert r.headers.get("X-Content-Type-Options") == "nosniff"
        # Provenance: served by the pagespeed module, not some other handler.
        assert r.headers.get("X-PageSpeed") == "async-css-loader"
        body = r.content.decode("utf-8", errors="replace")
        assert body, "Loader body must be non-empty"
        # The loader queries our marker; presence guards against truncation.
        assert "data-pagespeed-async" in body
        # CSP-safe: external script only, never an inline onload handler.
        assert "onload=" not in body

    def test_loader_cache_is_immutable_one_year(self, client, async_css_loader_path):
        """Content-addressed path => immutable + 1-year max-age (cache-bustable
        only by a new hash, i.e. a new loader)."""
        r = client.get(async_css_loader_path)
        cc = r.headers.get("Cache-Control", "")
        assert "max-age=31536000" in cc, f"expected 1-year max-age, got: {cc}"
        assert "immutable" in cc, f"hashed path must be immutable, got: {cc}"

    def test_loader_head_no_body(self, client, async_css_loader_path):
        """HEAD returns 200 headers and no body."""
        r = client.head(async_css_loader_path)
        assert r.status_code == 200
        assert "javascript" in r.headers.get("Content-Type", "")
        assert r.content == b""

    def test_loader_served_with_query_string(self, client, async_css_loader_path):
        """A cache-buster query still resolves the loader (nginx r->uri match,
        parity with the .NET Request.Path match)."""
        r = client.get(async_css_loader_path, params={"v": "2"})
        assert r.status_code == 200
        assert r.headers.get("X-PageSpeed") == "async-css-loader"
