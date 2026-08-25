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

"""Fallback rewrite CSS URLs filter tests.

Ported from: pagespeed/automatic/system_tests/fallback_rewrite_css_urls.sh

These tests verify that the fallback_rewrite_css_urls filter correctly
rewrites URLs in CSS even when the CSS itself cannot be fully parsed.
"""

import pytest

from pagespeed_test_framework import (
    PageSpeedClient,
    assert_contains,
    assert_http_status,
)


class TestFallbackRewriteCssUrls:
    """Tests for the fallback_rewrite_css_urls filter.

    Bash original::

        start_test fallback_rewrite_css_urls works.
        FILE=fallback_rewrite_css_urls.html?\
        PageSpeedFilters=fallback_rewrite_css_urls,rewrite_css,extend_cache
        URL=$EXAMPLE_ROOT/$FILE
        fetch_until $URL 'grep -c Cuppa.png.pagespeed.ce.' 1  # image cache extended
        fetch_until -save $URL 'grep -c fallback_rewrite_css_urls.css.pagespeed.cf.' 1
        # Test this was fallback flow -> no minification.
        check grep -q "body { background" $FETCH_FILE
    """

    def test_fallback_extends_image_cache(
        self, client: PageSpeedClient, example_root: str
    ):
        """Fallback rewrite should extend cache for images in CSS."""
        url = (
            f"{example_root}/fallback_rewrite_css_urls.html"
            "?PageSpeedFilters=fallback_rewrite_css_urls,rewrite_css,extend_cache"
        )

        response = client.fetch_until_count(
            url,
            pattern=r"Cuppa\.png\.pagespeed\.ce\.",
            expected_count=1,
            timeout=30.0,
        )
        assert_http_status(response, 200)

    def test_fallback_rewrites_css(
        self, client: PageSpeedClient, example_root: str
    ):
        """Fallback rewrite should rewrite CSS file."""
        url = (
            f"{example_root}/fallback_rewrite_css_urls.html"
            "?PageSpeedFilters=fallback_rewrite_css_urls,rewrite_css,extend_cache"
        )

        response = client.fetch_until_count(
            url,
            pattern=r"fallback_rewrite_css_urls\.css\.pagespeed\.cf\.",
            expected_count=1,
            timeout=30.0,
        )
        assert_http_status(response, 200)

    def test_fallback_no_minification(
        self, client: PageSpeedClient, example_root: str
    ):
        """Fallback flow should not minify CSS (preserves whitespace)."""
        url = (
            f"{example_root}/fallback_rewrite_css_urls.html"
            "?PageSpeedFilters=fallback_rewrite_css_urls,rewrite_css,extend_cache"
        )

        response = client.fetch_until_count(
            url,
            pattern=r"fallback_rewrite_css_urls\.css\.pagespeed\.cf\.",
            expected_count=1,
            timeout=30.0,
        )
        assert_http_status(response, 200)

        # In fallback mode, CSS is not minified (whitespace preserved)
        assert_contains(
            response,
            r"body \{ background",
            "Fallback flow should not minify CSS",
        )


if __name__ == "__main__":
    raise SystemExit(pytest.main([__file__, "-v"]))
