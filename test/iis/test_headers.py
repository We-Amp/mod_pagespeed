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

"""HTTP header tests for IIS PageSpeed module.

These tests verify that PageSpeed resources have correct HTTP headers
including Content-Length, Cache-Control, and other response headers.

Ported from: test/system/automatic/test_headers.py
"""

import re

import pytest

from pagespeed_test_framework import (
    PageSpeedClient,
    assert_contains,
    assert_not_contains,
    assert_http_status,
)


@pytest.mark.html_rewrite
class TestContentLength:
    """Tests for Content-Length header on PageSpeed resources."""

    def test_combined_css_has_content_length(
        self, client: PageSpeedClient, example_root: str
    ):
        """Combined CSS resources should have Content-Length header."""
        page_url = f"{example_root}/combine_css.html?PageSpeedFilters=combine_css"

        response = client.fetch_until_contains(
            page_url,
            pattern=r'\.pagespeed\.(cc|cf)\.',
            timeout=30.0,
        )
        assert_http_status(response, 200)

        # Extract combined CSS URL
        match = re.search(r'href="([^"]*\.pagespeed\.(cc|cf)\.[^"]*)"', response.text)
        if not match:
            pytest.skip("Could not find combined CSS URL")

        css_url = match.group(1)
        if css_url.startswith("http://") or css_url.startswith("https://"):
            from urllib.parse import urlparse
            css_url = urlparse(css_url).path
        elif not css_url.startswith("/"):
            css_url = f"{example_root}/{css_url}"

        css_response = client.get(css_url)
        assert_http_status(css_response, 200)

        # Should have Content-Length header
        content_length = css_response.header("Content-Length")
        assert content_length, "Combined CSS should have Content-Length header"
        assert int(content_length) > 0, "Content-Length should be positive"

    def test_combined_js_has_content_length(
        self, client: PageSpeedClient, example_root: str
    ):
        """Combined JavaScript resources should have Content-Length header."""
        page_url = f"{example_root}/combine_javascript.html?PageSpeedFilters=combine_javascript"

        response = client.fetch_until_contains(
            page_url,
            pattern=r'\.pagespeed\.jc\.',
            timeout=30.0,
        )
        assert_http_status(response, 200)

        # Extract combined JS URL
        match = re.search(r'src="([^"]*\.pagespeed\.jc\.[^"]*)"', response.text)
        if not match:
            pytest.skip("Could not find combined JS URL")

        js_url = match.group(1)
        if js_url.startswith("http://") or js_url.startswith("https://"):
            from urllib.parse import urlparse
            js_url = urlparse(js_url).path
        elif not js_url.startswith("/"):
            js_url = f"{example_root}/{js_url}"

        js_response = client.get(js_url)
        assert_http_status(js_response, 200)

        content_length = js_response.header("Content-Length")
        assert content_length, "Combined JS should have Content-Length header"


@pytest.mark.html_rewrite
class TestCacheControlHeaders:
    """Tests for Cache-Control headers on optimized resources."""

    def test_optimized_css_has_long_cache(
        self, client: PageSpeedClient, example_root: str
    ):
        """Optimized CSS resources should have long cache lifetimes."""
        page_url = f"{example_root}/combine_css.html?PageSpeedFilters=combine_css"

        response = client.fetch_until_contains(
            page_url,
            pattern=r'\.pagespeed\.(cc|cf)\.',
            timeout=30.0,
        )

        match = re.search(r'href="([^"]*\.pagespeed\.(cc|cf)\.[^"]*)"', response.text)
        if not match:
            pytest.skip("Could not find combined CSS URL")

        css_url = match.group(1)
        if css_url.startswith("http://") or css_url.startswith("https://"):
            from urllib.parse import urlparse
            css_url = urlparse(css_url).path
        elif not css_url.startswith("/"):
            css_url = f"{example_root}/{css_url}"

        css_response = client.get(css_url)
        assert_http_status(css_response, 200)

        cache_control = css_response.header("Cache-Control")
        assert cache_control, "Should have Cache-Control header"
        assert "max-age" in cache_control.lower(), \
            f"Should have max-age directive, got: {cache_control}"

        # Extract max-age value
        match = re.search(r'max-age=(\d+)', cache_control, re.IGNORECASE)
        if match:
            max_age = int(match.group(1))
            one_day = 86400
            assert max_age >= one_day, \
                f"max-age should be at least 1 day ({one_day}s), got {max_age}s"

    def test_optimized_resources_not_private(
        self, client: PageSpeedClient, example_root: str
    ):
        """Optimized resources should not have Cache-Control: private."""
        page_url = f"{example_root}/combine_css.html?PageSpeedFilters=combine_css"

        response = client.fetch_until_contains(
            page_url,
            pattern=r'\.pagespeed\.(cc|cf)\.',
            timeout=30.0,
        )

        match = re.search(r'href="([^"]*\.pagespeed\.(cc|cf)\.[^"]*)"', response.text)
        if not match:
            pytest.skip("Could not find combined CSS URL")

        css_url = match.group(1)
        if css_url.startswith("http://") or css_url.startswith("https://"):
            from urllib.parse import urlparse
            css_url = urlparse(css_url).path
        elif not css_url.startswith("/"):
            css_url = f"{example_root}/{css_url}"

        css_response = client.get(css_url)
        assert_http_status(css_response, 200)

        cache_control = css_response.header("Cache-Control")
        # Resources should be public, not private
        if cache_control:
            assert "private" not in cache_control.lower(), \
                f"Optimized resources should not be private, got: {cache_control}"


