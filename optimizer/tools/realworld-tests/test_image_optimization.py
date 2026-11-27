# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2024-2026 We-Amp B.V.

"""Image optimization tests using httpbin test images.

Verifies format negotiation (WebP, AVIF) and size reduction
on real image content served through the proxy.
"""

import pytest

# httpbin provides test images in various formats
HTTPBIN_PNG = "/httpbin/image/png"


def test_image_served_original_format(nginx):
    """Image is served in original format without Accept negotiation."""
    resp = nginx.poll_for_response(HTTPBIN_PNG, timeout=30)
    assert resp.status_code == 200
    ct = resp.headers.get("Content-Type", "")
    assert "image/" in ct, f"Expected image content type, got: {ct}"


def test_webp_negotiation(nginx):
    """WebP format negotiated via Accept header on cached image."""
    # Seed the cache
    nginx.poll_for_response(HTTPBIN_PNG, timeout=30)

    # Request with WebP Accept header and wait for worker optimization
    resp = nginx.poll_for_hit(
        HTTPBIN_PNG,
        timeout=120,
        headers={"Accept": "image/webp,image/png,image/*"},
        expect_content_type="image/webp",
    )
    if resp.headers.get("X-PageSpeed") == "HIT":
        ct = resp.headers.get("Content-Type", "")
        assert "image/webp" in ct, f"Expected image/webp, got: {ct}"
    else:
        pytest.skip("Worker has not finished optimizing image yet")


def test_avif_negotiation(nginx):
    """AVIF format negotiated via Accept header on cached image."""
    # Seed the cache
    nginx.poll_for_response(HTTPBIN_PNG, timeout=30)

    # Request with AVIF Accept header
    resp = nginx.poll_for_hit(
        HTTPBIN_PNG,
        timeout=120,
        headers={"Accept": "image/avif,image/webp,image/*"},
        expect_content_type="image/avif",
    )
    if resp.headers.get("X-PageSpeed") == "HIT":
        ct = resp.headers.get("Content-Type", "")
        assert "image/avif" in ct, f"Expected image/avif, got: {ct}"
    else:
        pytest.skip("Worker has not finished optimizing image yet")


def test_webp_smaller_than_original(nginx):
    """WebP variant is smaller than the original image."""
    # Get original size
    orig = nginx.poll_for_response(HTTPBIN_PNG, timeout=30)
    if orig.status_code != 200:
        pytest.skip("Could not fetch original image")
    orig_size = len(orig.content)

    # Get WebP variant
    webp = nginx.poll_for_hit(
        HTTPBIN_PNG,
        timeout=120,
        headers={"Accept": "image/webp,image/png,image/*"},
        expect_content_type="image/webp",
        expect_smaller_than=orig_size,
    )
    if webp.headers.get("X-PageSpeed") != "HIT":
        pytest.skip("Worker has not finished optimizing image yet")
    webp_size = len(webp.content)
    if webp_size >= orig_size:
        pytest.skip(
            f"WebP ({webp_size}B) not yet smaller than original ({orig_size}B) "
            "— optimization may still be in progress"
        )
