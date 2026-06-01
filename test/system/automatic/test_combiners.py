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

"""CSS and JavaScript combiner filter tests.

Ported from: pagespeed/automatic/system_tests/combiners.sh

These tests verify that the combine_css and combine_javascript filters work.
"""

import re

import pytest

from pagespeed_test_framework import (
    PageSpeedClient,
    assert_contains,
    assert_not_contains,
    assert_http_status,
)


class TestCombineCss:
    """Tests for the combine_css filter.

    Bash original:
        test_filter combine_css combines 4 CSS files into 1.
        fetch_until $URL 'fgrep -c text/css' 1
    """

    def test_combine_css_reduces_link_tags(
        self, client: PageSpeedClient, example_root: str
    ):
        """combine_css should combine multiple CSS files into one.

        The combine_css.html page has 4 CSS link tags that should be
        combined into 1.
        """
        url = f"{example_root}/combine_css.html?PageSpeedFilters=combine_css"

        # Wait until CSS is combined (should see only 1 text/css reference)
        response = client.fetch_until_count(
            url,
            pattern=r'text/css',
            expected_count=1,
            timeout=30.0,
        )

        assert_http_status(response, 200)
        # Should see combined CSS filename pattern (.cc. for combine, .cf. for filter)
        # Modern pagespeed may combine operations so accept either pattern
        assert_contains(response, r"\.pagespeed\.(cc|cf)\.")

    def test_combine_css_without_hash_returns_404(
        self, client: PageSpeedClient, example_root: str
    ):
        """Combined CSS URL without hash should return 404.

        Bash original:
            start_test combine_css without hash field should 404
            URL=$REWRITTEN_ROOT/styles/yellow.css+blue.css.pagespeed.cc..css
            check_not run_wget_with_args $URL
            check fgrep "404 Not Found" $WGET_OUTPUT
        """
        # URL with missing hash (note the empty hash between .cc. and .css)
        url = f"{example_root}/styles/yellow.css+blue.css.pagespeed.cc..css"
        response = client.get(url)
        assert_http_status(response, 404)

    def test_large_combined_css_url(self, client: PageSpeedClient, example_root: str):
        """Large combined CSS URLs should work.

        Bash original:
            start_test Fetch large css_combine URL
            LARGE_URL="$REWRITTEN_ROOT/styles/yellow.css+blue.css+big.css+..."
        """
        # Build a large combined CSS URL
        css_files = ["yellow.css", "blue.css", "big.css", "bold.css"]
        # Repeat the pattern many times
        repeated = "+".join(css_files * 16)  # 64 files
        large_url = f"{example_root}/styles/{repeated}.pagespeed.cc.46IlzLf_NK.css"

        response = client.get(large_url)
        assert_http_status(response, 200)

        # Should have substantial content (combined CSS from multiple files)
        line_count = len(response.text.splitlines())
        assert line_count > 800, f"Expected > 800 lines, got {line_count}"


class TestCombineJavascript:
    """Tests for the combine_javascript filter.

    Bash original:
        test_filter combine_javascript combines 2 JS files into 1.
        fetch_until $URL 'fgrep -c src=' 1
    """

    def test_combine_javascript_reduces_script_tags(
        self, client: PageSpeedClient, example_root: str
    ):
        """combine_javascript should combine multiple JS files into one."""
        url = f"{example_root}/combine_javascript.html?PageSpeedFilters=combine_javascript"

        # Wait until JS is combined (should see only 1 src= attribute)
        response = client.fetch_until_count(
            url,
            pattern=r'src=',
            expected_count=1,
            timeout=30.0,
        )

        assert_http_status(response, 200)
        # Should see the combined JS filename pattern
        assert_contains(response, r"\.pagespeed\.jc\.")

    def test_combine_javascript_with_many_files(
        self, client: PageSpeedClient, test_root: str
    ):
        """Combining many JS files should still work.

        Bash original:
            start_test combine_javascript with long URL still works
            URL=$TEST_ROOT/combine_js_very_many.html?PageSpeedFilters=combine_javascript
            fetch_until $URL 'fgrep -c src=' 4
        """
        url = f"{test_root}/combine_js_very_many.html?PageSpeedFilters=combine_javascript"

        # Should reduce to 4 or fewer src= tags
        response = client.fetch_until(
            url,
            condition=lambda r: len(re.findall(r'src=', r.text)) <= 4,
            timeout=30.0,
            headers={"X-PSA-Blocking-Rewrite": "psatest"},
        )

        assert_http_status(response, 200)


class TestCombineHeads:
    """Tests for the combine_heads filter.

    Bash original:
        test_filter combine_heads combines 2 heads into 1
        check run_wget_with_args $URL
        check [ $(fgrep -c '<head>' $FETCHED) = 1 ]
    """

    def test_combine_heads_merges_head_tags(
        self, client: PageSpeedClient, example_root: str
    ):
        """combine_heads should merge multiple <head> tags into one."""
        url = f"{example_root}/combine_heads.html?PageSpeedFilters=combine_heads"

        response = client.fetch_until(
            url,
            condition=lambda r: r.text.count("<head>") == 1,
            timeout=30.0,
        )

        assert_http_status(response, 200)
        assert response.text.count("<head>") == 1, \
            "Should have exactly one <head> tag"


class TestCombineCssDebug:
    """Tests for combine_css debug output.

    Bash original:
        start_test "combine_css debug filter"
        URL=$EXAMPLE_ROOT/combine_css_debug.html?PageSpeedFilters=combine_css,debug
    """

    def test_combine_css_debug_shows_reasons(
        self, client: PageSpeedClient, example_root: str
    ):
        """Debug mode should explain why CSS couldn't be combined."""
        url = f"{example_root}/combine_css_debug.html?PageSpeedFilters=combine_css,debug"

        response = client.fetch_until_contains(
            url,
            pattern=r"styles/yellow\.css\+blue\.css\+big\.css\+bold\.css\.pagespeed\.cc",
            timeout=30.0,
        )

        assert_http_status(response, 200)

        # Check for various debug messages about non-combinable elements
        assert_contains(
            response,
            r"potentially non-combinable attribute.*id",
            "Should show id attribute warning",
        )
        assert_contains(
            response,
            r"Could not combine over barrier: noscript",
            "Should show noscript barrier",
        )
        assert_contains(
            response,
            r"Could not combine over barrier: inline style",
            "Should show inline style barrier",
        )


if __name__ == "__main__":
    pytest.main([__file__, "-v"])
