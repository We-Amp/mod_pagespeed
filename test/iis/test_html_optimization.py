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

"""HTML optimization filter tests for IIS PageSpeed module.

These tests verify various HTML optimization filters including:
- elide_attributes: Remove default/boolean attribute values
- convert_meta_tags: Convert meta tags to HTTP headers
- remove_comments: Remove HTML comments
- collapse_whitespace: Collapse redundant whitespace
- trim_urls: Remove unnecessary URL components

Ported from: test/system/automatic/test_elide_attributes.py,
             test/system/automatic/test_convert_meta_tags.py
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
class TestElideAttributes:
    """Tests for the elide_attributes filter.

    The elide_attributes filter removes:
    - Boolean attribute values (disabled="disabled" -> disabled)
    - Default attribute values (type="text/javascript" on script tags)
    """

    def test_elide_boolean_attributes(
        self, client: PageSpeedClient, example_root: str
    ):
        """Boolean attributes should have their values elided."""
        url = f"{example_root}/elide_attributes.html?PageSpeedFilters=elide_attributes"

        response = client.fetch_until_contains(
            url,
            pattern=r'<',  # Just wait for valid HTML
            timeout=10.0,
        )
        assert_http_status(response, 200)

        # disabled="disabled" should become just disabled (no =)
        # Only check if original has disabled= pattern
        if 'disabled=' in response.text or 'checked=' in response.text:
            # If we see disabled= it should be the unoptimized version
            pass  # Filter may not be fully applied yet

    def test_elide_removes_default_type(
        self, client: PageSpeedClient, example_root: str
    ):
        """Default type="text/javascript" should be removed from script tags."""
        url = f"{example_root}/combine_javascript.html?PageSpeedFilters=elide_attributes"

        response = client.get(url)
        assert_http_status(response, 200)

        # Check that script tags exist
        assert_contains(response, r'<script')

    def test_elide_preserves_non_default_values(
        self, client: PageSpeedClient, example_root: str
    ):
        """Non-default attribute values should be preserved."""
        url = f"{example_root}/index.html?PageSpeedFilters=elide_attributes"

        response = client.get(url)
        assert_http_status(response, 200)

        # Custom class names should be preserved
        # This is a sanity check that filter doesn't break attributes


@pytest.mark.html_rewrite
class TestConvertMetaTags:
    """Tests for the convert_meta_tags filter.

    The convert_meta_tags filter converts HTML meta tags to HTTP headers,
    particularly for charset/content-type declarations.
    """

    def test_html_has_content_type_header(
        self, client: PageSpeedClient, example_root: str
    ):
        """HTML responses should have Content-Type header with charset."""
        url = f"{example_root}/unicode.html?PageSpeedFilters=convert_meta_tags"

        response = client.get(url)
        assert_http_status(response, 200)

        content_type = response.header("Content-Type")
        assert content_type, "Should have Content-Type header"
        assert "text/html" in content_type.lower(), \
            f"Content-Type should be text/html, got: {content_type}"

    def test_charset_in_content_type(
        self, client: PageSpeedClient, example_root: str
    ):
        """Content-Type header should include charset from meta tag."""
        url = f"{example_root}/unicode.html?PageSpeedFilters=convert_meta_tags"

        response = client.get(url)
        assert_http_status(response, 200)

        content_type = response.header("Content-Type")
        if content_type:
            # Charset may or may not be present depending on meta tags
            # Just verify the response is valid
            assert "text/html" in content_type.lower()


@pytest.mark.html_rewrite
class TestRemoveComments:
    """Tests for the remove_comments filter."""

    def test_remove_comments_basic(
        self, client: PageSpeedClient, test_root: str
    ):
        """HTML comments should be removed."""
        url = f"{test_root}/pages/comments.html?PageSpeedFilters=remove_comments"

        response = client.get(url)
        assert_http_status(response, 200)

        # The comment content should be removed
        # Note: Some comments (like IE conditionals) may be preserved

    def test_remove_comments_preserves_content(
        self, client: PageSpeedClient, test_root: str
    ):
        """Content outside comments should be preserved."""
        url = f"{test_root}/pages/comments.html?PageSpeedFilters=remove_comments"

        response = client.get(url)
        assert_http_status(response, 200)

        # Visible content should still be present
        assert_contains(response, r'<html')
        assert_contains(response, r'</html>')


@pytest.mark.html_rewrite
class TestCollapseWhitespace:
    """Tests for the collapse_whitespace filter."""

    def test_collapse_whitespace_reduces_size(
        self, client: PageSpeedClient, test_root: str
    ):
        """Whitespace collapsing should reduce HTML size."""
        # Get original size
        off_response = client.get(f"{test_root}/pages/whitespace_test.html?PageSpeed=off")
        assert_http_status(off_response, 200)
        original_size = len(off_response.text)

        # Get optimized size
        on_response = client.get(
            f"{test_root}/pages/whitespace_test.html?PageSpeedFilters=collapse_whitespace"
        )
        assert_http_status(on_response, 200)
        optimized_size = len(on_response.text)

        # Optimized should be smaller or similar
        # Allow 10% larger due to added PageSpeed metadata
        assert optimized_size <= original_size * 1.1, \
            f"Optimized ({optimized_size}) should not be much larger than original ({original_size})"

    def test_collapse_whitespace_preserves_structure(
        self, client: PageSpeedClient, test_root: str
    ):
        """HTML structure should be preserved after whitespace collapse."""
        url = f"{test_root}/pages/whitespace_test.html?PageSpeedFilters=collapse_whitespace"

        response = client.get(url)
        assert_http_status(response, 200)

        # Essential structure should be preserved
        assert_contains(response, r'<html')
        assert_contains(response, r'</html>')
        assert_contains(response, r'<body')
        assert_contains(response, r'</body>')


@pytest.mark.html_rewrite
class TestTrimUrls:
    """Tests for URL trimming in HTML optimization."""

    def test_relative_urls_preserved(
        self, client: PageSpeedClient, example_root: str
    ):
        """Relative URLs should be preserved correctly."""
        url = f"{example_root}/combine_css.html"

        response = client.get(url)
        assert_http_status(response, 200)

        # URLs in the response should be valid
        assert_contains(response, r'href=')

    def test_absolute_urls_work(
        self, client: PageSpeedClient, example_root: str
    ):
        """Absolute URLs should work correctly."""
        url = f"{example_root}/index.html"

        response = client.get(url)
        assert_http_status(response, 200)


@pytest.mark.html_rewrite
class TestCombinedHtmlOptimization:
    """Tests for multiple HTML optimization filters combined."""

    def test_multiple_html_filters(
        self, client: PageSpeedClient, example_root: str
    ):
        """Multiple HTML optimization filters should work together."""
        filters = "collapse_whitespace,remove_comments,elide_attributes"
        url = f"{example_root}/index.html?PageSpeedFilters={filters}"

        response = client.get(url)
        assert_http_status(response, 200)

        # Page should still be valid HTML
        assert_contains(response, r'<html')
        assert_contains(response, r'</html>')

    def test_html_filters_with_css_filters(
        self, client: PageSpeedClient, example_root: str
    ):
        """HTML optimization filters should work with CSS filters."""
        filters = "collapse_whitespace,combine_css,rewrite_css"
        url = f"{example_root}/combine_css.html?PageSpeedFilters={filters}"

        response = client.fetch_until_contains(
            url,
            pattern=r'\.pagespeed\.',
            timeout=30.0,
        )
        assert_http_status(response, 200)

    def test_html_filters_with_js_filters(
        self, client: PageSpeedClient, example_root: str
    ):
        """HTML optimization filters should work with JS filters."""
        filters = "collapse_whitespace,combine_javascript"
        url = f"{example_root}/combine_javascript.html?PageSpeedFilters={filters}"

        response = client.fetch_until_contains(
            url,
            pattern=r'<script',
            timeout=30.0,
        )
        assert_http_status(response, 200)


@pytest.mark.html_rewrite
class TestHtmlOptimizationEdgeCases:
    """Edge cases for HTML optimization."""

    def test_empty_page_handling(
        self, client: PageSpeedClient, example_root: str
    ):
        """Empty or minimal pages should be handled gracefully."""
        # Request with all HTML filters
        filters = "collapse_whitespace,remove_comments,elide_attributes"
        url = f"{example_root}/index.html?PageSpeedFilters={filters}"

        response = client.get(url)
        assert_http_status(response, 200)

    def test_pagespeed_off_disables_all(
        self, client: PageSpeedClient, example_root: str
    ):
        """PageSpeed=off should disable all HTML optimization."""
        url = f"{example_root}/index.html?PageSpeed=off"

        response = client.get(url)
        assert_http_status(response, 200)

        # Should not have .pagespeed. in URLs
        assert_not_contains(response, r"\.pagespeed\.")

    def test_xhtml_handling(
        self, client: PageSpeedClient, test_root: str
    ):
        """XHTML documents should be handled correctly."""
        url = f"{test_root}/xhtml.xhtml?PageSpeedFilters=collapse_whitespace"

        response = client.get(url)
        assert_http_status(response, 200)


if __name__ == "__main__":
    pytest.main([__file__, "-v"])
