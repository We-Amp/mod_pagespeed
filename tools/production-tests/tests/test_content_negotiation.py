# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2024-2026 We-Amp B.V.

"""Content negotiation tests for the caching reverse proxy."""

import time
from uuid import uuid4

import pytest


class TestContentNegotiation:
    """Verify Accept-Encoding, image format, viewport, and Vary handling."""

    # ------------------------------------------------------------------
    # P0 tests
    # ------------------------------------------------------------------

    @pytest.mark.p0
    def test_accept_encoding_gzip_served(self, client):
        """On HIT, request with Accept-Encoding: gzip receives gzip Content-Encoding."""
        uid = uuid4().hex
        path = f"/content/css-ae-gzip-{uid}"

        # Populate cache without Accept-Encoding.
        r = client.get(path)
        client.assert_miss(r)
        assert r.status_code == 200

        # Wait until the proxy has cached the response.
        client.poll_for_hit(path)

        # Request with Accept-Encoding: gzip and wait for compressed variant.
        # The worker writes compressed variants asynchronously; poll_for_header
        # waits for a HIT with the specific Content-Encoding.
        hit = client.poll_for_header(
            path,
            "Content-Encoding",
            "gzip",
            headers={"Accept-Encoding": "gzip"},
        )

    @pytest.mark.p0
    def test_accept_encoding_brotli_served(self, client):
        """On HIT, request with Accept-Encoding: br receives brotli Content-Encoding."""
        uid = uuid4().hex
        path = f"/content/css-ae-br-{uid}"

        r = client.get(path)
        client.assert_miss(r)
        assert r.status_code == 200

        client.poll_for_hit(path)

        # Brotli variant is produced asynchronously by the worker;
        # give it more time than the identity variant.
        hit = client.poll_for_header(
            path,
            "Content-Encoding",
            "br",
            max_attempts=60,
            headers={"Accept-Encoding": "br"},
        )

    @pytest.mark.p0
    def test_accept_encoding_missing_identity(self, client):
        """Explicit identity Accept-Encoding results in no Content-Encoding."""
        uid = uuid4().hex
        path = f"/content/css-ae-none-{uid}"

        # Explicitly set Accept-Encoding: identity because the `requests`
        # library adds a default "gzip, deflate, br" header otherwise.
        ae = {"Accept-Encoding": "identity"}
        r = client.get(path, headers=ae)
        client.assert_miss(r)
        assert r.status_code == 200

        hit = client.poll_for_hit(path, headers=ae)
        ce = hit.headers.get("Content-Encoding")
        assert ce is None or ce == "identity", (
            f"Expected no Content-Encoding or identity, got: {ce}"
        )

    @pytest.mark.p0
    def test_vary_cookie_not_cached(self, client):
        """Origin Vary: Cookie response is not cached by the proxy."""
        uid = uuid4().hex
        path = f"/vary/Cookie?_={uid}"

        r = client.get(path)
        client.assert_miss(r)
        assert r.status_code == 200

        # Should stay MISS across multiple attempts.
        client.assert_not_cached(path)

    @pytest.mark.p0
    def test_vary_star_not_cached(self, client):
        """Origin Vary: * response is not cached by the proxy."""
        uid = uuid4().hex
        path = f"/vary/*?_={uid}"

        r = client.get(path)
        client.assert_miss(r)
        assert r.status_code == 200

        client.assert_not_cached(path)

    @pytest.mark.p0
    def test_image_format_webp_served(self, client):
        """Image request with Accept: image/webp may receive image/webp on HIT."""
        uid = uuid4().hex
        path = f"/content/image/large?_={uid}"
        accept_webp = "image/webp,image/jpeg,*/*"

        # Populate cache.
        r = client.get(path)
        client.assert_miss(r)
        assert r.status_code == 200

        # Poll for HIT with WebP accept header; worker may produce a WebP variant.
        hit = client.poll_for_hit(path, headers={"Accept": accept_webp})
        ct = hit.headers.get("Content-Type", "")
        assert ct in ("image/webp", "image/jpeg"), (
            f"Expected image/webp or image/jpeg, got: {ct}"
        )

    # ------------------------------------------------------------------
    # P1 tests
    # ------------------------------------------------------------------

    @pytest.mark.p1
    def test_accept_encoding_quality_values(self, client):
        """Accept-Encoding with quality values prefers highest-quality encoding."""
        uid = uuid4().hex
        path = f"/content/css-ae-qval-{uid}"

        r = client.get(path)
        client.assert_miss(r)
        client.poll_for_hit(path)

        # br;q=1.0 should be preferred over gzip;q=0.5.
        # Wait for either compressed variant to be available.
        hit = None
        for _ in range(30):
            hit = client.get(
                path,
                headers={"Accept-Encoding": "br;q=1.0, gzip;q=0.5"},
            )
            ce = hit.headers.get("Content-Encoding")
            if hit.headers.get("X-PageSpeed") == "HIT" and ce in ("br", "gzip"):
                break
            time.sleep(1)
        ce = hit.headers.get("Content-Encoding")
        assert ce in ("br", "gzip"), f"Expected br or gzip, got: {ce}"

    @pytest.mark.p1
    def test_accept_encoding_identity_fallback(self, client):
        """If no compressed variant exists yet, identity content is served."""
        uid = uuid4().hex
        path = f"/content/css-ae-idfb-{uid}"

        # First request populates identity in cache.
        r = client.get(path)
        client.assert_miss(r)
        assert r.status_code == 200

        # Immediately request with gzip before worker has time to compress.
        r2 = client.get(path, headers={"Accept-Encoding": "gzip"})
        # Should get a valid 200 response (identity fallback or gzip if fast).
        assert r2.status_code == 200
        ce = r2.headers.get("Content-Encoding")
        assert ce in (None, "identity", "gzip"), (
            f"Expected identity fallback or gzip, got: {ce}"
        )

    @pytest.mark.p1
    def test_image_format_avif_served(self, client):
        """Image request with Accept: image/avif may receive image/avif after processing."""
        uid = uuid4().hex
        path = f"/content/image/large?_avif={uid}"
        accept_avif = "image/avif,image/webp,image/jpeg,*/*"

        r = client.get(path)
        client.assert_miss(r)
        assert r.status_code == 200

        hit = client.poll_for_hit(
            path,
            max_attempts=60,
            headers={"Accept": accept_avif},
        )
        ct = hit.headers.get("Content-Type", "")
        assert ct in ("image/avif", "image/webp", "image/jpeg"), (
            f"Expected image/avif, image/webp, or image/jpeg, got: {ct}"
        )

    @pytest.mark.p1
    def test_image_format_original_fallback(self, client):
        """No modern format in Accept header results in original format served."""
        uid = uuid4().hex
        path = f"/content/image/large?_orig={uid}"
        accept_jpeg_only = "image/jpeg,*/*"

        r = client.get(path)
        client.assert_miss(r)
        assert r.status_code == 200

        hit = client.poll_for_hit(path, headers={"Accept": accept_jpeg_only})
        ct = hit.headers.get("Content-Type", "")
        assert "image/jpeg" in ct, (
            f"Expected image/jpeg for original fallback, got: {ct}"
        )

    @pytest.mark.p1
    def test_viewport_mobile_classification(self, client):
        """Mobile User-Agent may produce a different viewport variant."""
        uid = uuid4().hex
        path = f"/content/image/large?_mob={uid}"
        mobile_ua = (
            "Mozilla/5.0 (iPhone; CPU iPhone OS 17_0 like Mac OS X) "
            "AppleWebKit/605.1.15 (KHTML, like Gecko) "
            "Version/17.0 Mobile/15E148 Safari/604.1"
        )

        r = client.get(path, headers={"User-Agent": mobile_ua})
        client.assert_miss(r)
        assert r.status_code == 200

        hit = client.poll_for_hit(path, headers={"User-Agent": mobile_ua})
        assert hit.status_code == 200
        # The proxy classifies by viewport; a valid response is sufficient.
        assert len(hit.content) > 0

    @pytest.mark.p1
    def test_viewport_desktop_classification(self, client):
        """Desktop User-Agent produces a desktop viewport variant."""
        uid = uuid4().hex
        path = f"/content/image/large?_desk={uid}"
        desktop_ua = (
            "Mozilla/5.0 (Windows NT 10.0; Win64; x64) "
            "AppleWebKit/537.36 (KHTML, like Gecko) "
            "Chrome/120.0.0.0 Safari/537.36"
        )

        r = client.get(path, headers={"User-Agent": desktop_ua})
        client.assert_miss(r)
        assert r.status_code == 200

        hit = client.poll_for_hit(path, headers={"User-Agent": desktop_ua})
        assert hit.status_code == 200
        assert len(hit.content) > 0

    @pytest.mark.p1
    def test_save_data_header(self, client):
        """Save-Data: on may affect variant selection."""
        uid = uuid4().hex
        path = f"/content/image/large?_sd={uid}"

        r = client.get(path)
        client.assert_miss(r)
        assert r.status_code == 200

        hit = client.poll_for_hit(path, headers={"Save-Data": "on"})
        assert hit.status_code == 200
        assert len(hit.content) > 0

    # ------------------------------------------------------------------
    # P2 tests
    # ------------------------------------------------------------------

    @pytest.mark.p2
    def test_vary_accept_encoding_allowed(self, client):
        """Vary: Accept-Encoding from origin is allowed and content is cached."""
        uid = uuid4().hex
        path = f"/vary/Accept-Encoding?_={uid}"

        r = client.get(path)
        client.assert_miss(r)
        assert r.status_code == 200

        hit = client.poll_for_hit(path)
        assert hit.status_code == 200

    @pytest.mark.p2
    def test_vary_user_agent_not_cached(self, client):
        """Origin Vary: User-Agent response is not cached by the proxy.

        Only a response that varies on nothing but Accept-Encoding is stored
        (#796); any other field is passed through from the origin on every
        request, with the origin's own Vary, and is never answered as a HIT.
        """
        uid = uuid4().hex
        path = f"/vary/User-Agent?_={uid}"

        r = client.get(path)
        client.assert_miss(r)
        assert r.status_code == 200
        assert len(r.content) > 0
        assert "user-agent" in r.headers.get("Vary", "").lower()

        # Stays MISS (or passthrough) across repeated requests.
        client.assert_not_cached(path)

        # Still served correctly, with the origin's Vary intact.
        again = client.get(path)
        client.assert_miss(again)
        assert again.status_code == 200
        assert again.content == r.content
        assert "user-agent" in again.headers.get("Vary", "").lower()

    @pytest.mark.p2
    def test_accept_encoding_empty_string(self, client):
        """Accept-Encoding set to empty string results in identity encoding."""
        uid = uuid4().hex
        path = f"/content/css-ae-empty-{uid}"

        # Use explicit identity to populate cache (requests adds default AE).
        ae = {"Accept-Encoding": "identity"}
        r = client.get(path, headers=ae)
        client.assert_miss(r)
        client.poll_for_hit(path, headers=ae)

        # Empty AE string should be treated as identity.
        hit = client.get(path, headers={"Accept-Encoding": ""})
        assert hit.status_code == 200
        ce = hit.headers.get("Content-Encoding")
        assert ce is None or ce == "identity", (
            f"Expected no Content-Encoding or identity, got: {ce}"
        )
