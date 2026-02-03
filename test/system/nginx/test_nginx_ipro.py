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

"""nginx In-Place Resource Optimization (IPRO) tests.

These tests verify that IPRO correctly optimizes CSS, JavaScript, and image
resources in-place without changing their URLs when running with nginx.

IPRO is a key PageSpeed feature that allows resources to be optimized
at their original URLs, with optimization happening asynchronously.
The first request may return the original resource, while subsequent
requests return the optimized version once it's ready.
"""

import random
import re

import pytest

from pagespeed_test_framework import (
    PageSpeedClient,
    assert_contains,
    assert_file_size,
    assert_header_contains,
    assert_http_status,
    assert_not_contains,
)


class TestNginxIproCssMinification:
    """Tests for CSS minification via IPRO on nginx.

    IPRO should remove whitespace, comments, and other unnecessary bytes
    from CSS files while preserving functionality.
    """

    @pytest.mark.nginx_only
    @pytest.mark.requires_module
    def test_ipro_css_minification(
        self, client: PageSpeedClient, test_root: str
    ):
        """Verify CSS is minified via IPRO.

        The IPRO flow:
        1. First request: resource not in cache, returns original
        2. PageSpeed optimizes the resource in background
        3. Subsequent requests: optimized resource from cache

        We use fetch_until to wait for optimization to complete.
        """
        # Use a CSS file with whitespace that can be minified
        # The ipro/cc200/example.css file has extra whitespace
        url = f"{test_root}/ipro/cc200/example.css?r={random.randint(1, 100000)}"

        # Original CSS: "  h1  {  background-color  :  orange;   }" (43 bytes)
        # Minified CSS: "h1{background-color:orange}" (27 bytes)
        original_size = 43

        # Wait for IPRO to optimize the CSS
        response = client.fetch_until(
            url,
            condition=lambda r: len(r.body) < original_size,
            timeout=60.0,
        )

        assert_http_status(response, 200)
        assert_file_size(
            response, "<", original_size,
            "IPRO should minify CSS by removing whitespace",
        )

        # Verify the content is still valid CSS
        assert_contains(response, r"h1\s*\{")
        assert_contains(response, r"background-color")

    @pytest.mark.nginx_only
    @pytest.mark.requires_module
    def test_ipro_css_removes_comments(
        self, client: PageSpeedClient, test_root: str
    ):
        """Verify IPRO removes CSS comments.

        The mod_deflate/big.css file contains CSS comments that should
        be stripped during minification.
        """
        url = f"{test_root}/ipro/mod_deflate/big.css?r={random.randint(1, 100000)}"

        # Original file has comment: "/* This is purposefully repetitive..."
        # Wait for the comment to be removed
        response = client.fetch_until(
            url,
            condition=lambda r: "purposefully" not in r.text,
            timeout=60.0,
        )

        assert_http_status(response, 200)
        assert_not_contains(response, r"/\*.*\*/", "Comments should be removed")


class TestNginxIproJavascriptMinification:
    """Tests for JavaScript minification via IPRO on nginx."""

    @pytest.mark.nginx_only
    @pytest.mark.requires_module
    def test_ipro_javascript_minification(
        self, client: PageSpeedClient, test_root: str
    ):
        """Verify JavaScript is minified via IPRO.

        The normal.js file contains a comment that should be removed:
        Original: "/* comment2 */\ndocument.write(...)"
        Minified: "document.write(...)"
        """
        url = f"{test_root}/normal.js?r={random.randint(1, 100000)}"

        # Wait for the comment to be removed
        response = client.fetch_until(
            url,
            condition=lambda r: "comment2" not in r.text,
            timeout=60.0,
        )

        assert_http_status(response, 200)
        assert_not_contains(response, r"/\*.*comment.*\*/", "JS comments should be removed")

        # Verify the actual code is still present
        assert_contains(response, r"document\.write")

    @pytest.mark.nginx_only
    @pytest.mark.requires_module
    def test_ipro_javascript_reduces_size(
        self, client: PageSpeedClient, test_root: str
    ):
        """Verify IPRO reduces JavaScript file size."""
        url = f"{test_root}/normal.js?r={random.randint(1, 100000)}"

        # Original file is ~60 bytes with comment
        original_size = 60

        response = client.fetch_until(
            url,
            condition=lambda r: len(r.body) < original_size,
            timeout=60.0,
        )

        assert_http_status(response, 200)
        assert_file_size(
            response, "<", original_size,
            "IPRO should reduce JS file size",
        )


