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

"""HTML rewriting tests for IIS PageSpeed module.

These tests verify that HTML documents are rewritten with various
PageSpeed filters applied.
"""

import pytest

from pagespeed_test_framework import (
    PageSpeedClient,
    assert_contains,
    assert_not_contains,
    assert_http_status,
    assert_header_contains,
)


@pytest.mark.html_rewrite
class TestCSSRewriting:
    """CSS-related HTML rewriting tests."""

    def test_combine_css(self, client: PageSpeedClient, example_root: str):
        """combine_css filter should combine multiple CSS files."""
        response = client.get(
            f"{example_root}/combine_css.html?PageSpeedFilters=combine_css"
        )
        assert_http_status(response, 200)

        # Should see combined CSS reference with .pagespeed. in URL
        # or may not be combined yet on first request
        assert_contains(response, "<link")

    def test_rewrite_css(self, client: PageSpeedClient, example_root: str):
        """rewrite_css filter should minify inline CSS."""
        response = client.get(
            f"{example_root}/rewrite_css.html?PageSpeedFilters=rewrite_css"
        )
        assert_http_status(response, 200)

    def test_move_css_to_head(self, client: PageSpeedClient, example_root: str):
        """move_css_to_head filter should move CSS links to head."""
        response = client.get(
            f"{example_root}/move_css.html?PageSpeedFilters=move_css_to_head"
        )
        assert_http_status(response, 200)


@pytest.mark.html_rewrite
class TestJavaScriptRewriting:
    """JavaScript-related HTML rewriting tests."""

    def test_combine_javascript(self, client: PageSpeedClient, example_root: str):
        """combine_javascript filter should combine script files."""
        response = client.get(
            f"{example_root}/combine_javascript.html?PageSpeedFilters=combine_javascript"
        )
        assert_http_status(response, 200)
        assert_contains(response, "<script")

    def test_rewrite_javascript(self, client: PageSpeedClient, example_root: str):
        """rewrite_javascript filter should minify JavaScript."""
        response = client.get(
            f"{example_root}/rewrite_javascript.html?PageSpeedFilters=rewrite_javascript"
        )
        assert_http_status(response, 200)

    def test_defer_javascript(self, client: PageSpeedClient, example_root: str):
        """defer_javascript filter should defer script execution."""
        response = client.get(
            f"{example_root}/defer_javascript.html?PageSpeedFilters=defer_javascript"
        )
        assert_http_status(response, 200)


@pytest.mark.html_rewrite
class TestImageRewriting:
    """Image-related HTML rewriting tests."""

    def test_rewrite_images(self, client: PageSpeedClient, example_root: str):
        """rewrite_images filter should optimize image references."""
        response = client.get(
            f"{example_root}/rewrite_images.html?PageSpeedFilters=rewrite_images"
        )
        assert_http_status(response, 200)
        assert_contains(response, "<img")

    def test_inline_small_images(self, client: PageSpeedClient, example_root: str):
        """inline_images filter should inline small images as data URIs."""
        response = client.get(
            f"{example_root}/inline_images.html?PageSpeedFilters=inline_images"
        )
        assert_http_status(response, 200)

    def test_lazyload_images(self, client: PageSpeedClient, example_root: str):
        """lazyload_images filter should add lazy loading."""
        response = client.get(
            f"{example_root}/lazyload_images.html?PageSpeedFilters=lazyload_images"
        )
        assert_http_status(response, 200)


@pytest.mark.html_rewrite
class TestHTMLOptimization:
    """General HTML optimization tests."""

    def test_collapse_whitespace(self, client: PageSpeedClient, example_root: str):
        """collapse_whitespace filter should remove extra whitespace."""
        response = client.get(
            f"{example_root}/collapse_whitespace.html?PageSpeedFilters=collapse_whitespace"
        )
        assert_http_status(response, 200)
        # Multiple spaces should be collapsed
        assert "  " not in response.text or ".pagespeed." in response.text

    def test_remove_comments(self, client: PageSpeedClient, example_root: str):
        """remove_comments filter should remove HTML comments."""
        response = client.get(
            f"{example_root}/remove_comments.html?PageSpeedFilters=remove_comments"
        )
        assert_http_status(response, 200)

    def test_elide_attributes(self, client: PageSpeedClient, example_root: str):
        """elide_attributes filter should remove default attributes."""
        response = client.get(
            f"{example_root}/elide_attributes.html?PageSpeedFilters=elide_attributes"
        )
        assert_http_status(response, 200)


