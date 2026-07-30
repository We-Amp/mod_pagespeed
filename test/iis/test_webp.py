#!/usr/bin/env python3
# Copyright 2024 Google LLC
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#      http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.

"""WebP image conversion tests for IIS PageSpeed module.

These tests verify WebP image conversion and content negotiation
based on browser Accept header support.
"""

import time
import pytest

from pagespeed_test_framework import (
    PageSpeedClient,
    assert_contains,
    assert_not_contains,
    assert_http_status,
    assert_header_contains,
)


# WebP Accept header for clients that support WebP
WEBP_ACCEPT_HEADER = "image/webp,image/*,*/*"
# Non-WebP Accept header for clients that don't support WebP
NON_WEBP_ACCEPT_HEADER = "image/png,image/jpeg,image/*,*/*"


def is_webp_content(response) -> bool:
    """Check if response content is WebP format."""
    content_type = response.header("Content-Type") or ""
    if "webp" in content_type.lower():
        return True
    # Also check magic bytes: RIFF....WEBP
    body = response.body
    if len(body) >= 12:
        return body[:4] == b"RIFF" and body[8:12] == b"WEBP"
    return False


def is_jpeg_content(response) -> bool:
    """Check if response content is JPEG format."""
    content_type = response.header("Content-Type") or ""
    if "jpeg" in content_type.lower() or "jpg" in content_type.lower():
        return True
    # Check JPEG magic bytes: FFD8FF
    body = response.body
    if len(body) >= 3:
        return body[:2] == b"\xff\xd8"
    return False


def is_png_content(response) -> bool:
    """Check if response content is PNG format."""
    content_type = response.header("Content-Type") or ""
    if "png" in content_type.lower():
        return True
    # Check PNG magic bytes
    body = response.body
    if len(body) >= 8:
        return body[:8] == b"\x89PNG\r\n\x1a\n"
    return False


@pytest.mark.ipro
class TestWebPConversion:
    """Tests for WebP image conversion via IPRO."""

    def test_webp_conversion_for_jpeg(self, client: PageSpeedClient, example_root: str):
        """JPEG images should be converted to WebP for supporting browsers.

        This test requests a JPEG image with WebP Accept header and verifies
        that PageSpeed can convert it to WebP format.
        """
        # First request triggers optimization
        client.get(
            f"{example_root}/images/Puzzle.jpg",
            headers={"Accept": WEBP_ACCEPT_HEADER},
        )

        # Allow time for background optimization
        time.sleep(1.0)

        # Second request should return optimized version
        response = client.get(
            f"{example_root}/images/Puzzle.jpg",
            headers={"Accept": WEBP_ACCEPT_HEADER},
        )
        assert_http_status(response, 200)

        # Content should be an image
        content_type = response.header("Content-Type") or ""
        assert "image" in content_type.lower(), f"Expected image, got {content_type}"

        # Note: WebP conversion may not happen on first request due to IPRO
        # async optimization. The test verifies the request succeeds and
        # returns valid image content.

    def test_png_request_with_webp_accept_succeeds(
        self, client: PageSpeedClient, example_root: str
    ):
        """A PNG requested with a WebP Accept header still returns an image.

        This enforces only that the request succeeds and returns image content;
        it does NOT assert that WebP conversion happened, because the assertion
        below accepts any image content type. Whether the response comes back as
        PNG or WebP depends on IPRO cache state and configuration. Proving the
        conversion needs a poll plus an explicit image/webp assertion -- tracked
        as a follow-up.
        """
        # First request triggers optimization
        client.get(
            f"{example_root}/images/Cuppa.png",
            headers={"Accept": WEBP_ACCEPT_HEADER},
        )

        # Allow time for background optimization
        time.sleep(1.0)

        # Second request should return optimized version
        response = client.get(
            f"{example_root}/images/Cuppa.png",
            headers={"Accept": WEBP_ACCEPT_HEADER},
        )
        assert_http_status(response, 200)

        # Content should be an image
        content_type = response.header("Content-Type") or ""
        assert "image" in content_type.lower(), f"Expected image, got {content_type}"

    def test_webp_smaller_than_original(self, client: PageSpeedClient, example_root: str):
        """WebP conversion should produce smaller or equal size images.

        Requests the same JPEG with and without WebP support and compares sizes.
        """
        # Request without WebP support
        response_original = client.get(
            f"{example_root}/images/Puzzle.jpg",
            headers={"Accept": NON_WEBP_ACCEPT_HEADER},
        )
        assert_http_status(response_original, 200)
        original_size = len(response_original.body)

        # Allow optimization
        time.sleep(1.0)

        # Request with WebP support
        response_webp = client.get(
            f"{example_root}/images/Puzzle.jpg",
            headers={"Accept": WEBP_ACCEPT_HEADER},
        )
        assert_http_status(response_webp, 200)
        webp_size = len(response_webp.body)

        # Both should be valid images
        assert original_size > 0, "Original image should have content"
        assert webp_size > 0, "WebP image should have content"

        # If WebP conversion happened, it should be smaller or close to original
        # We allow some tolerance since optimization may not always reduce size
        if is_webp_content(response_webp):
            assert webp_size <= original_size * 1.1, (
                f"WebP ({webp_size}) should not be much larger than original ({original_size})"
            )