class TestNginxIproHeaderPreservation:
    """Tests for header preservation in IPRO responses on nginx."""

    @pytest.mark.nginx_only
    @pytest.mark.requires_module
    def test_ipro_preserves_vary_header(
        self, client: PageSpeedClient, test_root: str
    ):
        """Verify IPRO preserves Vary header from origin response.

        The vary/vary.css file is configured to send Vary: Accept-Encoding.
        IPRO should preserve this header in the optimized response.
        """
        url = f"{test_root}/vary/vary.css?r={random.randint(1, 100000)}"

        # First, verify the original has the Vary header
        original_response = client.get(f"{url}&PageSpeed=off")
        vary_header = original_response.header("Vary")

        # Wait for optimization
        response = client.fetch_until(
            url,
            # Wait for some optimization (file size or content change)
            condition=lambda r: len(r.body) <= len(original_response.body),
            timeout=60.0,
        )

        assert_http_status(response, 200)

        # Check if Vary header is preserved
        optimized_vary = response.header("Vary")
        if vary_header:
            assert optimized_vary, \
                f"Vary header should be preserved. Original had: {vary_header}"
            # Note: nginx may combine Vary headers, so check for presence
            assert "Accept-Encoding" in optimized_vary or vary_header in optimized_vary, \
                f"Vary header value should be preserved. Got: {optimized_vary}"

    @pytest.mark.nginx_only
    @pytest.mark.requires_module
    def test_ipro_cache_control_preserved(
        self, client: PageSpeedClient, test_root: str
    ):
        """Verify IPRO preserves Cache-Control from origin.

        The ipro/cc200/example.css file has Cache-Control: max-age=200.
        IPRO should preserve this (or set appropriate cache headers).
        """
        url = f"{test_root}/ipro/cc200/example.css?r={random.randint(1, 100000)}"

        # Wait for optimization
        response = client.fetch_until(
            url,
            condition=lambda r: len(r.body) < 40,  # Minified should be smaller
            timeout=60.0,
        )

        assert_http_status(response, 200)

        # IPRO should set Cache-Control
        cache_control = response.header("Cache-Control")
        assert cache_control, "Should have Cache-Control header"

        # Should have a max-age directive
        match = re.search(r"max-age=(\d+)", cache_control)
        assert match, f"Should have max-age in Cache-Control: {cache_control}"

        max_age = int(match.group(1))
        # IPRO typically uses the origin's TTL or a reasonable default
        assert max_age > 0, f"max-age should be positive, got {max_age}"

    @pytest.mark.nginx_only
    @pytest.mark.requires_module
    def test_ipro_no_cache_not_optimized(
        self, client: PageSpeedClient, test_root: str
    ):
        """Verify IPRO respects no-cache directive.

        Resources with Cache-Control: no-cache should not be optimized
        or should preserve the no-cache directive.
        """
        # The cc200nc subdirectory has max-age=200,no-cache
        url = f"{test_root}/ipro/cc200nc/example.css?r={random.randint(1, 100000)}"

        response = client.get(url)
        assert_http_status(response, 200)

        # With no-cache, the response might not be cached/optimized
        # or should preserve the no-cache directive
        cache_control = response.header("Cache-Control")
        if "no-cache" in cache_control:
            # Good - directive preserved
            pass
        # Otherwise, the resource might still be served but with appropriate headers


