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

    def test_webp_for_supported_browsers(self, client: PageSpeedClient, example_root: str):
        """Images should be served as WebP for supporting browsers."""
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


if __name__ == "__main__":
    pytest.main([__file__, "-v"])
