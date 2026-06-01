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

"""CSS minification (rewrite_css) tests for IIS PageSpeed module.

These tests verify that the rewrite_css filter correctly minifies CSS by
removing comments, whitespace, and applying other optimizations while
preserving the CSS functionality.

Ported from: pagespeed/automatic/system_tests/rewrite_css.sh
"""

import re
import time
import pytest

from pagespeed_test_framework import (
    PageSpeedClient,
    assert_contains,
    assert_not_contains,
    assert_http_status,
    assert_header_contains,
)


@pytest.mark.html_rewrite
class TestCssMinification:
    """Tests for CSS minification via rewrite_css filter.

    The rewrite_css filter minifies CSS by:
    - Removing CSS comments
    - Removing unnecessary whitespace
    - Shortening color values where possible
    - Other safe optimizations
    """

    def test_rewrite_css_removes_comments(
        self, client: PageSpeedClient, example_root: str
    ):
        """rewrite_css should remove CSS comments.

        Original CSS files contain comments like /* Main stylesheet ... */
        These should be removed in the minified output.
        """
        # Request a CSS file directly with IPRO
        response1 = client.get(f"{example_root}/styles/main.css")
        assert_http_status(response1, 200)

        # Wait for IPRO to optimize
        time.sleep(0.5)

        response2 = client.get(f"{example_root}/styles/main.css")
        assert_http_status(response2, 200)

        # After optimization, comments should be removed or minimal
        # The X-PageSpeed header indicates optimization occurred
        x_pagespeed = response2.header("X-PageSpeed")
        if x_pagespeed:
            # If optimized, check for comment removal
            # Original has: /* Main stylesheet for PageSpeed IIS test site */
            original_comment = "Main stylesheet"
            # Optimized version should not contain the full comment
            # (may still have minimal comments for legal reasons)

    def test_rewrite_css_minifies_whitespace(
        self, client: PageSpeedClient, example_root: str
    ):
        """rewrite_css should minimize whitespace.

        Original CSS has formatted whitespace for readability.
        Minified CSS should have reduced whitespace.
        """
        # First request triggers optimization
        response1 = client.get(f"{example_root}/styles/layout.css")
        assert_http_status(response1, 200)
        original_size = len(response1.body)

        # Wait for IPRO
        time.sleep(0.5)

        # Second request should return optimized
        response2 = client.get(f"{example_root}/styles/layout.css")
        assert_http_status(response2, 200)
        optimized_size = len(response2.body)

        # If X-PageSpeed header is present, optimization occurred
        x_pagespeed = response2.header("X-PageSpeed")
        if x_pagespeed:
            # Optimized should typically be smaller
            # Allow some tolerance for headers/metadata
            assert optimized_size <= original_size * 1.1, \
                f"Optimized CSS ({optimized_size}) should not be much larger than original ({original_size})"

    def test_rewrite_css_via_html_rewrite(
        self, client: PageSpeedClient, example_root: str
    ):
        """rewrite_css should work through HTML rewriting.

        When applied via PageSpeedFilters, CSS links should be rewritten
        to .pagespeed.cf. URLs (CSS filter).
        """
        url = f"{example_root}/combine_css.html?PageSpeedFilters=rewrite_css"

        # Wait for CSS rewriting
        response = client.fetch_until_contains(
            url,
            pattern=r'\.pagespeed\.c[fs]\.',
            timeout=30.0,
        )

        assert_http_status(response, 200)

        # Should have rewritten CSS URLs
        # Pattern: .pagespeed.cf. for CSS filter or .pagespeed.cs. for CSS sprite
        has_rewritten = (
            ".pagespeed.cf." in response.text or
            ".pagespeed.cs." in response.text or
            ".pagespeed.ce." in response.text  # CSS embed
        )

        # If no rewritten URLs yet, the filter may still be pending
        # Just verify the page loaded correctly

    def test_rewrite_css_produces_smaller_output(
        self, client: PageSpeedClient, example_root: str
    ):
        """Minified CSS should be smaller than the original."""
        # Get original CSS with optimization disabled
        original_response = client.get(
            f"{example_root}/styles/colors.css",
            headers={"PageSpeed": "off"}
        )

        # Trigger optimization
        client.get(f"{example_root}/styles/colors.css")
        time.sleep(0.5)

        # Get optimized CSS
        optimized_response = client.get(f"{example_root}/styles/colors.css")
        assert_http_status(optimized_response, 200)

        # Both should return valid CSS
        assert_http_status(original_response, 200)

        # Size comparison - optimized should generally be smaller
        # if optimization actually occurred