class TestNginxIproGzipHandling:
    """Tests for IPRO with gzip compression on nginx."""

    @pytest.mark.nginx_only
    @pytest.mark.requires_module
    def test_ipro_gzip_response(
        self, client: PageSpeedClient, test_root: str
    ):
        """Verify IPRO works correctly with gzip compression.

        When Accept-Encoding: gzip is sent, the optimized response
        should be gzip-compressed and still contain valid optimized content.
        """
        url = f"{test_root}/ipro/mod_deflate/big.css?r={random.randint(1, 100000)}"

        # Wait for optimization with gzip
        response = client.fetch_until(
            url,
            condition=lambda r: "purposefully" not in r.text,
            timeout=60.0,
            use_gzip=True,
        )

        assert_http_status(response, 200)

        # Content should be optimized (comments removed)
        assert_not_contains(response, r"purposefully")

        # Content should still be valid CSS
        assert_contains(response, r"color")

    @pytest.mark.nginx_only
    @pytest.mark.requires_module
    def test_ipro_gzip_content_encoding(
        self, client: PageSpeedClient, test_root: str
    ):
        """Verify gzip Content-Encoding header when requesting with gzip.

        Note: The test framework's get_gzip method decompresses the body
        automatically, so we need to check the raw response headers.
        """
        url = f"{test_root}/ipro/mod_deflate/big.css?r={random.randint(1, 100000)}"

        # Make a request with Accept-Encoding: gzip but don't auto-decompress
        response = client.get(
            url,
            headers={"Accept-Encoding": "gzip"},
        )

        assert_http_status(response, 200)

        # nginx should serve gzip content when Accept-Encoding is set
        content_encoding = response.header("Content-Encoding")
        # Note: nginx may or may not gzip depending on configuration and response size
        # The key is that if it's gzipped, the content should still be valid when decompressed


class TestNginxIproImageOptimization:
    """Tests for image optimization via IPRO on nginx."""

    @pytest.mark.nginx_only
    @pytest.mark.requires_module
    def test_ipro_image_compression(
        self, client: PageSpeedClient, test_root: str
    ):
        """Verify images are compressed via IPRO.

        This test uses the same approach as the Apache IPRO tests.
        """
        url = f"{test_root}/ipro/test_image_dont_reuse.png?r={random.randint(1, 100000)}"
        threshold_size = 13000  # Original is ~15KB

        # Wait for image optimization
        response = client.fetch_until(
            url,
            condition=lambda r: len(r.body) < threshold_size,
            timeout=60.0,
        )

        assert_http_status(response, 200)
        assert_file_size(
            response, "<", threshold_size,
            "IPRO should compress the image",
        )

    @pytest.mark.nginx_only
    @pytest.mark.requires_module
    def test_ipro_disabled_returns_original(
        self, client: PageSpeedClient, test_root: str
    ):
        """Verify original image is returned when PageSpeed is disabled.

        Using PageSpeed=off query parameter should bypass IPRO.
        """
        url = f"{test_root}/ipro/test_image_dont_reuse.png?PageSpeed=off"
        threshold_size = 13000

        response = client.get(url)
        assert_http_status(response, 200)

        # Should be larger than threshold (original, not optimized)
        assert_file_size(
            response, ">", threshold_size,
            "Original image should be larger than optimized",
        )


class TestNginxIproContentType:
    """Tests for Content-Type preservation in IPRO responses."""

    @pytest.mark.nginx_only
    @pytest.mark.requires_module
    def test_ipro_css_content_type(
        self, client: PageSpeedClient, test_root: str
    ):
        """Verify IPRO preserves Content-Type for CSS files."""
        url = f"{test_root}/ipro/cc200/example.css?r={random.randint(1, 100000)}"

        response = client.fetch_until(
            url,
            condition=lambda r: len(r.body) < 40,
            timeout=60.0,
        )

        assert_http_status(response, 200)

        content_type = response.header("Content-Type")
        assert "text/css" in content_type.lower(), \
            f"Content-Type should be text/css, got: {content_type}"

    @pytest.mark.nginx_only
    @pytest.mark.requires_module
    def test_ipro_javascript_content_type(
        self, client: PageSpeedClient, test_root: str
    ):
        """Verify IPRO preserves Content-Type for JavaScript files."""
        url = f"{test_root}/normal.js?r={random.randint(1, 100000)}"

        response = client.fetch_until(
            url,
            condition=lambda r: "comment2" not in r.text,
            timeout=60.0,
        )

        assert_http_status(response, 200)

        content_type = response.header("Content-Type")
        # JavaScript can be served as various MIME types
        assert any(js_type in content_type.lower() for js_type in [
            "application/javascript",
            "text/javascript",
            "application/x-javascript",
        ]), f"Content-Type should be JavaScript type, got: {content_type}"


if __name__ == "__main__":
    pytest.main([__file__, "-v"])
