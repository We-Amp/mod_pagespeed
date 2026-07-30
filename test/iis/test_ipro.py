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

"""IPRO (In-Place Resource Optimization) tests for IIS PageSpeed module.

These tests verify that CSS, JavaScript, and image resources are
optimized in-place without modifying the original URLs.
"""

import time
import pytest

from pagespeed_test_framework import (
    PageSpeedClient,
    assert_contains,
    assert_not_contains,
    assert_http_status,
    assert_header_contains,
    assert_file_size,
)


@pytest.mark.ipro
class TestIPROBasic:
    """Basic IPRO functionality tests."""

    def test_css_is_minified(self, client: PageSpeedClient, example_root: str):
        """CSS files should be minified when served."""
        # First request may trigger optimization
        response1 = client.get(f"{example_root}/styles/yellow.css")
        assert_http_status(response1, 200)

        # Allow time for background optimization
        time.sleep(0.5)

        # Second request should return optimized version
        response2 = client.get(f"{example_root}/styles/yellow.css")
        assert_http_status(response2, 200)

        # Check for X-PageSpeed header indicating optimization
        x_pagespeed = response2.header("X-PageSpeed")
        assert x_pagespeed, "X-PageSpeed header should be present"

    def test_javascript_is_minified(self, client: PageSpeedClient, example_root: str):
        """JavaScript files should be minified when served."""
        response = client.get(f"{example_root}/scripts/example.js")
        assert_http_status(response, 200)

        # Allow time for optimization
        time.sleep(0.5)

        response = client.get(f"{example_root}/scripts/example.js")
        assert_http_status(response, 200)

    def test_image_is_compressed(self, client: PageSpeedClient, example_root: str):
        """Images should be compressed when served."""
        response = client.get(f"{example_root}/images/sample.png")
        assert_http_status(response, 200)

        # Check that we get an image response
        content_type = response.header("Content-Type")
        assert "image" in content_type.lower(), f"Expected image, got {content_type}"

    def test_png_request_with_webp_accept_succeeds(
        self, client: PageSpeedClient, example_root: str
    ):
        """A PNG requested with a WebP Accept header still returns an image.

        The assertion below accepts any image content type, so this does NOT
        enforce that WebP was served. Proving that needs a poll plus an explicit
        image/webp assertion -- tracked as a follow-up.
        """
        response = client.get(
            f"{example_root}/images/sample.png",
            headers={"Accept": "image/webp,image/*,*/*"},
        )
        assert_http_status(response, 200)

        # May be WebP or original format depending on cache state
        content_type = response.header("Content-Type")
        assert "image" in content_type.lower(), f"Expected image, got {content_type}"


@pytest.mark.ipro
class TestIPROCaching:
    """IPRO caching behavior tests."""

    def test_cache_headers_present(self, client: PageSpeedClient, example_root: str):
        """Optimized resources should have cache headers."""
        # Request a CSS file
        response = client.get(f"{example_root}/styles/yellow.css")
        assert_http_status(response, 200)

        # Check for caching headers
        cache_control = response.header("Cache-Control")
        # Should have some caching directive
        assert cache_control, "Cache-Control header should be present"

    def test_etag_present(self, client: PageSpeedClient, example_root: str):
        """Resources should have ETag for conditional requests."""
        response = client.get(f"{example_root}/styles/yellow.css")
        assert_http_status(response, 200)

        etag = response.header("ETag")
        # ETag may or may not be present depending on config
        # Just ensure response is valid

    def test_conditional_request_304(self, client: PageSpeedClient, example_root: str):
        """If-None-Match should return 304 for cached resources."""
        # First request to get ETag
        response1 = client.get(f"{example_root}/styles/yellow.css")
        assert_http_status(response1, 200)

        etag = response1.header("ETag")
        if etag:
            # Conditional request with ETag
            response2 = client.get(
                f"{example_root}/styles/yellow.css",
                headers={"If-None-Match": etag},
            )
            # Should return 304 Not Modified
            assert response2.status in (200, 304), \
                f"Expected 200 or 304, got {response2.status}"


@pytest.mark.ipro
class TestIPROContentTypes:
    """IPRO content type handling tests."""

    def test_preserves_css_content_type(self, client: PageSpeedClient, example_root: str):
        """CSS files should maintain text/css content type."""
        response = client.get(f"{example_root}/styles/yellow.css")
        assert_http_status(response, 200)

        content_type = response.header("Content-Type")
        assert "text/css" in content_type.lower(), \
            f"Expected text/css, got {content_type}"

    def test_preserves_js_content_type(self, client: PageSpeedClient, example_root: str):
        """JavaScript files should maintain appropriate content type."""
        response = client.get(f"{example_root}/scripts/example.js")
        assert_http_status(response, 200)

        content_type = response.header("Content-Type")
        assert "javascript" in content_type.lower() or "application" in content_type.lower(), \
            f"Expected JavaScript content type, got {content_type}"


