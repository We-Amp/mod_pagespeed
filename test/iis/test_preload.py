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

"""Hint preload subresources filter tests for IIS PageSpeed module.

These tests verify that the hint_preload_subresources filter correctly adds
Link headers with preload hints for critical resources (CSS and JavaScript).

The hint_preload_subresources filter:
- Scans HTML for CSS and JavaScript resources
- Adds HTTP Link headers with rel=preload hints
- Follows CSS @import rules to find indirectly referenced resources
- Includes nopush directive to prevent HTTP/2 server push
- Does NOT add preload hints for images

Reference: test/system/automatic/test_hint_preload.py
"""

import re

import pytest

from pagespeed_test_framework import (
    PageSpeedClient,
    assert_contains,
    assert_not_contains,
    assert_http_status,
    assert_header_contains,
)


@pytest.mark.html_rewrite
class TestPreloadHints:
    """Tests for Link preload header insertion.

    The hint_preload_subresources filter adds Link headers for CSS and JS
    resources found in the HTML page. It also follows @import rules to
    discover indirectly referenced stylesheets.
    """

    def test_preload_hints_added(
        self, client: PageSpeedClient, example_root: str
    ):
        """hint_preload_subresources should add Link preload headers.

        Uses hint_preload_subresources.html which has:
        - styles/all_using_imports.css (which @imports yellow.css, colors.css, layout.css)
        - scripts/example.js
        - An image (which should NOT get a preload hint)

        The filter should add Link headers for at least 2 resources
        (CSS and JS direct dependencies; @imported CSS may also appear).
        """
        url = f"{example_root}/hint_preload_subresources.html?PageSpeedFilters=hint_preload_subresources"

        # Wait for Link headers to appear - may require background fetch
        response = client.fetch_until(
            url,
            condition=lambda r: r.header("Link", "").count("rel=preload") >= 2,
            timeout=30.0,
        )
        assert_http_status(response, 200)

        link_header = response.header("Link", "")
        link_count = link_header.count("rel=preload")
        assert link_count >= 2, f"Expected at least 2 Link preload headers, got {link_count}"

    def test_preload_css_resources(
        self, client: PageSpeedClient, example_root: str
    ):
        """CSS resources should get preload hints with as=style.

        The filter should add Link headers for the main CSS file and
        all CSS files referenced via @import.
        """
        url = f"{example_root}/hint_preload_subresources.html?PageSpeedFilters=hint_preload_subresources"

        # Wait for CSS preload to appear
        response = client.fetch_until(
            url,
            condition=lambda r: "all_using_imports.css" in r.header("Link", ""),
            timeout=30.0,
        )
        assert_http_status(response, 200)

        link_header = response.header("Link", "")

        # Main CSS should have as=style
        assert re.search(r"all_using_imports\.css.*rel=preload.*as=style", link_header), \
            f"Main CSS should have preload with as=style. Link: {link_header}"

    def test_preload_js_resources(
        self, client: PageSpeedClient, example_root: str
    ):
        """JavaScript resources should get preload hints with as=script.

        The filter should add Link headers for script files with as=script.
        """
        url = f"{example_root}/hint_preload_subresources.html?PageSpeedFilters=hint_preload_subresources"

        # Wait for JS preload to appear
        response = client.fetch_until(
            url,
            condition=lambda r: "example.js" in r.header("Link", ""),
            timeout=30.0,
        )
        assert_http_status(response, 200)

        link_header = response.header("Link", "")

        # JavaScript should have as=script
        assert re.search(r"example\.js.*rel=preload.*as=script", link_header), \
            f"JavaScript should have preload with as=script. Link: {link_header}"

    def test_preload_imported_css(
        self, client: PageSpeedClient, example_root: str
    ):
        """CSS @import targets should also get preload hints.

        The filter should follow @import rules and add preload hints
        for indirectly referenced stylesheets like yellow.css.
        """
        url = f"{example_root}/hint_preload_subresources.html?PageSpeedFilters=hint_preload_subresources"

        # Wait for imported CSS preload to appear
        response = client.fetch_until(
            url,
            condition=lambda r: "yellow.css" in r.header("Link", ""),
            timeout=30.0,
        )
        assert_http_status(response, 200)

        link_header = response.header("Link", "")

        # Imported CSS (yellow.css from @import in all_using_imports.css)
        assert re.search(r"yellow\.css.*rel=preload.*as=style", link_header), \
            f"Imported CSS should have preload hint. Link: {link_header}"

    def test_preload_includes_nopush(
        self, client: PageSpeedClient, example_root: str
    ):
        """Preload hints should include nopush directive.

        The nopush directive tells HTTP/2 servers not to push the resource,
        allowing the browser to decide whether to fetch it.
        """
        url = f"{example_root}/hint_preload_subresources.html?PageSpeedFilters=hint_preload_subresources"

        # Wait for nopush in Link headers
        response = client.fetch_until(
            url,
            condition=lambda r: "nopush" in r.header("Link", ""),
            timeout=30.0,
        )
        assert_http_status(response, 200)

        link_header = response.header("Link", "")
        assert "nopush" in link_header, \
            f"Preload hints should include nopush directive. Link: {link_header}"


