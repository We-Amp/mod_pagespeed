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

"""CSS and JavaScript inlining filter tests.

Ported from: pagespeed/automatic/system_tests/inliners.sh

These tests verify that the inline_css and inline_javascript filters work.
"""

import re

import pytest

from pagespeed_test_framework import (
    PageSpeedClient,
    assert_contains,
    assert_not_contains,
    assert_http_status,
)


class TestInlineCss:
    """Tests for the inline_css filter.

    Bash original:
        test_filter inline_css converts 3 out of 5 link tags to style tags.
        fetch_until $URL 'grep -c <style' 3
    """

    def test_inline_css_converts_links_to_styles(
        self, client: PageSpeedClient, example_root: str
    ):
        """inline_css should convert small CSS link tags to inline style tags.

        The test page has 5 link tags, 3 of which are small enough to inline.
        """
        url = f"{example_root}/inline_css.html?PageSpeedFilters=inline_css"

        # Wait until we see 3 inline style tags
        response = client.fetch_until_count(
            url,
            pattern=r'<style',
            expected_count=3,
            timeout=30.0,
        )

        assert_http_status(response, 200)


class TestInlineJavascript:
    """Tests for the inline_javascript filter.

    Bash original:
        test_filter inline_javascript inlines a small JS file.
        fetch_until $URL 'grep -c document.write' 1
    """

    def test_inline_javascript_inlines_small_files(
        self, client: PageSpeedClient, example_root: str
    ):
        """inline_javascript should inline small JS files."""
        url = f"{example_root}/inline_javascript.html?PageSpeedFilters=inline_javascript"

        # Wait until we see the inlined document.write call
        response = client.fetch_until_contains(
            url,
            pattern=r'document\.write',
            timeout=30.0,
        )

        assert_http_status(response, 200)
        # The JavaScript should be inlined directly in the HTML
        # (may be wrapped in CDATA comments)
        assert_contains(response, r'<script[^>]*>.*document\.write', flags=re.DOTALL)


class TestGzipEncodedResources:
    """Tests for handling gzip-encoded resources.

    Bash original:
        start_test inlining gzip-encoded resources
        URL="$TEST_ROOT/gzip_precompressed/?PageSpeedFilters=+debug"
        fetch_until -save $URL 'fgrep -c gzip-encoded' 2
    """

    def test_gzip_encoded_resources_not_inlined(
        self, client: PageSpeedClient, test_root: str
    ):
        """Gzip-encoded resources should not be inlined.

        If a resource is already gzip-encoded on disk, we shouldn't
        inline the binary compressed data. Debug mode should show
        comments explaining this.
        """
        # Note: +debug ADDS debug to CoreFilters so we get inline_css+inline_javascript+debug
        url = f"{test_root}/gzip_precompressed/?PageSpeedFilters=+debug"

        # Wait until we see 2 "gzip-encoded" debug messages
        response = client.fetch_until_count(
            url,
            pattern=r'gzip-encoded',
            expected_count=2,
            timeout=30.0,
        )

        assert_http_status(response, 200)

        # Verify debug mode is showing the inliner filters are active
        assert_contains(response, r"Inline Javascript")
        assert_contains(response, r"Inline Css")

        # Check for the specific debug messages about gzip
        assert_contains(
            response,
            r"JS not inlined because it appears to be gzip-encoded",
        )
        assert_contains(
            response,
            r"CSS not inlined because it appears to be gzip-encoded",
        )


if __name__ == "__main__":
    raise SystemExit(pytest.main([__file__, "-v"]))
