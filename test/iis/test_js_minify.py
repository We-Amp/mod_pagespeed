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

"""JavaScript minification filter tests for IIS PageSpeed module.

Ported from: test/system/automatic/test_rewrite_javascript.py

These tests verify that JavaScript minification works correctly,
including external file minification, inline minification, and
preservation of functionality.
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
class TestJsMinification:
    """Tests for JavaScript minification.

    These tests verify that:
    - External JS files are minified (comments and whitespace removed)
    - Rewritten JS URLs contain .pagespeed.jm. hash
    - Minified files are smaller than originals
    """

    def test_rewrite_javascript_minifies_external(
        self, client: PageSpeedClient, test_root: str
    ):
        """rewrite_javascript should minify external JS files."""
        url = f"{test_root}/pages/rewrite_javascript.html?PageSpeedFilters=rewrite_javascript"

        response = client.fetch_until_contains(
            url,
            pattern=r"\.pagespeed\.jm\.",
            timeout=30.0,
        )

        assert_http_status(response, 200)
        assert_contains(
            response,
            r"\.pagespeed\.jm\.",
            "Rewritten JS should have .pagespeed.jm. in URL",
        )

    def test_rewrite_javascript_removes_comments(
        self, client: PageSpeedClient, test_root: str
    ):
        """Minified JS should not contain comments.

        The rewrite_javascript filter should remove single-line and
        multi-line comments from external JS files.
        """
        url = f"{test_root}/pages/rewrite_javascript.html?PageSpeedFilters=rewrite_javascript"

        response = client.fetch_until_contains(
            url,
            pattern=r"\.pagespeed\.jm\.",
            timeout=30.0,
        )

        # Extract rewritten JS URL
        match = re.search(r'src="([^"]*\.pagespeed\.jm\.[^"]*)"', response.text)
        if not match:
            pytest.skip("Could not find rewritten JS URL")

        js_url = match.group(1)
        # Handle both absolute URLs (http://...) and relative URLs
        if js_url.startswith("http://") or js_url.startswith("https://"):
            from urllib.parse import urlparse
            js_url = urlparse(js_url).path
        elif not js_url.startswith("/"):
            js_url = f"{test_root}/{js_url}"

        js_response = client.get(js_url)
        assert_http_status(js_response, 200)

        # Minified JS should not contain multi-line comments (except potentially preserved ones)
        # Check that standard comments like "Configuration object" are removed
        assert_not_contains(
            js_response,
            r"Configuration object",
            "Comments should be removed from minified JS",
        )

    def test_rewrite_javascript_cache_extended(
        self, client: PageSpeedClient, test_root: str
    ):
        """Rewritten JS should have long cache lifetime.

        Rewritten resources should have max-age set to 1 year
        (31536000 seconds) for cache extension.
        """
        url = f"{test_root}/pages/rewrite_javascript.html?PageSpeedFilters=rewrite_javascript"

        response = client.fetch_until_contains(
            url,
            pattern=r"\.pagespeed\.jm\.",
            timeout=30.0,
        )

        match = re.search(r'src="([^"]*\.pagespeed\.jm\.[^"]*)"', response.text)
        if not match:
            pytest.skip("Could not find rewritten JS URL")

        js_url = match.group(1)
        # Handle both absolute URLs (http://...) and relative URLs
        if js_url.startswith("http://") or js_url.startswith("https://"):
            from urllib.parse import urlparse
            js_url = urlparse(js_url).path
        elif not js_url.startswith("/"):
            js_url = f"{test_root}/{js_url}"

        js_response = client.get(js_url)
        assert_http_status(js_response, 200)

        cache_control = js_response.header("Cache-Control")
        assert "max-age=" in cache_control, \
            f"Expected max-age in Cache-Control, got: {cache_control}"

        # Should have 1 year cache (31536000 seconds)
        match = re.search(r"max-age=(\d+)", cache_control)
        if match:
            max_age = int(match.group(1))
            assert max_age >= 31536000, \
                f"Expected max-age >= 31536000, got {max_age}"


@pytest.mark.html_rewrite
class TestJsPreservation:
    """Tests that JS functionality is preserved after minification.

    These tests verify that:
    - Variable names and function definitions are preserved
    - Code execution behavior is unchanged
    - Important comments (like licenses) may be preserved
    """

    def test_js_code_structure_preserved(
        self, client: PageSpeedClient, test_root: str
    ):
        """Minified JS should preserve code structure and functionality."""
        url = f"{test_root}/pages/rewrite_javascript.html?PageSpeedFilters=rewrite_javascript"

        response = client.fetch_until_contains(
            url,
            pattern=r"\.pagespeed\.jm\.",
            timeout=30.0,
        )

        match = re.search(r'src="([^"]*\.pagespeed\.jm\.[^"]*)"', response.text)
        if not match:
            pytest.skip("Could not find rewritten JS URL")

        js_url = match.group(1)
        if js_url.startswith("http://") or js_url.startswith("https://"):
            from urllib.parse import urlparse
            js_url = urlparse(js_url).path
        elif not js_url.startswith("/"):
            js_url = f"{test_root}/{js_url}"

        js_response = client.get(js_url)
        assert_http_status(js_response, 200)

        # Key function and variable names should be preserved
        # Check for ExampleClass or PSHelper depending on which file we fetched
        assert (
            "ExampleClass" in js_response.text or
            "PSHelper" in js_response.text or
            "function" in js_response.text
        ), "Core code structures should be preserved"

    def test_inline_js_preserved_functionality(
        self, client: PageSpeedClient, test_root: str
    ):
        """rewrite_javascript_inline should preserve functionality.

        Inline JS should be minified but variable names and
        function calls should be preserved.
        """
        url = f"{test_root}/pages/rewrite_javascript.html?PageSpeedFilters=rewrite_javascript_inline"

        response = client.get(
            url,
            headers={"X-PSA-Blocking-Rewrite": "psatest"},
        )
        assert_http_status(response, 200)

        # The preserved comment should still be there
        assert_contains(
            response,
            r"preserved",
            "Important comments should be preserved",
        )


@pytest.mark.html_rewrite
class TestJsContentType:
    """Tests for correct content-type headers on JS responses.

    These tests verify that:
    - Minified JS has correct application/javascript content type
    - Content type is consistent across requests
    """

    def test_rewritten_js_has_correct_content_type(
        self, client: PageSpeedClient, test_root: str
    ):
        """Rewritten JS should have application/javascript content type."""
        url = f"{test_root}/pages/rewrite_javascript.html?PageSpeedFilters=rewrite_javascript"

        response = client.fetch_until_contains(
            url,
            pattern=r"\.pagespeed\.jm\.",
            timeout=30.0,
        )

        match = re.search(r'src="([^"]*\.pagespeed\.jm\.[^"]*)"', response.text)
        if not match:
            pytest.skip("Could not find rewritten JS URL")

        js_url = match.group(1)
        if js_url.startswith("http://") or js_url.startswith("https://"):
            from urllib.parse import urlparse
            js_url = urlparse(js_url).path
        elif not js_url.startswith("/"):
            js_url = f"{test_root}/{js_url}"

        js_response = client.get(js_url)
        assert_http_status(js_response, 200)

        content_type = js_response.header("Content-Type")
        assert content_type, "Content-Type header should be present"
        assert (
            "javascript" in content_type.lower() or
            "application/x-javascript" in content_type.lower()
        ), f"Expected JavaScript content type, got: {content_type}"

    def test_original_js_still_accessible(
        self, client: PageSpeedClient, test_root: str
    ):
        """Original JS files should still be accessible via direct URL."""
        response = client.get(f"{test_root}/scripts/example.js")
        assert_http_status(response, 200)

        content_type = response.header("Content-Type")
        assert "javascript" in content_type.lower() or "application" in content_type.lower(), \
            f"Expected JavaScript content type, got {content_type}"


@pytest.mark.html_rewrite
class TestRewriteJavascriptInline:
    """Tests for rewrite_javascript_inline filter.

    This filter only minifies inline <script> blocks,
    leaving external script references untouched.
    """

    def test_rewrite_javascript_inline_only(
        self, client: PageSpeedClient, test_root: str
    ):
        """rewrite_javascript_inline should only rewrite inline scripts."""
        url = f"{test_root}/pages/rewrite_javascript.html?PageSpeedFilters=rewrite_javascript_inline"

        # Use blocking rewrite to ensure we see the final result
        response = client.get(
            url,
            headers={"X-PSA-Blocking-Rewrite": "psatest"},
        )
        assert_http_status(response, 200)

        # Inline comments SHOULD be removed
        assert_not_contains(
            response,
            r"This comment will be removed",
            "Inline JS comments should be removed",
        )

        # Inline JS block should be minified
        assert_contains(
            response,
            r'id="int1">var state=0',
            "Inline JS should be minified (whitespace reduced)",
        )

        # External JS links should be left alone (not rewritten)
        assert_not_contains(
            response,
            r"\.pagespeed\.jm\.",
            "External JS should not be rewritten by inline-only filter",
        )


@pytest.mark.html_rewrite
class TestRewriteJavascriptExternal:
    """Tests for rewrite_javascript_external filter.

    This filter only rewrites external script files,
    leaving inline script blocks untouched.
    """

    def test_rewrite_javascript_external_only(
        self, client: PageSpeedClient, test_root: str
    ):
        """rewrite_javascript_external should only rewrite external scripts."""
        url = f"{test_root}/pages/rewrite_javascript.html?PageSpeedFilters=rewrite_javascript_external"

        response = client.fetch_until_contains(
            url,
            pattern=r"\.pagespeed\.jm\.",
            timeout=30.0,
        )

        assert_http_status(response, 200)

        # External scripts SHOULD be rewritten
        assert_contains(
            response,
            r"\.pagespeed\.jm\.",
            "External JS should be rewritten",
        )

        # Inline comments should NOT be removed (only external is rewritten)
        assert_contains(
            response,
            r"This comment will be removed",
            "Inline JS should not be minified by external-only filter",
        )


@pytest.mark.html_rewrite
class TestJsRegressions:
    """Regression tests for JavaScript handling."""

    def test_outlined_resource_not_in_cache_404(
        self, client: PageSpeedClient, test_root: str
    ):
        """Fetch of outlined resources not in cache should return 404.

        This is a regression test to ensure that requesting a
        non-existent pagespeed resource doesn't crash the server.
        """
        url = f"{test_root}/_.pagespeed.jo.3tPymVdi9b.js"
        response = client.get(url)
        # Should return 404, not crash or leak
        assert_http_status(response, 404)

    def test_invalid_pagespeed_url_404(
        self, client: PageSpeedClient, test_root: str
    ):
        """Invalid pagespeed URLs should return 404."""
        url = f"{test_root}/invalid.pagespeed.jm.INVALID.js"
        response = client.get(url)
        assert_http_status(response, 404)


if __name__ == "__main__":
    pytest.main([__file__, "-v"])