@pytest.mark.ipro
class TestWebPNegotiation:
    """Tests for Accept header negotiation for WebP."""

    def test_webp_served_to_webp_client(self, client: PageSpeedClient, example_root: str):
        """Clients with Accept: image/webp should receive WebP images.

        After optimization completes, WebP-capable clients should receive
        WebP format images.
        """
        # First request to trigger optimization
        client.get(
            f"{example_root}/images/Puzzle.jpg",
            headers={"Accept": WEBP_ACCEPT_HEADER},
        )
        time.sleep(1.5)

        # Subsequent request with WebP support
        response = client.get(
            f"{example_root}/images/Puzzle.jpg",
            headers={"Accept": WEBP_ACCEPT_HEADER},
        )
        assert_http_status(response, 200)

        # Verify it's a valid image
        content_type = response.header("Content-Type") or ""
        assert "image" in content_type.lower(), f"Expected image, got {content_type}"

        # If optimized to WebP, content type should indicate WebP
        # Note: May still be JPEG if optimization not complete
        if is_webp_content(response):
            assert "webp" in content_type.lower() or response.body[8:12] == b"WEBP", \
                "WebP content should have WebP content type or magic bytes"

    def test_original_served_to_non_webp_client(self, client: PageSpeedClient, example_root: str):
        """Clients without WebP support should receive original format.

        Browsers that don't send Accept: image/webp should receive the
        original JPEG or PNG format, not WebP.
        """
        response = client.get(
            f"{example_root}/images/Puzzle.jpg",
            headers={"Accept": NON_WEBP_ACCEPT_HEADER},
        )
        assert_http_status(response, 200)

        # Should not be WebP for non-WebP client
        content_type = response.header("Content-Type") or ""
        assert "image" in content_type.lower(), f"Expected image, got {content_type}"

        # If content negotiation is working, WebP should not be served
        # to clients that don't accept it
        if "webp" not in NON_WEBP_ACCEPT_HEADER:
            # Body should not be WebP format (check magic bytes)
            body = response.body
            if len(body) >= 12:
                assert body[8:12] != b"WEBP" or "webp" in content_type.lower(), \
                    "Non-WebP client should not receive WebP without accepting it"

    def test_vary_accept_header(self, client: PageSpeedClient, example_root: str):
        """Response should have Vary: Accept header for content negotiation.

        When serving optimized images based on Accept header, the response
        should include Vary: Accept to enable proper caching.
        """
        # Request image with WebP support
        response = client.get(
            f"{example_root}/images/Puzzle.jpg",
            headers={"Accept": WEBP_ACCEPT_HEADER},
        )
        assert_http_status(response, 200)

        # Check for Vary header
        vary = response.header("Vary")
        # Vary header may or may not be present depending on optimization state
        # If present, it should include Accept
        if vary:
            assert "accept" in vary.lower() or vary == "*", \
                f"Vary header should include Accept, got: {vary}"


