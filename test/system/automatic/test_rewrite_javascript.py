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

"""JavaScript rewriting filter tests.

Ported from: pagespeed/automatic/system_tests/rewrite_javascript.sh

These tests verify that JavaScript minification works correctly.
"""

import re

import pytest

from pagespeed_test_framework import (
    PageSpeedClient,
    assert_contains,
    assert_not_contains,
    assert_http_status,
    assert_file_size,
    assert_header_contains,
    require_match,
)


class TestRewriteJavascript:
    """Tests for the rewrite_javascript filter.

    Bash original::

        test_filter rewrite_javascript minifies JavaScript and saves bytes.
        fetch_until -save -recursive $URL 'grep -c src=.*rewrite_javascript.js.pagespeed.jm.' 2
    """

    def test_rewrite_javascript_minifies(
        self, client: PageSpeedClient, example_root: str
    ):
        """rewrite_javascript should minify external JS files."""
        url = f"{example_root}/rewrite_javascript.html?PageSpeedFilters=rewrite_javascript"

        response = client.fetch_until_count(
            url,
            pattern=r"src=.*rewrite_javascript\.js\.pagespeed\.jm\.",
            expected_count=2,
            timeout=30.0,
        )

        assert_http_status(response, 200)

    def test_rewrite_javascript_removes_comments(
        self, client: PageSpeedClient, example_root: str
    ):
        """Minified JS should not contain comments.

        Bash original:
            check_not grep removed $WGET_DIR/*.pagespeed.jm.*
        """
        url = f"{example_root}/rewrite_javascript.html?PageSpeedFilters=rewrite_javascript"

        response = client.fetch_until(
            url,
            condition=lambda r: re.search(
                r'src="[^"]*\.pagespeed\.jm\.[^"]*"', r.text
            )
            is not None,
            timeout=30.0,
        )

        # Extract rewritten JS URL
        match = require_match(
            r'src="([^"]*\.pagespeed\.jm\.[^"]*)"',
            response,
            "rewritten JS URL",
        )

        js_url = match.group(1)
        # Handle both absolute URLs (http://...) and relative URLs
        if js_url.startswith("http://") or js_url.startswith("https://"):
            from urllib.parse import urlparse
            js_url = urlparse(js_url).path
        elif not js_url.startswith("/"):
            js_url = f"{example_root}/{js_url}"

        js_response = client.get(js_url)
        assert_http_status(js_response, 200)

        # Should not contain "removed" (from comments that should be removed)
        assert_not_contains(js_response, r"\bremoved\b")

    def test_rewrite_javascript_preserves_special_comments(
        self, client: PageSpeedClient, example_root: str
    ):
        """Minified JS should preserve certain comments (e.g., license).

        Bash original:
            check grep -q preserved $FETCH_FILE
        """
        url = f"{example_root}/rewrite_javascript.html?PageSpeedFilters=rewrite_javascript"

        response = client.fetch_until_contains(
            url,
            pattern=r"\.pagespeed\.jm\.",
            timeout=30.0,
        )

        # The HTML page should still have "preserved" in important comments
        assert_contains(response, r"preserved")

    def test_rewrite_javascript_saves_bytes(
        self, client: PageSpeedClient, example_root: str
    ):
        """Minified page should be smaller.

        Bash original:
            check_file_size $FETCH_FILE -lt 1560
        """
        url = f"{example_root}/rewrite_javascript.html?PageSpeedFilters=rewrite_javascript"

        response = client.fetch_until_contains(
            url,
            pattern=r"\.pagespeed\.jm\.",
            timeout=30.0,
        )

        assert_http_status(response, 200)
        assert_file_size(response, "<", 1560, "Minified page should be smaller")

    def test_rewritten_js_cache_extended(
        self, client: PageSpeedClient, example_root: str
    ):
        """Rewritten JS should have long cache lifetime.

        Bash original:
            check grep -qi "Cache-control: max-age=31536000" $WGET_OUTPUT
        """
        url = f"{example_root}/rewrite_javascript.html?PageSpeedFilters=rewrite_javascript"

        response = client.fetch_until(
            url,
            condition=lambda r: re.search(
                r'src="[^"]*\.pagespeed\.jm\.[^"]*"', r.text
            )
            is not None,
            timeout=30.0,
        )

        match = require_match(
            r'src="([^"]*\.pagespeed\.jm\.[^"]*)"',
            response,
            "rewritten JS URL",
        )

        js_url = match.group(1)
        # Handle both absolute URLs (http://...) and relative URLs
        if js_url.startswith("http://") or js_url.startswith("https://"):
            from urllib.parse import urlparse
            js_url = urlparse(js_url).path
        elif not js_url.startswith("/"):
            js_url = f"{example_root}/{js_url}"

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


class TestRewriteJavascriptExternal:
    """Tests for rewrite_javascript_external filter.

    Bash original:
        start_test rewrite_javascript_external
        fetch_until -save "$URL?PageSpeedFilters=rewrite_javascript_external"
    """

    def test_rewrite_javascript_external_only(
        self, client: PageSpeedClient, example_root: str
    ):
        """rewrite_javascript_external should only rewrite external scripts."""
        url = f"{example_root}/rewrite_javascript.html?PageSpeedFilters=rewrite_javascript_external"

        response = client.fetch_until_count(
            url,
            pattern=r"src=.*rewrite_javascript\.js\.pagespeed\.jm\.",
            expected_count=2,
            timeout=30.0,
        )

        assert_http_status(response, 200)

        # Inline comments should NOT be removed (only external is rewritten)
        assert_contains(
            response,
            r"This comment will be removed",
            "Inline JS should not be minified by external-only filter",
        )


class TestRewriteJavascriptInline:
    """Tests for rewrite_javascript_inline filter.

    Bash original:
        start_test rewrite_javascript_inline
        OUT=$($WGET_DUMP --header=X-PSA-Blocking-Rewrite:psatest
              $URL?PageSpeedFilters=rewrite_javascript_inline)
    """

    def test_rewrite_javascript_inline_only(
        self, client: PageSpeedClient, example_root: str
    ):
        """rewrite_javascript_inline should only rewrite inline scripts."""
        url = f"{example_root}/rewrite_javascript.html?PageSpeedFilters=rewrite_javascript_inline"

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
            r'id="int1">var state=0;document\.write',
            "Inline JS should be minified",
        )

        # External JS links should be left alone (not rewritten)
        assert response.text.count('src="rewrite_javascript.js"') == 2, \
            "External JS should not be rewritten by inline-only filter"

        assert_not_contains(
            response,
            r"src=.*rewrite_javascript\.js\.pagespeed\.jm\.",
            "External JS should not be rewritten",
        )


class TestJavascriptRegressions:
    """Regression tests for JavaScript handling."""

    def test_outlined_resource_not_in_cache(
        self, client: PageSpeedClient, test_root: str
    ):
        """Fetch of outlined resources not in cache should not leak.

        Bash original:
            start_test regression test for RewriteDriver leak
            check_not $WGET ... $TEST_ROOT/_.pagespeed.jo.3tPymVdi9b.js
        """
        url = f"{test_root}/_.pagespeed.jo.3tPymVdi9b.js"
        response = client.get(url)
        # Should return 404, not crash or leak
        assert_http_status(response, 404)


if __name__ == "__main__":
    raise SystemExit(pytest.main([__file__, "-v"]))
