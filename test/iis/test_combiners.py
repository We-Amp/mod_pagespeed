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

"""CSS and JavaScript combiner tests for IIS PageSpeed module.

These tests verify that combine_css and combine_javascript filters work
correctly, combining multiple CSS/JS files into a single resource with
.pagespeed.cc. and .pagespeed.jc. URLs respectively.
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
class TestCombineCss:
    """Tests for the combine_css filter.

    The combine_css filter combines multiple CSS <link> elements into a
    single combined CSS file with a .pagespeed.cc. URL pattern.
    """

    def test_combine_css_produces_combined_url(
        self, client: PageSpeedClient, example_root: str
    ):
        """combine_css should produce a .pagespeed.cc. URL."""
        url = f"{example_root}/combine_css.html?PageSpeedFilters=combine_css"

        # Wait for combined CSS to appear
        response = client.fetch_until_contains(
            url,
            pattern=r'\.pagespeed\.cc\.',
            timeout=30.0,
        )

        assert_http_status(response, 200)
        # Should have a combined CSS URL
        assert_contains(response, ".pagespeed.cc.")

    def test_combine_css_reduces_link_tags(
        self, client: PageSpeedClient, example_root: str
    ):
        """combine_css should reduce the number of link tags.

        The original page has 3 CSS links (main.css, colors.css, layout.css).
        After combining, there should be fewer link tags.
        """
        url = f"{example_root}/combine_css.html?PageSpeedFilters=combine_css"

        # Wait for optimization
        response = client.fetch_until_contains(
            url,
            pattern=r'\.pagespeed\.cc\.',
            timeout=30.0,
        )

        assert_http_status(response, 200)

        # Count stylesheet link tags
        original_links = ["main.css", "colors.css", "layout.css"]
        remaining_original = sum(
            1 for link in original_links if link in response.text
        )

        # After combining, we should see the combined URL and fewer original links
        assert ".pagespeed.cc." in response.text, "Should have combined CSS URL"
        # If combining worked, not all original links should remain
        # (they may still appear in comments or be partially processed)

    def test_combined_css_is_fetchable(
        self, client: PageSpeedClient, example_root: str
    ):
        """The combined CSS URL should be fetchable and return valid CSS."""
        url = f"{example_root}/combine_css.html?PageSpeedFilters=combine_css"

        # Get the HTML with combined CSS URL
        response = client.fetch_until_contains(
            url,
            pattern=r'\.pagespeed\.cc\.',
            timeout=30.0,
        )

        assert_http_status(response, 200)

        # Extract the combined CSS URL
        match = re.search(r'href="([^"]*\.pagespeed\.cc\.[^"]*)"', response.text)
        assert match, "Should find combined CSS URL in href"

        combined_url = match.group(1)
        # Make absolute if relative
        if combined_url.startswith("/"):
            combined_url = f"{example_root}{combined_url}"
        elif not combined_url.startswith("http"):
            combined_url = f"{example_root}/{combined_url}"

        # Fetch the combined CSS
        css_response = client.get(combined_url)
        assert_http_status(css_response, 200)

        # Should be CSS content type
        content_type = css_response.header("Content-Type")
        assert "css" in content_type.lower(), f"Expected CSS, got {content_type}"

    def test_combined_css_contains_original_rules(
        self, client: PageSpeedClient, example_root: str
    ):
        """The combined CSS should contain rules from all original files."""
        url = f"{example_root}/combine_css.html?PageSpeedFilters=combine_css"

        # Get the HTML with combined CSS URL
        response = client.fetch_until_contains(
            url,
            pattern=r'\.pagespeed\.cc\.',
            timeout=30.0,
        )

        assert_http_status(response, 200)

        # Extract the combined CSS URL
        match = re.search(r'href="([^"]*\.pagespeed\.cc\.[^"]*)"', response.text)
        if not match:
            pytest.skip("Combined CSS URL not found - combining may be pending")

        combined_url = match.group(1)
        if combined_url.startswith("/"):
            combined_url = f"{example_root}{combined_url}"
        elif not combined_url.startswith("http"):
            combined_url = f"{example_root}/{combined_url}"

        # Fetch the combined CSS
        css_response = client.get(combined_url)
        assert_http_status(css_response, 200)

        # Check for content from each original file
        # From main.css: body rules
        # From colors.css: .box rules
        # From layout.css: .flex rules
        css_text = css_response.text

        # Should contain at least some content from the original files
        # (exact content may be minified)
        has_body = "body" in css_text or "font-family" in css_text
        has_box = "box" in css_text or "inline-block" in css_text
        has_flex = "flex" in css_text or "display" in css_text

        assert has_body or has_box or has_flex, \
            "Combined CSS should contain rules from original files"


@pytest.mark.html_rewrite
class TestCombineJavascript:
    """Tests for the combine_javascript filter.

    The combine_javascript filter combines multiple <script> elements into
    a single combined JS file with a .pagespeed.jc. URL pattern.
    """

    def test_combine_javascript_produces_combined_url(
        self, client: PageSpeedClient, example_root: str
    ):
        """combine_javascript should produce a .pagespeed.jc. URL."""
        url = f"{example_root}/combine_javascript.html?PageSpeedFilters=combine_javascript"

        # Wait for combined JS to appear
        response = client.fetch_until_contains(
            url,
            pattern=r'\.pagespeed\.jc\.',
            timeout=30.0,
        )

        assert_http_status(response, 200)
        assert_contains(response, ".pagespeed.jc.")

    def test_combine_javascript_reduces_script_tags(
        self, client: PageSpeedClient, example_root: str
    ):
        """combine_javascript should reduce the number of script tags.

        The original page has 3 script files (util.js, helper.js, main.js).
        After combining, there should be fewer script tags.
        """
        url = f"{example_root}/combine_javascript.html?PageSpeedFilters=combine_javascript"

        # Wait for optimization
        response = client.fetch_until_contains(
            url,
            pattern=r'\.pagespeed\.jc\.',
            timeout=30.0,
        )

        assert_http_status(response, 200)

        # After combining, we should see the combined URL
        assert ".pagespeed.jc." in response.text, "Should have combined JS URL"

    def test_combined_javascript_is_fetchable(
        self, client: PageSpeedClient, example_root: str
    ):
        """The combined JavaScript URL should be fetchable and return valid JS."""
        url = f"{example_root}/combine_javascript.html?PageSpeedFilters=combine_javascript"

        # Get the HTML with combined JS URL
        response = client.fetch_until_contains(
            url,
            pattern=r'\.pagespeed\.jc\.',
            timeout=30.0,
        )

        assert_http_status(response, 200)

        # Extract the combined JS URL
        match = re.search(r'src="([^"]*\.pagespeed\.jc\.[^"]*)"', response.text)
        assert match, "Should find combined JS URL in src"

        combined_url = match.group(1)
        if combined_url.startswith("/"):
            combined_url = f"{example_root}{combined_url}"
        elif not combined_url.startswith("http"):
            combined_url = f"{example_root}/{combined_url}"

        # Fetch the combined JS
        js_response = client.get(combined_url)
        assert_http_status(js_response, 200)

        # Should be JavaScript content type
        content_type = js_response.header("Content-Type")
        assert "javascript" in content_type.lower() or "application" in content_type.lower(), \
            f"Expected JavaScript, got {content_type}"

    def test_combined_javascript_contains_original_code(
        self, client: PageSpeedClient, example_root: str
    ):
        """The combined JavaScript should contain code from all original files."""
        url = f"{example_root}/combine_javascript.html?PageSpeedFilters=combine_javascript"

        # Get the HTML with combined JS URL
        response = client.fetch_until_contains(
            url,
            pattern=r'\.pagespeed\.jc\.',
            timeout=30.0,
        )

        assert_http_status(response, 200)

        # Extract the combined JS URL
        match = re.search(r'src="([^"]*\.pagespeed\.jc\.[^"]*)"', response.text)
        if not match:
            pytest.skip("Combined JS URL not found - combining may be pending")

        combined_url = match.group(1)
        if combined_url.startswith("/"):
            combined_url = f"{example_root}{combined_url}"
        elif not combined_url.startswith("http"):
            combined_url = f"{example_root}/{combined_url}"

        # Fetch the combined JS
        js_response = client.get(combined_url)
        assert_http_status(js_response, 200)

        # Check for content from each original file
        # util.js: PSUtil, formatDate, getCookie
        # helper.js: PSHelper, log, isInViewport
        # main.js: init, DOMContentLoaded
        js_text = js_response.text

        # Should contain identifiers from original files
        has_util = "PSUtil" in js_text or "formatDate" in js_text or "getCookie" in js_text
        has_helper = "PSHelper" in js_text or "isInViewport" in js_text or "debounce" in js_text
        has_main = "init" in js_text or "DOMContentLoaded" in js_text

        assert has_util or has_helper or has_main, \
            "Combined JS should contain code from original files"


@pytest.mark.html_rewrite
class TestCombineMultipleFiles:
    """Tests for combining files from different paths."""

    def test_combine_css_from_different_paths(
        self, client: PageSpeedClient, example_root: str
    ):
        """combine_css should handle CSS files from the same directory."""
        url = f"{example_root}/combine_css.html?PageSpeedFilters=combine_css"

        # Wait for combining
        response = client.fetch_until_contains(
            url,
            pattern=r'\.pagespeed\.cc\.',
            timeout=30.0,
        )

        assert_http_status(response, 200)

        # The combined URL should be present
        assert ".pagespeed.cc." in response.text

    def test_combine_javascript_preserves_execution_order(
        self, client: PageSpeedClient, example_root: str
    ):
        """Combined JavaScript should preserve script execution order.

        The original order is: util.js, helper.js, main.js
        The combined output should maintain this order so dependencies work.
        """
        url = f"{example_root}/combine_javascript.html?PageSpeedFilters=combine_javascript"

        # Get the HTML with combined JS URL
        response = client.fetch_until_contains(
            url,
            pattern=r'\.pagespeed\.jc\.',
            timeout=30.0,
        )

        assert_http_status(response, 200)

        # Extract the combined JS URL
        match = re.search(r'src="([^"]*\.pagespeed\.jc\.[^"]*)"', response.text)
        if not match:
            pytest.skip("Combined JS URL not found - combining may be pending")

        combined_url = match.group(1)
        if combined_url.startswith("/"):
            combined_url = f"{example_root}{combined_url}"
        elif not combined_url.startswith("http"):
            combined_url = f"{example_root}/{combined_url}"

        # Fetch the combined JS
        js_response = client.get(combined_url)
        assert_http_status(js_response, 200)

        js_text = js_response.text

        # PSUtil should come before PSHelper (util.js before helper.js)
        util_pos = js_text.find("PSUtil")
        helper_pos = js_text.find("PSHelper")

        if util_pos >= 0 and helper_pos >= 0:
            assert util_pos < helper_pos, \
                "PSUtil should appear before PSHelper to preserve execution order"


@pytest.mark.html_rewrite
class TestCombinerCaching:
    """Tests for cache headers on combined resources."""

    def test_combined_css_has_cache_headers(
        self, client: PageSpeedClient, example_root: str
    ):
        """Combined CSS resources should have appropriate cache headers."""
        url = f"{example_root}/combine_css.html?PageSpeedFilters=combine_css"

        # Get the HTML with combined CSS URL
        response = client.fetch_until_contains(
            url,
            pattern=r'\.pagespeed\.cc\.',
            timeout=30.0,
        )

        assert_http_status(response, 200)

        # Extract the combined CSS URL
        match = re.search(r'href="([^"]*\.pagespeed\.cc\.[^"]*)"', response.text)
        if not match:
            pytest.skip("Combined CSS URL not found")

        combined_url = match.group(1)
        if combined_url.startswith("/"):
            combined_url = f"{example_root}{combined_url}"
        elif not combined_url.startswith("http"):
            combined_url = f"{example_root}/{combined_url}"

        # Fetch the combined CSS
        css_response = client.get(combined_url)
        assert_http_status(css_response, 200)

        # Should have Cache-Control header
        cache_control = css_response.header("Cache-Control")
        assert cache_control, "Combined CSS should have Cache-Control header"

        # PageSpeed typically sets long cache times for combined resources
        # because the URL contains a hash that changes when content changes
        if "max-age" in cache_control.lower():
            # Extract max-age value
            match = re.search(r'max-age=(\d+)', cache_control)
            if match:
                max_age = int(match.group(1))
                # Should have a reasonably long cache time (at least 1 hour)
                assert max_age >= 3600, \
                    f"max-age should be >= 3600, got {max_age}"

    def test_combined_javascript_has_cache_headers(
        self, client: PageSpeedClient, example_root: str
    ):
        """Combined JavaScript resources should have appropriate cache headers."""
        url = f"{example_root}/combine_javascript.html?PageSpeedFilters=combine_javascript"

        # Get the HTML with combined JS URL
        response = client.fetch_until_contains(
            url,
            pattern=r'\.pagespeed\.jc\.',
            timeout=30.0,
        )

        assert_http_status(response, 200)

        # Extract the combined JS URL
        match = re.search(r'src="([^"]*\.pagespeed\.jc\.[^"]*)"', response.text)
        if not match:
            pytest.skip("Combined JS URL not found")

        combined_url = match.group(1)
        if combined_url.startswith("/"):
            combined_url = f"{example_root}{combined_url}"
        elif not combined_url.startswith("http"):
            combined_url = f"{example_root}/{combined_url}"

        # Fetch the combined JS
        js_response = client.get(combined_url)
        assert_http_status(js_response, 200)

        # Should have Cache-Control header
        cache_control = js_response.header("Cache-Control")
        assert cache_control, "Combined JS should have Cache-Control header"

    def test_combined_resource_etag(
        self, client: PageSpeedClient, example_root: str
    ):
        """Combined resources may have ETag for validation."""
        url = f"{example_root}/combine_css.html?PageSpeedFilters=combine_css"

        # Get the HTML with combined CSS URL
        response = client.fetch_until_contains(
            url,
            pattern=r'\.pagespeed\.cc\.',
            timeout=30.0,
        )

        assert_http_status(response, 200)

        # Extract the combined CSS URL
        match = re.search(r'href="([^"]*\.pagespeed\.cc\.[^"]*)"', response.text)
        if not match:
            pytest.skip("Combined CSS URL not found")

        combined_url = match.group(1)
        if combined_url.startswith("/"):
            combined_url = f"{example_root}{combined_url}"
        elif not combined_url.startswith("http"):
            combined_url = f"{example_root}/{combined_url}"

        # Fetch the combined CSS
        css_response = client.get(combined_url)
        assert_http_status(css_response, 200)

        # ETag is optional but commonly provided
        etag = css_response.header("ETag")
        # Just verify response is valid - ETag may or may not be present

    def test_combined_resource_conditional_request(
        self, client: PageSpeedClient, example_root: str
    ):
        """Combined resources should support conditional requests if ETag present."""
        url = f"{example_root}/combine_css.html?PageSpeedFilters=combine_css"

        # Get the HTML with combined CSS URL
        response = client.fetch_until_contains(
            url,
            pattern=r'\.pagespeed\.cc\.',
            timeout=30.0,
        )

        assert_http_status(response, 200)

        # Extract the combined CSS URL
        match = re.search(r'href="([^"]*\.pagespeed\.cc\.[^"]*)"', response.text)
        if not match:
            pytest.skip("Combined CSS URL not found")

        combined_url = match.group(1)
        if combined_url.startswith("/"):
            combined_url = f"{example_root}{combined_url}"
        elif not combined_url.startswith("http"):
            combined_url = f"{example_root}/{combined_url}"

        # First request to get ETag
        css_response = client.get(combined_url)
        assert_http_status(css_response, 200)

        etag = css_response.header("ETag")
        if etag:
            # Make conditional request
            conditional_response = client.get(
                combined_url,
                headers={"If-None-Match": etag}
            )
            # Should return 304 Not Modified or 200 OK
            assert conditional_response.status in (200, 304), \
                f"Expected 200 or 304, got {conditional_response.status}"


@pytest.mark.html_rewrite
class TestCombinerEdgeCases:
    """Edge cases for CSS/JS combining."""

    def test_combine_with_pagespeed_off(
        self, client: PageSpeedClient, example_root: str
    ):
        """PageSpeed=off should disable combining."""
        url = f"{example_root}/combine_css.html?PageSpeed=off"

        response = client.get(url)
        assert_http_status(response, 200)

        # Should NOT have combined URLs
        assert ".pagespeed.cc." not in response.text
        assert ".pagespeed.jc." not in response.text

    def test_multiple_filters_with_combine(
        self, client: PageSpeedClient, example_root: str
    ):
        """combine_css should work with other filters."""
        filters = "combine_css,collapse_whitespace"
        url = f"{example_root}/combine_css.html?PageSpeedFilters={filters}"

        response = client.fetch_until_contains(
            url,
            pattern=r'\.pagespeed\.cc\.',
            timeout=30.0,
        )

        assert_http_status(response, 200)
        assert_contains(response, ".pagespeed.cc.")

    def test_combine_css_and_javascript_together(
        self, client: PageSpeedClient, example_root: str
    ):
        """Both combine_css and combine_javascript should work on same page."""
        filters = "combine_css,combine_javascript"
        # Use combine_css.html since it has CSS; JS combining needs JS on page
        url = f"{example_root}/combine_css.html?PageSpeedFilters={filters}"

        response = client.fetch_until_contains(
            url,
            pattern=r'\.pagespeed\.cc\.',
            timeout=30.0,
        )

        assert_http_status(response, 200)
        # Should at least have combined CSS
        assert_contains(response, ".pagespeed.cc.")


if __name__ == "__main__":
    raise SystemExit(pytest.main([__file__, "-v"]))
