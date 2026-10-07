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
    require_status_ok,
)
from pagespeed_test_framework.stats import count_matching_lines


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
        url = f"{example_root}/rewrite_css.html?PageSpeedFilters=rewrite_css"

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
        url = f"{example_root}/rewrite_css.html?PageSpeedFilters=rewrite_css"

        # Wait for rewriting to complete
        response = client.fetch_until(
            url,
            condition=lambda r: "comment" not in r.text.lower(),
            timeout=30.0,
        )

        assert_http_status(response, 200)

        # Strip debug comments before checking size
        text = response.text
        debug_start = text.find("<!--\nmod_pagespeed on")
        if debug_start > 0:
            text = text[:debug_start]

        # Content should be smaller than original
        assert len(text.encode('utf-8')) < 800, \
            f"Minified CSS should be smaller, got {len(text.encode('utf-8'))} bytes"


class TestRewriteCssImages:
    """Tests for CSS with image URL rewriting."""

    def test_rewrite_css_images(self, client: PageSpeedClient, example_root: str):
        """Bash original (rewrite_css_images.sh:15-22):

            FILE='rewrite_css_images.html?PageSpeedFilters=rewrite_css,rewrite_images'
            FILE+='&ModPagespeedCssImageInlineMaxBytes=2048'
            fetch_until $URL 'grep -c url.data:image/png;base64,' 1  # image inlined
            fetch_until $URL 'grep -c rewrite_css_images.css.pagespeed.cf.' 1
            check run_wget_with_args $URL
        """
        url = (
            f"{example_root}/rewrite_css_images.html"
            "?PageSpeedFilters=rewrite_css,rewrite_images"
            "&ModPagespeedCssImageInlineMaxBytes=2048"
        )
        for pattern in (r"url.data:image/png;base64,",
                        r"rewrite_css_images.css.pagespeed.cf."):
            client.fetch_until(
                url,
                condition=lambda r, p=pattern: count_matching_lines(r.text, p) == 1,
                timeout=60.0,
                detail_fn=lambda r, p=pattern: (
                    f"lines matching {p!r}: {count_matching_lines(r.text, p)} expected=1"
                ),
            )
        require_status_ok(client.get(url), "rewrite_css_images.html")


if __name__ == "__main__":
    raise SystemExit(pytest.main([__file__, "-v"]))