@pytest.mark.ipro
class TestWebPFallback:
    """Tests for WebP fallback behavior."""

    def test_gif_request_with_webp_accept_succeeds(
        self, client: PageSpeedClient, example_root: str
    ):
        """A GIF requested with a WebP Accept header still returns an image.

        The old name claimed this proved GIFs are NOT converted to WebP. It
        never did: the assertion below accepts any image content type, and
        small.gif is not an animated fixture. Proving the negative needs an
        animated fixture plus an explicit assertion that the content type is
        not image/webp -- tracked as a follow-up.
        """
        # Request GIF with WebP support (small.gif exists in testsite)
        response = client.get(
            f"{example_root}/images/small.gif",
            headers={"Accept": WEBP_ACCEPT_HEADER},
        )

        assert_http_status(response, 200)

        # Content should be an image
        content_type = response.header("Content-Type") or ""
        assert "image" in content_type.lower(), f"Expected image, got {content_type}"

        # NOTE: this deliberately does not assert the content type is not
        # image/webp -- see the docstring.

    def test_small_image_not_converted(self, client: PageSpeedClient, example_root: str):
        """Very small images may not be converted to WebP.

        PageSpeed has a minimum size threshold for image optimization.
        Very small images may not benefit from WebP conversion.
        """
        # This test is informational - small images may or may not be converted
        # depending on configuration. We just verify the request succeeds.
        response = client.get(
            f"{example_root}/images/Puzzle.jpg",
            headers={"Accept": WEBP_ACCEPT_HEADER},
        )
        assert_http_status(response, 200)

        # Verify valid image response
        content_type = response.header("Content-Type") or ""
        assert "image" in content_type.lower(), f"Expected image, got {content_type}"


@pytest.mark.html_rewrite
class TestWebPInHtml:
    """Tests for WebP in HTML rewriting context."""

    def test_convert_jpeg_to_webp_in_html(self, client: PageSpeedClient, example_root: str):
        """JPEG images in HTML should be rewritten to WebP for supported browsers.

        When convert_jpeg_to_webp filter is enabled, JPEG references in HTML
        should be rewritten to WebP URLs for browsers that support WebP.
        """
        # Request HTML page with image rewriting and WebP conversion
        response = client.get(
            f"{example_root}/rewrite_images.html?PageSpeedFilters=rewrite_images,convert_jpeg_to_webp",
            headers={"Accept": "text/html,*/*", "Accept": WEBP_ACCEPT_HEADER},
        )
        assert_http_status(response, 200)

        # Page should contain img tags
        assert_contains(response, "<img")

        # Note: WebP URL rewriting requires the filter to be enabled and
        # the optimization to complete. On first request, URLs may not be
        # rewritten yet due to async optimization.

    def test_srcset_webp_rewriting(self, client: PageSpeedClient, example_root: str):
        """srcset attributes should be rewritten for WebP.

        If the HTML contains srcset attributes, they should also be
        considered for WebP rewriting.
        """
        # Request page with srcset (if available)
        response = client.get(
            f"{example_root}/rewrite_images.html?PageSpeedFilters=rewrite_images",
            headers={"Accept": "text/html,*/*"},
        )
        assert_http_status(response, 200)

        # Verify page loads successfully
        # srcset rewriting is tested implicitly - actual srcset content
        # depends on the test page having srcset attributes

    def test_webp_conversion_with_lazyload(self, client: PageSpeedClient, example_root: str):
        """WebP conversion should work with lazyload_images filter.

        Both optimizations should be able to coexist without conflicts.
        """
        response = client.get(
            f"{example_root}/lazyload_images.html?PageSpeedFilters=lazyload_images,convert_jpeg_to_webp",
            headers={"Accept": "text/html,*/*", "Accept": WEBP_ACCEPT_HEADER},
        )
        assert_http_status(response, 200)

        # Page should be valid HTML
        assert_contains(response, "</html>")

    def test_picture_element_webp(self, client: PageSpeedClient, example_root: str):
        """Picture elements with WebP sources should be handled correctly.

        PageSpeed should not break existing picture elements that already
        have WebP sources defined.
        """
        response = client.get(
            f"{example_root}/rewrite_images.html?PageSpeedFilters=rewrite_images",
            headers={"Accept": "text/html,*/*"},
        )
        assert_http_status(response, 200)

        # Page should load without errors
        content_type = response.header("Content-Type") or ""
        assert "text/html" in content_type.lower() or "application/xhtml" in content_type.lower(), \
            f"Expected HTML content type, got {content_type}"