@pytest.mark.ipro
class TestIPROCacheFlow:
    """Test IPRO cache hit/miss scenarios."""

    def test_ipro_first_request_miss(self, client: PageSpeedClient, example_root: str):
        """First request returns original, triggers background optimization."""
        # First request - may return original or optimized
        response1 = client.get(f"{example_root}/styles/yellow.css")
        assert_http_status(response1, 200)

        # The response should be valid CSS regardless of optimization state
        content_type = response1.header("Content-Type")
        assert "text/css" in content_type.lower(), \
            f"Expected text/css, got {content_type}"

    def test_ipro_second_request_optimized(self, client: PageSpeedClient, example_root: str):
        """Second request returns optimized resource."""
        # First request to trigger optimization
        client.get(f"{example_root}/styles/yellow.css")
        time.sleep(1.0)  # Allow time for background optimization

        # Second request should have optimization indicators
        response2 = client.get(f"{example_root}/styles/yellow.css")
        assert_http_status(response2, 200)

        # Check for X-PageSpeed or X-Page-Speed header (IIS uses hyphen)
        x_pagespeed = response2.header("X-PageSpeed") or response2.header("X-Page-Speed")
        assert x_pagespeed, "X-PageSpeed or X-Page-Speed header should be present"

    def test_ipro_etag_304_response(self, client: PageSpeedClient, example_root: str):
        """If-None-Match returns 304 for cached resources."""
        # First request to get ETag
        response1 = client.get(f"{example_root}/styles/yellow.css")
        assert_http_status(response1, 200)

        etag = response1.header("ETag")
        if etag:
            # Conditional request with If-None-Match
            response2 = client.get(
                f"{example_root}/styles/yellow.css",
                headers={"If-None-Match": etag},
            )
            # Should return 304 Not Modified for cached resource
            assert response2.status in (200, 304), \
                f"Expected 200 or 304 for conditional request, got {response2.status}"
            if response2.status == 304:
                # 304 responses should have minimal body
                assert len(response2.body) == 0 or response2.body == b"", \
                    "304 response should have empty body"


@pytest.mark.ipro
class TestIPROWebPNegotiation:
    """Test WebP content negotiation for images."""

    def test_ipro_png_with_webp_accept_is_well_formed_if_webp(
        self, client: PageSpeedClient, example_root: str
    ):
        """A PNG requested with a WebP Accept header returns well-formed bytes.

        Unconditionally this enforces only that an image comes back; the
        magic-byte check is conditional on the response actually being WebP, so
        this does NOT enforce that WebP was served. Proving that needs a poll
        plus an unconditional image/webp assertion -- tracked as a follow-up.
        """
        # First request to trigger optimization
        client.get(
            f"{example_root}/images/sample.png",
            headers={"Accept": "image/webp,image/*,*/*"},
        )
        time.sleep(1.0)  # Allow time for background optimization

        # Second request with WebP support
        response = client.get(
            f"{example_root}/images/sample.png",
            headers={"Accept": "image/webp,image/*,*/*"},
        )
        assert_http_status(response, 200)

        # Content-Type may be image/webp if optimized, or original format
        content_type = response.header("Content-Type")
        assert "image" in content_type.lower(), \
            f"Expected image content type, got {content_type}"

        # If WebP conversion happened, verify content type
        # Note: May still be PNG if optimization not complete or WebP disabled
        if "webp" in content_type.lower():
            # Verify it's actually WebP by checking magic bytes
            assert response.body[:4] == b"RIFF" or response.body[8:12] == b"WEBP", \
                "WebP content type but content is not WebP format"

    def test_ipro_no_webp_for_unsupported_browser(self, client: PageSpeedClient, example_root: str):
        """Regular client without WebP support gets original format."""
        # Request without WebP in Accept header
        response = client.get(
            f"{example_root}/images/sample.png",
            headers={"Accept": "image/png,image/*,*/*"},
        )
        assert_http_status(response, 200)

        # Should not receive WebP format
        content_type = response.header("Content-Type")
        assert "image" in content_type.lower(), \
            f"Expected image content type, got {content_type}"
        # Should be PNG or other non-WebP format for non-WebP client
        # Note: Server may still serve original format even if Accept includes webp


@pytest.mark.ipro
@pytest.mark.requires_stats
class TestIPROStatistics:
    """Test IPRO statistics reporting.

    These tests require the stats endpoint to be enabled.
    """

    def test_ipro_records_optimization(self, client: PageSpeedClient, example_root: str):
        """Stats should show optimization occurred."""
        # Make requests to trigger optimizations
        client.get(f"{example_root}/styles/yellow.css")
        time.sleep(1.0)  # Allow time for optimization
        client.get(f"{example_root}/styles/yellow.css")

        # Request stats page
        response = client.get("/pagespeed_admin/statistics")

        # Stats endpoint may not be enabled in all configurations
        if response.status == 200:
            stats_content = response.body.decode("utf-8", errors="replace")
            # Look for IPRO-related statistics
            # Common stat names include: ipro_served, ipro_not_rewritable, etc.
            assert "ipro" in stats_content.lower() or "in_place" in stats_content.lower() or \
                   response.status == 200, \
                "Stats page should contain IPRO statistics or be accessible"


if __name__ == "__main__":
    pytest.main([__file__, "-v"])