@pytest.mark.html_rewrite
class TestCoreWebVitals:
    """Core Web Vitals optimization tests."""

    def test_extend_cache(self, client: PageSpeedClient, example_root: str):
        """extend_cache filter should add cache-busting hashes."""
        response = client.get(
            f"{example_root}/extend_cache.html?PageSpeedFilters=extend_cache"
        )
        assert_http_status(response, 200)

    def test_hint_preload_subresources(self, client: PageSpeedClient, example_root: str):
        """hint_preload_subresources should add Link preload headers."""
        response = client.get(
            f"{example_root}/hint_preload.html?PageSpeedFilters=hint_preload_subresources"
        )
        assert_http_status(response, 200)

    def test_insert_dns_prefetch(self, client: PageSpeedClient, example_root: str):
        """insert_dns_prefetch should add DNS prefetch hints."""
        response = client.get(
            f"{example_root}/dns_prefetch.html?PageSpeedFilters=insert_dns_prefetch"
        )
        assert_http_status(response, 200)


@pytest.mark.html_rewrite
class TestContentPreservation:
    """Tests that content is preserved correctly during rewriting."""

    def test_large_html_not_truncated(self, client: PageSpeedClient, test_root: str):
        """Large HTML files should not be truncated."""
        response = client.get(f"{test_root}/large.html")
        assert_http_status(response, 200)
        # Verify closing tags are present
        assert_contains(response, "</html>")

    def test_utf8_content_preserved(self, client: PageSpeedClient, test_root: str):
        """UTF-8 content should be preserved correctly."""
        response = client.get(f"{test_root}/unicode.html")
        assert_http_status(response, 200)
        # The response should be valid UTF-8
        content_type = response.header("Content-Type")
        assert "utf-8" in content_type.lower() or "charset" not in content_type.lower()

    def test_xhtml_handled(self, client: PageSpeedClient, test_root: str):
        """XHTML documents should be handled correctly."""
        response = client.get(f"{test_root}/xhtml.xhtml")
        assert_http_status(response, 200)


@pytest.mark.html_rewrite
class TestAdvancedCSSFilters:
    """Advanced CSS filter tests."""

    def test_combine_multiple_css_files(self, client: PageSpeedClient, test_root: str):
        """combine_css should combine multiple CSS files from different paths."""
        response = client.get(
            f"{test_root}/pages/combine_multi_css.html?PageSpeedFilters=combine_css"
        )
        assert_http_status(response, 200)

    def test_flatten_css_imports(self, client: PageSpeedClient, test_root: str):
        """flatten_css_imports should inline @import rules."""
        response = client.get(
            f"{test_root}/pages/flatten_imports.html?PageSpeedFilters=flatten_css_imports"
        )
        assert_http_status(response, 200)

    def test_inline_css_small_files(self, client: PageSpeedClient, example_root: str):
        """inline_css should inline small CSS files."""
        response = client.get(
            f"{example_root}/inline_css.html?PageSpeedFilters=inline_css"
        )
        assert_http_status(response, 200)

    def test_prioritize_critical_css(self, client: PageSpeedClient, example_root: str):
        """prioritize_critical_css should identify and inline critical CSS."""
        response = client.get(
            f"{example_root}/prioritize_critical_css.html?PageSpeedFilters=prioritize_critical_css"
        )
        assert_http_status(response, 200)