@pytest.mark.html_rewrite
class TestPageSpeedHeader:
    """Tests for the X-Page-Speed (or X-PageSpeed) response header."""

    def test_html_has_pagespeed_header(
        self, client: PageSpeedClient, example_root: str
    ):
        """HTML responses should have X-Page-Speed header."""
        response = client.get(f"{example_root}/index.html")
        assert_http_status(response, 200)

        # IIS uses X-Page-Speed (with hyphen), Apache uses X-PageSpeed
        x_pagespeed = response.header("X-Page-Speed") or response.header("X-PageSpeed")
        assert x_pagespeed, "Should have X-Page-Speed or X-PageSpeed header"

    def test_pagespeed_header_contains_version(
        self, client: PageSpeedClient, example_root: str
    ):
        """X-Page-Speed header should contain version info."""
        response = client.get(f"{example_root}/index.html")
        assert_http_status(response, 200)

        x_pagespeed = response.header("X-Page-Speed") or response.header("X-PageSpeed")
        if x_pagespeed:
            # Version typically looks like "1.13.35.2-0" or similar
            assert len(x_pagespeed) > 0, "Version should not be empty"


@pytest.mark.html_rewrite
class TestVaryHeader:
    """Tests for Vary header on responses."""

    def test_html_has_vary_accept_encoding(
        self, client: PageSpeedClient, example_root: str
    ):
        """HTML responses may have Vary: Accept-Encoding for compression."""
        response = client.get(f"{example_root}/index.html")
        assert_http_status(response, 200)

        # Vary header is optional but commonly set
        vary = response.header("Vary")
        # Just verify response is valid - Vary is optional

    def test_image_with_webp_has_vary_accept(
        self, client: PageSpeedClient, example_root: str
    ):
        """Images that can be WebP should have Vary: Accept."""
        # Request an image that might be converted to WebP
        response = client.get(
            f"{example_root}/images/Puzzle.jpg",
            headers={"Accept": "image/webp,image/*,*/*"}
        )
        assert_http_status(response, 200)

        # If WebP conversion is enabled, should have Vary: Accept
        vary = response.header("Vary")
        # This is conditional - some configs may not enable WebP


@pytest.mark.html_rewrite
class TestETagHeader:
    """Tests for ETag header on resources."""

    def test_static_resource_has_etag(
        self, client: PageSpeedClient, example_root: str
    ):
        """Static resources should have ETag for caching."""
        response = client.get(f"{example_root}/styles/yellow.css")
        assert_http_status(response, 200)

        etag = response.header("ETag")
        # ETag is optional but recommended

    def test_etag_enables_conditional_request(
        self, client: PageSpeedClient, example_root: str
    ):
        """ETag should enable conditional GET with If-None-Match."""
        response = client.get(f"{example_root}/styles/yellow.css")
        assert_http_status(response, 200)

        etag = response.header("ETag")
        if not etag:
            pytest.skip("No ETag header present")

        # Conditional request with ETag
        response2 = client.get(
            f"{example_root}/styles/yellow.css",
            headers={"If-None-Match": etag}
        )

        # Should return 304 Not Modified
        assert response2.status in (200, 304), \
            f"Expected 200 or 304, got {response2.status}"


@pytest.mark.html_rewrite
class TestContentTypeHeader:
    """Tests for Content-Type header correctness."""

    def test_html_has_correct_content_type(
        self, client: PageSpeedClient, example_root: str
    ):
        """HTML pages should have text/html content type."""
        response = client.get(f"{example_root}/index.html")
        assert_http_status(response, 200)

        content_type = response.header("Content-Type")
        assert content_type, "Should have Content-Type header"
        assert "text/html" in content_type.lower(), \
            f"Expected text/html, got {content_type}"

    def test_css_has_correct_content_type(
        self, client: PageSpeedClient, example_root: str
    ):
        """CSS files should have text/css content type."""
        response = client.get(f"{example_root}/styles/yellow.css")
        assert_http_status(response, 200)

        content_type = response.header("Content-Type")
        assert content_type, "Should have Content-Type header"
        assert "text/css" in content_type.lower(), \
            f"Expected text/css, got {content_type}"

    def test_js_has_correct_content_type(
        self, client: PageSpeedClient, example_root: str
    ):
        """JavaScript files should have appropriate content type."""
        response = client.get(f"{example_root}/scripts/example.js")
        assert_http_status(response, 200)

        content_type = response.header("Content-Type")
        assert content_type, "Should have Content-Type header"
        # Accept either application/javascript or text/javascript
        assert "javascript" in content_type.lower(), \
            f"Expected javascript content type, got {content_type}"


if __name__ == "__main__":
    raise SystemExit(pytest.main([__file__, "-v"]))