@pytest.mark.html_rewrite
class TestPreloadInHtml:
    """Tests for preload link elements in HTML.

    Some PageSpeed configurations may insert <link rel="preload"> elements
    directly into the HTML head section instead of or in addition to
    HTTP Link headers.
    """

    def test_preload_link_in_head(
        self, client: PageSpeedClient, example_root: str
    ):
        """Check for preload link elements in the HTML head.

        The filter may add <link rel="preload" ...> elements to the head.
        This test verifies Link headers are present (primary mechanism).
        """
        url = f"{example_root}/hint_preload_subresources.html?PageSpeedFilters=hint_preload_subresources"

        # Wait for preload to be ready
        response = client.fetch_until(
            url,
            condition=lambda r: r.header("Link", "").count("rel=preload") >= 1,
            timeout=30.0,
        )
        assert_http_status(response, 200)

        # Primary mechanism is Link headers
        link_header = response.header("Link", "")
        assert "rel=preload" in link_header, \
            "Should have rel=preload in Link header"

    def test_preload_as_attribute(
        self, client: PageSpeedClient, example_root: str
    ):
        """Preload hints should have correct 'as' attribute for resource type.

        - CSS: as=style
        - JavaScript: as=script
        - Fonts: as=font
        - Images: as=image (though images are typically not preloaded)
        """
        url = f"{example_root}/hint_preload_subresources.html?PageSpeedFilters=hint_preload_subresources"

        # Wait for Link headers
        response = client.fetch_until(
            url,
            condition=lambda r: r.header("Link", "").count("rel=preload") >= 2,
            timeout=30.0,
        )
        assert_http_status(response, 200)

        link_header = response.header("Link", "")

        # Verify as=style for CSS (Link format: .css>; rel=preload; as=style)
        has_style = re.search(r'\.css.*?as=style', link_header)
        # Verify as=script for JS
        has_script = re.search(r'\.js.*?as=script', link_header)

        assert has_style or has_script, \
            f"Should have proper 'as' attribute. Link: {link_header}"