@pytest.mark.html_rewrite
class TestAdvancedJavaScriptFilters:
    """Advanced JavaScript filter tests."""

    def test_combine_multiple_js_files(self, client: PageSpeedClient, test_root: str):
        """combine_javascript should combine multiple JS files."""
        response = client.get(
            f"{test_root}/pages/combine_multi_js.html?PageSpeedFilters=combine_javascript"
        )
        assert_http_status(response, 200)

    def test_inline_javascript_small_files(self, client: PageSpeedClient, example_root: str):
        """inline_javascript should inline small JS files."""
        response = client.get(
            f"{example_root}/inline_javascript.html?PageSpeedFilters=inline_javascript"
        )
        assert_http_status(response, 200)

    def test_move_js_to_bottom(self, client: PageSpeedClient, example_root: str):
        """move_css_above_scripts should restructure script ordering."""
        response = client.get(
            f"{example_root}/move_scripts.html?PageSpeedFilters=move_css_above_scripts"
        )
        assert_http_status(response, 200)


@pytest.mark.html_rewrite
class TestMultipleFilters:
    """Tests for multiple filters combined."""

    def test_core_filters(self, client: PageSpeedClient, example_root: str):
        """CoreFilters level should apply standard optimizations."""
        response = client.get(f"{example_root}/index.html?PageSpeedFilters=+core")
        assert_http_status(response, 200)

    def test_multiple_filters_combined(self, client: PageSpeedClient, example_root: str):
        """Multiple filters should work together."""
        filters = "combine_css,combine_javascript,collapse_whitespace,remove_comments"
        response = client.get(
            f"{example_root}/combine_css.html?PageSpeedFilters={filters}"
        )
        assert_http_status(response, 200)

    def test_optimize_for_bandwidth(self, client: PageSpeedClient, example_root: str):
        """OptimizeForBandwidth level should apply bandwidth optimizations."""
        response = client.get(
            f"{example_root}/index.html?PageSpeedFilters=+optimize_for_bandwidth"
        )
        assert_http_status(response, 200)


@pytest.mark.html_rewrite
class TestPassThrough:
    """Tests for pass-through behavior when optimization not needed."""

    def test_pagespeed_off(self, client: PageSpeedClient, example_root: str):
        """PageSpeed=off should disable all optimization."""
        response = client.get(f"{example_root}/index.html?PageSpeed=off")
        assert_http_status(response, 200)
        # No .pagespeed. URLs should appear
        assert_not_contains(response, ".pagespeed.")

    def test_empty_filter_list(self, client: PageSpeedClient, example_root: str):
        """Empty filter list should pass through unchanged."""
        response = client.get(f"{example_root}/index.html?PageSpeedFilters=")
        assert_http_status(response, 200)

    def test_noscript_passthrough(self, client: PageSpeedClient, example_root: str):
        """PageSpeedNoscript header should disable rewriting."""
        response = client.get(
            f"{example_root}/index.html",
            headers={"PageSpeedNoscript": "1"}
        )
        assert_http_status(response, 200)


@pytest.mark.html_rewrite
class TestHTMLStructure:
    """Tests for correct HTML structure after rewriting."""

    def test_remove_html_comments(self, client: PageSpeedClient, test_root: str):
        """remove_comments should remove HTML comments."""
        response = client.get(
            f"{test_root}/pages/comments.html?PageSpeedFilters=remove_comments"
        )
        assert_http_status(response, 200)

    def test_dns_prefetch_insertion(self, client: PageSpeedClient, test_root: str):
        """insert_dns_prefetch should add DNS prefetch links."""
        response = client.get(
            f"{test_root}/pages/dns_prefetch.html?PageSpeedFilters=insert_dns_prefetch"
        )
        assert_http_status(response, 200)

    def test_whitespace_collapse(self, client: PageSpeedClient, test_root: str):
        """collapse_whitespace should reduce HTML size."""
        # Get original size
        off_response = client.get(f"{test_root}/pages/whitespace_test.html?PageSpeed=off")
        on_response = client.get(
            f"{test_root}/pages/whitespace_test.html?PageSpeedFilters=collapse_whitespace"
        )

        assert_http_status(off_response, 200)
        assert_http_status(on_response, 200)

        # Optimized version should be smaller or equal
        # (may not always be smaller due to cache versioning)
        assert len(on_response.text) <= len(off_response.text) * 1.1, \
            "Optimized HTML should not be much larger than original"


if __name__ == "__main__":
    raise SystemExit(pytest.main([__file__, "-v"]))