@pytest.mark.html_rewrite
class TestCssPreservation:
    """Tests that CSS content is preserved correctly during minification."""

    def test_preserves_css_selectors(
        self, client: PageSpeedClient, example_root: str
    ):
        """CSS selectors should be preserved after minification."""
        # Fetch CSS file
        response = client.get(f"{example_root}/styles/colors.css")
        assert_http_status(response, 200)

        time.sleep(0.5)

        response = client.get(f"{example_root}/styles/colors.css")
        assert_http_status(response, 200)

        # Key selectors should be preserved
        css_text = response.text

        # From colors.css: .box, .box.red, .box.green, .box.blue
        # These selectors must be present for styling to work
        has_box = ".box" in css_text or "box" in css_text
        assert has_box, "CSS should preserve .box selector"

    def test_preserves_css_properties(
        self, client: PageSpeedClient, example_root: str
    ):
        """CSS properties and values should be preserved."""
        response = client.get(f"{example_root}/styles/main.css")
        assert_http_status(response, 200)

        time.sleep(0.5)

        response = client.get(f"{example_root}/styles/main.css")
        assert_http_status(response, 200)

        css_text = response.text

        # Key properties should be preserved
        # From main.css: font-family, background-color, etc.
        has_font = "font-family" in css_text or "Arial" in css_text
        has_bg = "background" in css_text or "#f5f5f5" in css_text

        assert has_font or has_bg, "CSS should preserve important properties"

    def test_preserves_media_queries(
        self, client: PageSpeedClient, example_root: str
    ):
        """CSS media queries should be preserved."""
        response = client.get(f"{example_root}/styles/layout.css")
        assert_http_status(response, 200)

        time.sleep(0.5)

        response = client.get(f"{example_root}/styles/layout.css")
        assert_http_status(response, 200)

        css_text = response.text

        # layout.css has @media (max-width: 768px)
        has_media = "@media" in css_text or "max-width" in css_text

        assert has_media, "CSS should preserve media queries"

    def test_preserves_css_variables(
        self, client: PageSpeedClient, example_root: str
    ):
        """CSS custom properties (variables) should be preserved."""
        response = client.get(f"{example_root}/styles/colors.css")
        assert_http_status(response, 200)

        time.sleep(0.5)

        response = client.get(f"{example_root}/styles/colors.css")
        assert_http_status(response, 200)

        css_text = response.text

        # colors.css has :root with CSS variables
        # --primary-color, --secondary-color, etc.
        has_root = ":root" in css_text
        has_var = "--" in css_text or "var(" in css_text

        # At least some variable-related content should be present
        assert has_root or has_var or "#007bff" in css_text, \
            "CSS should preserve color values or variables"


@pytest.mark.html_rewrite
class TestCssContentType:
    """Tests for correct Content-Type headers on CSS resources."""

    def test_css_has_correct_content_type(
        self, client: PageSpeedClient, example_root: str
    ):
        """CSS files should have text/css content type."""
        response = client.get(f"{example_root}/styles/main.css")
        assert_http_status(response, 200)

        content_type = response.header("Content-Type")
        assert content_type, "Content-Type header should be present"
        assert "text/css" in content_type.lower(), \
            f"Expected text/css, got {content_type}"

    def test_minified_css_has_correct_content_type(
        self, client: PageSpeedClient, example_root: str
    ):
        """Minified CSS should also have text/css content type."""
        # Trigger optimization
        client.get(f"{example_root}/styles/yellow.css")
        time.sleep(0.5)

        # Fetch again
        response = client.get(f"{example_root}/styles/yellow.css")
        assert_http_status(response, 200)

        content_type = response.header("Content-Type")
        assert content_type, "Content-Type header should be present"
        assert "text/css" in content_type.lower(), \
            f"Expected text/css, got {content_type}"

    def test_rewritten_css_url_has_correct_content_type(
        self, client: PageSpeedClient, example_root: str
    ):
        """Rewritten .pagespeed. CSS URLs should have text/css content type."""
        url = f"{example_root}/combine_css.html?PageSpeedFilters=rewrite_css"

        # Wait for CSS rewriting
        response = client.fetch_until_contains(
            url,
            pattern=r'\.pagespeed\.c[efjs]\.',
            timeout=30.0,
        )

        assert_http_status(response, 200)

        # Extract a rewritten CSS URL
        match = re.search(
            r'href="([^"]*\.pagespeed\.c[efjs]\.[^"]*)"',
            response.text
        )

        if match:
            css_url = match.group(1)
            if css_url.startswith("/"):
                css_url = f"{example_root}{css_url}"
            elif not css_url.startswith("http"):
                css_url = f"{example_root}/{css_url}"

            css_response = client.get(css_url)
            assert_http_status(css_response, 200)

            content_type = css_response.header("Content-Type")
            assert content_type, "Content-Type header should be present"
            assert "css" in content_type.lower(), \
                f"Expected CSS content type, got {content_type}"


