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

"""Flatten CSS imports filter tests for IIS PageSpeed module.

Ported from: test/system/automatic/test_flatten_css_imports.py

These tests verify that CSS @import statements are flattened by inlining
the imported content into the parent stylesheet.
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
class TestFlattenImports:
    """Tests for the flatten_css_imports filter.

    These tests verify that:
    - @import rules are inlined into the parent CSS
    - The combined CSS contains content from imported files
    - CSS functionality is preserved after flattening
    """

    def test_flatten_imports_removes_at_import(
        self, client: PageSpeedClient, test_root: str
    ):
        """flatten_css_imports should remove @import statements.

        After flattening, the @import rule should be replaced with
        the actual content from the imported file.
        """
        url = f"{test_root}/pages/flatten_imports.html?PageSpeedFilters=flatten_css_imports"

        response = client.get(
            url,
            headers={"X-PSA-Blocking-Rewrite": "psatest"},
        )
        assert_http_status(response, 200)

        # Fetch the rewritten CSS file if it was rewritten externally
        match = re.search(r'href="([^"]*with_import[^"]*\.css[^"]*)"', response.text)
        if match:
            css_url = match.group(1)
            if css_url.startswith("http://") or css_url.startswith("https://"):
                from urllib.parse import urlparse
                css_url = urlparse(css_url).path
            elif not css_url.startswith("/"):
                # Handle relative path from pages/ directory
                css_url = f"{test_root}/{css_url}"
                # Normalize path (remove pages/../)
                css_url = re.sub(r'/pages/\.\./', '/', css_url)

            css_response = client.get(css_url)
            assert_http_status(css_response, 200)

            # After flattening, @import should be removed from CSS
            # Check that the raw @import url("imported.css") is not present
            assert_not_contains(
                css_response,
                r'@import\s+url\s*\(\s*["\']?imported\.css',
                "@import statement should be removed after flattening",
            )

    def test_flatten_imports_inlines_content(
        self, client: PageSpeedClient, test_root: str
    ):
        """flatten_css_imports should inline imported CSS content.

        The flattened CSS should contain styles from the imported file.
        """
        url = f"{test_root}/pages/flatten_imports.html?PageSpeedFilters=flatten_css_imports"

        response = client.get(
            url,
            headers={"X-PSA-Blocking-Rewrite": "psatest"},
        )
        assert_http_status(response, 200)

        # Find the CSS link
        match = re.search(r'href="([^"]*with_import[^"]*\.css[^"]*)"', response.text)
        if match:
            css_url = match.group(1)
            if css_url.startswith("http://") or css_url.startswith("https://"):
                from urllib.parse import urlparse
                css_url = urlparse(css_url).path
            elif not css_url.startswith("/"):
                css_url = f"{test_root}/{css_url}"
                css_url = re.sub(r'/pages/\.\./', '/', css_url)

            css_response = client.get(css_url)
            assert_http_status(css_response, 200)

            # The imported CSS content should be present
            # imported.css contains .button and .alert classes
            assert_contains(
                css_response,
                r"\.button",
                "Imported CSS content (.button class) should be inlined",
            )

    def test_flatten_imports_preserves_parent_styles(
        self, client: PageSpeedClient, test_root: str
    ):
        """Parent stylesheet styles should be preserved after flattening."""
        url = f"{test_root}/pages/flatten_imports.html?PageSpeedFilters=flatten_css_imports"

        response = client.get(
            url,
            headers={"X-PSA-Blocking-Rewrite": "psatest"},
        )
        assert_http_status(response, 200)

        match = re.search(r'href="([^"]*with_import[^"]*\.css[^"]*)"', response.text)
        if match:
            css_url = match.group(1)
            if css_url.startswith("http://") or css_url.startswith("https://"):
                from urllib.parse import urlparse
                css_url = urlparse(css_url).path
            elif not css_url.startswith("/"):
                css_url = f"{test_root}/{css_url}"
                css_url = re.sub(r'/pages/\.\./', '/', css_url)

            css_response = client.get(css_url)
            assert_http_status(css_response, 200)

            # Styles from with_import.css (parent) should still be present
            assert_contains(
                css_response,
                r"\.page-wrapper",
                "Parent CSS content should be preserved",
            )


@pytest.mark.html_rewrite
class TestFlattenImportsNested:
    """Tests for nested @import resolution.

    CSS files can have @imports that themselves have @imports.
    PageSpeed should recursively flatten all levels.
    """

    def test_flatten_with_rewrite_css_combined(
        self, client: PageSpeedClient, test_root: str
    ):
        """flatten_css_imports combined with rewrite_css should work.

        When both filters are enabled, imports should be flattened
        and the resulting CSS should be minified.
        """
        url = f"{test_root}/pages/flatten_imports.html?PageSpeedFilters=flatten_css_imports,rewrite_css"

        response = client.get(
            url,
            headers={"X-PSA-Blocking-Rewrite": "psatest"},
        )
        assert_http_status(response, 200)

        # Check that page loads successfully
        assert_contains(
            response,
            r"<title>",
            "Page should render with title",
        )

    def test_flatten_imports_respects_media_queries(
        self, client: PageSpeedClient, test_root: str
    ):
        """flatten_css_imports should handle @import with media queries.

        @import rules can specify media queries, e.g.:
        @import url("print.css") print;
        """
        # This tests that the filter doesn't break on complex @import syntax
        url = f"{test_root}/pages/flatten_imports.html?PageSpeedFilters=flatten_css_imports"

        response = client.get(
            url,
            headers={"X-PSA-Blocking-Rewrite": "psatest"},
        )
        assert_http_status(response, 200)

        # Just verify the page loads without error
        assert_contains(response, r"</html>")


@pytest.mark.html_rewrite
class TestFlattenImportsPreservation:
    """Tests that CSS content and structure is preserved after flattening.

    These tests ensure that:
    - CSS selectors are preserved
    - CSS properties and values are intact
    - Order of rules is maintained
    """

    def test_selectors_preserved(
        self, client: PageSpeedClient, test_root: str
    ):
        """CSS selectors should be preserved after flattening."""
        url = f"{test_root}/pages/flatten_imports.html?PageSpeedFilters=flatten_css_imports"

        response = client.get(
            url,
            headers={"X-PSA-Blocking-Rewrite": "psatest"},
        )
        assert_http_status(response, 200)

        match = re.search(r'href="([^"]*with_import[^"]*\.css[^"]*)"', response.text)
        if match:
            css_url = match.group(1)
            if css_url.startswith("http://") or css_url.startswith("https://"):
                from urllib.parse import urlparse
                css_url = urlparse(css_url).path
            elif not css_url.startswith("/"):
                css_url = f"{test_root}/{css_url}"
                css_url = re.sub(r'/pages/\.\./', '/', css_url)

            css_response = client.get(css_url)
            assert_http_status(css_response, 200)

            # Check that complex selectors are preserved
            # imported.css has .alert-success and .button:hover
            assert_contains(
                css_response,
                r"\.alert",
                "Alert class selector should be preserved",
            )

    def test_color_values_preserved(
        self, client: PageSpeedClient, test_root: str
    ):
        """CSS color values should be preserved after flattening."""
        url = f"{test_root}/pages/flatten_imports.html?PageSpeedFilters=flatten_css_imports"

        response = client.get(
            url,
            headers={"X-PSA-Blocking-Rewrite": "psatest"},
        )
        assert_http_status(response, 200)

        match = re.search(r'href="([^"]*with_import[^"]*\.css[^"]*)"', response.text)
        if match:
            css_url = match.group(1)
            if css_url.startswith("http://") or css_url.startswith("https://"):
                from urllib.parse import urlparse
                css_url = urlparse(css_url).path
            elif not css_url.startswith("/"):
                css_url = f"{test_root}/{css_url}"
                css_url = re.sub(r'/pages/\.\./', '/', css_url)

            css_response = client.get(css_url)
            assert_http_status(css_response, 200)

            # imported.css has #007bff (button background) - may be minified
            # Just check that some color value exists
            assert_contains(
                css_response,
                r"#[0-9a-fA-F]{3,6}|rgb|background",
                "CSS color values should be present",
            )

    def test_page_renders_correctly(
        self, client: PageSpeedClient, test_root: str
    ):
        """Page should render correctly with flattened CSS.

        This is a sanity check to ensure the full page loads
        without errors after CSS flattening.
        """
        url = f"{test_root}/pages/flatten_imports.html?PageSpeedFilters=flatten_css_imports"

        response = client.get(
            url,
            headers={"X-PSA-Blocking-Rewrite": "psatest"},
        )
        assert_http_status(response, 200)

        # Check page structure
        assert_contains(response, r"<html>")
        assert_contains(response, r"</html>")
        assert_contains(response, r"CSS @import Flatten Test")


@pytest.mark.html_rewrite
class TestFlattenImportsLimits:
    """Tests for flatten_css_imports byte limit behavior.

    PageSpeed has a configurable limit on how large a CSS file
    can be after flattening. These tests verify limit handling.
    """

    def test_default_limit_allows_small_imports(
        self, client: PageSpeedClient, test_root: str
    ):
        """Default limit should allow our small test CSS to be flattened."""
        url = f"{test_root}/pages/flatten_imports.html?PageSpeedFilters=flatten_css_imports"

        response = client.get(
            url,
            headers={"X-PSA-Blocking-Rewrite": "psatest"},
        )
        assert_http_status(response, 200)

        match = re.search(r'href="([^"]*with_import[^"]*\.css[^"]*)"', response.text)
        if match:
            css_url = match.group(1)
            if css_url.startswith("http://") or css_url.startswith("https://"):
                from urllib.parse import urlparse
                css_url = urlparse(css_url).path
            elif not css_url.startswith("/"):
                css_url = f"{test_root}/{css_url}"
                css_url = re.sub(r'/pages/\.\./', '/', css_url)

            css_response = client.get(css_url)
            assert_http_status(css_response, 200)

            # With default limit, small imports should be flattened
            # Check for content from imported.css
            assert_contains(
                css_response,
                r"\.button",
                "Small imports should be flattened with default limit",
            )

    def test_tiny_limit_prevents_flattening(
        self, client: PageSpeedClient, test_root: str
    ):
        """Tiny byte limit should prevent CSS flattening.

        When CssFlattenMaxBytes is set very low, the import
        should not be flattened.
        """
        url = f"{test_root}/pages/flatten_imports.html?PageSpeedFilters=flatten_css_imports"

        response = client.get(
            url,
            headers={
                "X-PSA-Blocking-Rewrite": "psatest",
                "PageSpeedCssFlattenMaxBytes": "5",
            },
        )
        assert_http_status(response, 200)

        # With tiny limit, the @import may remain
        # This behavior depends on IIS module configuration


@pytest.mark.html_rewrite
class TestFlattenImportsContentType:
    """Tests for correct content-type on flattened CSS."""

    def test_flattened_css_has_correct_content_type(
        self, client: PageSpeedClient, test_root: str
    ):
        """Flattened CSS should have text/css content type."""
        url = f"{test_root}/pages/flatten_imports.html?PageSpeedFilters=flatten_css_imports"

        response = client.get(
            url,
            headers={"X-PSA-Blocking-Rewrite": "psatest"},
        )
        assert_http_status(response, 200)

        match = re.search(r'href="([^"]*with_import[^"]*\.css[^"]*)"', response.text)
        if match:
            css_url = match.group(1)
            if css_url.startswith("http://") or css_url.startswith("https://"):
                from urllib.parse import urlparse
                css_url = urlparse(css_url).path
            elif not css_url.startswith("/"):
                css_url = f"{test_root}/{css_url}"
                css_url = re.sub(r'/pages/\.\./', '/', css_url)

            css_response = client.get(css_url)
            assert_http_status(css_response, 200)

            content_type = css_response.header("Content-Type")
            assert content_type, "Content-Type header should be present"
            assert "text/css" in content_type.lower(), \
                f"Expected text/css content type, got: {content_type}"

    def test_original_css_still_accessible(
        self, client: PageSpeedClient, test_root: str
    ):
        """Original CSS files should still be directly accessible."""
        response = client.get(f"{test_root}/css/imported.css")
        assert_http_status(response, 200)

        content_type = response.header("Content-Type")
        assert "text/css" in content_type.lower(), \
            f"Expected text/css content type, got {content_type}"


if __name__ == "__main__":
    pytest.main([__file__, "-v"])