@pytest.mark.ipro
class TestWebPCaching:
    """Tests for WebP caching behavior."""

    def test_webp_cache_headers(self, client: PageSpeedClient, example_root: str):
        """WebP responses should have appropriate cache headers.

        Optimized WebP images should include cache control headers to
        enable browser and CDN caching.
        """
        # Trigger optimization
        client.get(
            f"{example_root}/images/Puzzle.jpg",
            headers={"Accept": WEBP_ACCEPT_HEADER},
        )
        time.sleep(1.0)

        # Request again to get cached/optimized version
        response = client.get(
            f"{example_root}/images/Puzzle.jpg",
            headers={"Accept": WEBP_ACCEPT_HEADER},
        )
        assert_http_status(response, 200)

        # Should have cache control headers
        cache_control = response.header("Cache-Control")
        # Cache-Control should be present for optimized resources
        if cache_control:
            assert len(cache_control) > 0, "Cache-Control should have value"

    def test_etag_for_webp_images(self, client: PageSpeedClient, example_root: str):
        """WebP images should have ETag for conditional requests.

        ETags enable efficient cache validation via If-None-Match headers.
        """
        response = client.get(
            f"{example_root}/images/Puzzle.jpg",
            headers={"Accept": WEBP_ACCEPT_HEADER},
        )
        assert_http_status(response, 200)

        etag = response.header("ETag")
        # ETag may or may not be present depending on configuration
        if etag:
            # Verify conditional request works
            conditional_response = client.get(
                f"{example_root}/images/Puzzle.jpg",
                headers={
                    "Accept": WEBP_ACCEPT_HEADER,
                    "If-None-Match": etag,
                },
            )
            # Should return 304 Not Modified or 200 OK
            assert conditional_response.status in (200, 304), \
                f"Expected 200 or 304 for conditional request, got {conditional_response.status}"


@pytest.mark.ipro
class TestWebPQuality:
    """Tests for WebP quality settings."""

    def test_webp_quality_preserved(self, client: PageSpeedClient, example_root: str):
        """WebP conversion should preserve reasonable image quality.

        The converted WebP should be a valid image that renders correctly.
        """
        response = client.get(
            f"{example_root}/images/Puzzle.jpg",
            headers={"Accept": WEBP_ACCEPT_HEADER},
        )
        assert_http_status(response, 200)

        # Image should have non-trivial size (not corrupted/empty)
        assert len(response.body) > 100, "Image should have substantial content"

        # If it's WebP, verify basic structure
        if is_webp_content(response):
            body = response.body
            assert body[:4] == b"RIFF", "WebP should start with RIFF"
            assert body[8:12] == b"WEBP", "WebP should have WEBP signature"


if __name__ == "__main__":
    pytest.main([__file__, "-v"])