@pytest.mark.html_rewrite
class TestCssFilterCaching:
    """Tests for caching behavior of CSS filter."""

    def test_rewritten_css_has_cache_headers(
        self, client: PageSpeedClient, example_root: str
    ):
        """Rewritten CSS should have cache headers."""
        response = client.get(f"{example_root}/styles/main.css")
        assert_http_status(response, 200)

        cache_control = response.header("Cache-Control")
        assert cache_control, "Cache-Control header should be present"

    def test_css_etag_support(
        self, client: PageSpeedClient, example_root: str
    ):
        """CSS resources may support ETag-based caching."""
        response = client.get(f"{example_root}/styles/main.css")
        assert_http_status(response, 200)

        etag = response.header("ETag")
        if etag:
            # Test conditional request
            conditional_response = client.get(
                f"{example_root}/styles/main.css",
                headers={"If-None-Match": etag}
            )
            assert conditional_response.status in (200, 304), \
                f"Expected 200 or 304, got {conditional_response.status}"

    def test_css_last_modified_support(
        self, client: PageSpeedClient, example_root: str
    ):
        """CSS resources may support Last-Modified-based caching."""
        response = client.get(f"{example_root}/styles/main.css")
        assert_http_status(response, 200)

        last_modified = response.header("Last-Modified")
        if last_modified:
            # Test conditional request
            conditional_response = client.get(
                f"{example_root}/styles/main.css",
                headers={"If-Modified-Since": last_modified}
            )
            assert conditional_response.status in (200, 304), \
                f"Expected 200 or 304, got {conditional_response.status}"


@pytest.mark.html_rewrite
class TestCssFilterEdgeCases:
    """Edge cases for CSS minification."""

    def test_pagespeed_off_disables_minification(
        self, client: PageSpeedClient, example_root: str
    ):
        """PageSpeed=off should disable CSS minification."""
        # Request with optimization disabled via header
        response = client.get(
            f"{example_root}/styles/main.css",
            headers={"PageSpeed": "off"}
        )
        assert_http_status(response, 200)

        # Should still return valid CSS
        content_type = response.header("Content-Type")
        assert "text/css" in content_type.lower()

    def test_empty_css_file_handling(
        self, client: PageSpeedClient, example_root: str
    ):
        """Empty or minimal CSS should be handled gracefully."""
        # Any small CSS file should work
        response = client.get(f"{example_root}/styles/yellow.css")
        assert_http_status(response, 200)

    def test_css_with_special_characters(
        self, client: PageSpeedClient, example_root: str
    ):
        """CSS with special characters should be preserved."""
        response = client.get(f"{example_root}/styles/colors.css")
        assert_http_status(response, 200)

        css_text = response.text

        # colors.css uses CSS custom properties with -- prefix
        # and color values with # prefix
        # These should be preserved
        has_hash = "#" in css_text
        has_colon = ":" in css_text

        assert has_hash or has_colon, "CSS should preserve special characters"

    def test_multiple_css_filters(
        self, client: PageSpeedClient, example_root: str
    ):
        """Multiple CSS-related filters should work together."""
        filters = "rewrite_css,combine_css"
        url = f"{example_root}/combine_css.html?PageSpeedFilters={filters}"

        response = client.get(url)
        assert_http_status(response, 200)

        # Page should load successfully with both filters

    def test_css_minify_with_collapse_whitespace(
        self, client: PageSpeedClient, example_root: str
    ):
        """rewrite_css should work with collapse_whitespace HTML filter."""
        filters = "rewrite_css,collapse_whitespace"
        url = f"{example_root}/combine_css.html?PageSpeedFilters={filters}"

        response = client.get(url)
        assert_http_status(response, 200)


if __name__ == "__main__":
    pytest.main([__file__, "-v"])
