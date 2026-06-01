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

"""CSS rewriting filter tests.

Ported from: pagespeed/automatic/system_tests/rewrite_css.sh

These tests verify that CSS minification and rewriting work correctly.
"""

import pytest

from pagespeed_test_framework import (
    PageSpeedClient,
    assert_contains,
    assert_not_contains,
    assert_http_status,
    assert_file_size,
)


class TestRewriteCss:
    """Tests for the rewrite_css filter.

    Bash original:
        test_filter rewrite_css minifies CSS and saves bytes.
        fetch_until -save $URL 'grep -c comment' 0
        check_file_size $FETCH_FILE -lt 680
    """

    def test_rewrite_css_removes_comments(
        self, client: PageSpeedClient, example_root: str
    ):
        """rewrite_css should remove CSS comments."""
        url = f"{example_root}/rewrite_css.html?PageSpeedFilters=+rewrite_css"

        # Wait until CSS is minified (comments removed)
        response = client.fetch_until(
            url,
            condition=lambda r: "comment" not in r.text.lower()
            or r.text.lower().count("comment") == 0,
            timeout=30.0,
        )

        assert_http_status(response, 200)
        # Should not contain CSS comments
        assert_not_contains(response, r'/\*.*comment.*\*/')

    def test_rewrite_css_reduces_size(
        self, client: PageSpeedClient, example_root: str
    ):
        """rewrite_css should reduce CSS file size.

        The original CSS file is ~689 bytes, should be reduced to < 680.
        """
        url = f"{example_root}/rewrite_css.html?PageSpeedFilters=+rewrite_css"

        # Wait for rewriting to complete
        response = client.fetch_until(
            url,
            condition=lambda r: "comment" not in r.text.lower(),
            timeout=30.0,
        )

        assert_http_status(response, 200)
        # File should be smaller than original
        assert_file_size(response, "<", 680, "Minified CSS should be smaller")


class TestRewriteCssImages:
    """Tests for CSS with image URL rewriting."""

    def test_rewrite_css_images(self, client: PageSpeedClient, example_root: str):
        """CSS with images should have URLs rewritten."""
        url = f"{example_root}/rewrite_css_images.html?PageSpeedFilters=rewrite_css,rewrite_images"

        # Wait for CSS to be rewritten
        response = client.fetch_until_contains(
            url,
            pattern=r'\.pagespeed\.',
            timeout=60.0,
        )

        assert_http_status(response, 200)


if __name__ == "__main__":
    pytest.main([__file__, "-v"])