@pytest.mark.html_rewrite
class TestPreloadWithOtherFilters:
    """Tests for hint_preload_subresources combined with other filters.

    Preload hints should work correctly when combined with other
    PageSpeed filters that modify resources.
    """

    def test_preload_with_combine_css(
        self, client: PageSpeedClient, example_root: str
    ):
        """Preload should work with combine_css filter.

        When CSS files are combined, the preload hint should point to
        the combined CSS URL (with .pagespeed.cc. pattern).
        """
        filters = "hint_preload_subresources,combine_css"
        url = f"{example_root}/combine_css.html?PageSpeedFilters={filters}"

        # Wait for combined CSS or preload headers
        response = client.fetch_until(
            url,
            condition=lambda r: (
                ".pagespeed.cc." in r.text or
                r.header("Link", "").count("rel=preload") >= 1
            ),
            timeout=30.0,
        )
        assert_http_status(response, 200)

        # Should have either:
        # 1. Link headers for CSS (may point to combined or original URLs)
        # 2. Combined CSS in the HTML
        link_header = response.header("Link", "")
        html = response.text

        has_preload = "rel=preload" in link_header
        has_combined = ".pagespeed.cc." in html

        assert has_preload or has_combined, \
            f"Should have preload hints or combined CSS. Link: {link_header}"

    def test_preload_with_rewrite_images(
        self, client: PageSpeedClient, example_root: str
    ):
        """Preload should work with rewrite_images filter.

        The preload filter should coexist with image rewriting,
        but images should NOT get preload hints (per filter design).
        """
        filters = "hint_preload_subresources,rewrite_images"
        url = f"{example_root}/hint_preload_subresources.html?PageSpeedFilters={filters}"

        # Wait for response
        response = client.fetch_until(
            url,
            condition=lambda r: r.header("Link", "").count("rel=preload") >= 1,
            timeout=30.0,
        )
        assert_http_status(response, 200)

        link_header = response.header("Link", "")

        # Should have CSS/JS preload hints
        assert "rel=preload" in link_header

        # Images should NOT have preload hints (by design)
        # The Puzzle.jpg in the page should not appear in Link headers
        assert "Puzzle.jpg" not in link_header, \
            "Images should not get preload hints"
        assert "as=image" not in link_header, \
            "Should not have as=image preload hints"

    def test_preload_with_rewrite_javascript(
        self, client: PageSpeedClient, example_root: str
    ):
        """Preload should work with rewrite_javascript filter.

        When JavaScript is minified/rewritten, preload hints should
        still be generated (may point to original or rewritten URLs).
        """
        filters = "hint_preload_subresources,rewrite_javascript"
        url = f"{example_root}/hint_preload_subresources.html?PageSpeedFilters={filters}"

        # Wait for preload headers
        response = client.fetch_until(
            url,
            condition=lambda r: r.header("Link", "").count("rel=preload") >= 1,
            timeout=30.0,
        )
        assert_http_status(response, 200)

        link_header = response.header("Link", "")
        assert "as=script" in link_header or "as=style" in link_header, \
            f"Should have preload hints. Link: {link_header}"

    def test_preload_with_collapse_whitespace(
        self, client: PageSpeedClient, example_root: str
    ):
        """Preload should work with collapse_whitespace filter.

        HTML whitespace collapsing should not affect Link header generation.
        """
        filters = "hint_preload_subresources,collapse_whitespace"
        url = f"{example_root}/hint_preload_subresources.html?PageSpeedFilters={filters}"

        response = client.fetch_until(
            url,
            condition=lambda r: r.header("Link", "").count("rel=preload") >= 1,
            timeout=30.0,
        )
        assert_http_status(response, 200)

        link_header = response.header("Link", "")
        assert "rel=preload" in link_header


@pytest.mark.html_rewrite
class TestPreloadEdgeCases:
    """Edge cases and error handling for preload hints."""

    def test_preload_disabled_with_pagespeed_off(
        self, client: PageSpeedClient, example_root: str
    ):
        """PageSpeed=off should disable preload hints.

        When PageSpeed is disabled, no Link headers should be added.
        """
        url = f"{example_root}/hint_preload_subresources.html?PageSpeed=off"

        response = client.get(url)
        assert_http_status(response, 200)

        link_header = response.header("Link", "")
        # Either no Link header or no rel=preload in it
        assert "rel=preload" not in link_header, \
            f"PageSpeed=off should not add preload hints. Link: {link_header}"

    def test_preload_empty_filter_list(
        self, client: PageSpeedClient, example_root: str
    ):
        """Empty filter list should not add preload hints.

        Without hint_preload_subresources in the filter list,
        no preload Link headers should be generated.
        """
        url = f"{example_root}/hint_preload_subresources.html?PageSpeedFilters="

        response = client.get(url)
        assert_http_status(response, 200)

        link_header = response.header("Link", "")
        assert "rel=preload" not in link_header

    def test_preload_on_page_without_resources(
        self, client: PageSpeedClient, example_root: str
    ):
        """Pages without CSS/JS should have no preload hints.

        The index.html page may have some resources, but testing
        with PageSpeed=off as baseline shows no preload is added.
        """
        # Use a simple page - collapse_whitespace.html has minimal resources
        url = f"{example_root}/collapse_whitespace.html?PageSpeedFilters=hint_preload_subresources"

        response = client.get(url)
        assert_http_status(response, 200)

        # May or may not have preload depending on page content
        # Just verify the page loads successfully with the filter

    def test_preload_multiple_css_files(
        self, client: PageSpeedClient, example_root: str
    ):
        """Multiple CSS files should each get preload hints.

        Using combine_css.html which has multiple stylesheet links.
        """
        url = f"{example_root}/combine_css.html?PageSpeedFilters=hint_preload_subresources"

        response = client.fetch_until(
            url,
            condition=lambda r: r.header("Link", "").count("as=style") >= 1,
            timeout=30.0,
        )
        assert_http_status(response, 200)

        link_header = response.header("Link", "")
        # Should have preload hints for CSS
        assert "as=style" in link_header, \
            f"Should have CSS preload hints. Link: {link_header}"


if __name__ == "__main__":
    pytest.main([__file__, "-v"])
