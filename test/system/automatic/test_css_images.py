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

"""CSS image cache extension tests.

Ported from: pagespeed/automatic/system_tests/css_images.sh

These tests verify that images referenced in CSS have their cache extended.
"""

import pytest

from pagespeed_test_framework import (
    PageSpeedClient,
    assert_contains,
    assert_http_status,
)


class TestCssImageCacheExtension:
    """Tests for extending cache of images referenced in CSS.

    Bash original::

        start_test rewrite_css,extend_cache extends cache of images in CSS.
        FILE=rewrite_css_images.html?PageSpeedFilters=rewrite_css,extend_cache
        URL=$EXAMPLE_ROOT/$FILE
        FETCHED=$WGET_DIR/$FILE
        fetch_until $URL 'grep -c Cuppa.png.pagespeed.ce.' 1  # image cache extended
        fetch_until $URL 'grep -c rewrite_css_images.css.pagespeed.cf.' 1
        check run_wget_with_args $URL
    """

    def test_image_cache_extended_in_css(
        self, client: PageSpeedClient, example_root: str
    ):
        """Images in CSS should have cache extended with .pagespeed.ce. prefix."""
        url = f"{example_root}/rewrite_css_images.html?PageSpeedFilters=rewrite_css,extend_cache"

        response = client.fetch_until_count(
            url,
            pattern=r"Cuppa\.png\.pagespeed\.ce\.",
            expected_count=1,
            timeout=30.0,
        )
        assert_http_status(response, 200)

        # Verify the image URL has the cache extension marker
        assert_contains(
            response,
            r"Cuppa\.png\.pagespeed\.ce\.",
            "Image in CSS should have cache extended",
        )

    def test_css_file_rewritten(
        self, client: PageSpeedClient, example_root: str
    ):
        """CSS file should be rewritten with .pagespeed.cf. marker."""
        url = f"{example_root}/rewrite_css_images.html?PageSpeedFilters=rewrite_css,extend_cache"

        response = client.fetch_until_count(
            url,
            pattern=r"rewrite_css_images\.css\.pagespeed\.cf\.",
            expected_count=1,
            timeout=30.0,
        )
        assert_http_status(response, 200)

        # Verify the CSS URL has the rewrite marker
        assert_contains(
            response,
            r"rewrite_css_images\.css\.pagespeed\.cf\.",
            "CSS file should be rewritten",
        )

    def test_css_and_image_both_optimized(
        self, client: PageSpeedClient, example_root: str
    ):
        """Both CSS and images within should be optimized together."""
        url = f"{example_root}/rewrite_css_images.html?PageSpeedFilters=rewrite_css,extend_cache"

        # First ensure both optimizations complete
        response = client.fetch_until(
            url,
            condition=lambda r: (
                "pagespeed.ce." in r.text and "pagespeed.cf." in r.text
            ),
            timeout=30.0,
        )
        assert_http_status(response, 200)

        # Verify both patterns are present
        assert_contains(
            response,
            r"\.pagespeed\.ce\.",
            "Image cache extension marker should be present",
        )
        assert_contains(
            response,
            r"\.pagespeed\.cf\.",
            "CSS filter marker should be present",
        )


if __name__ == "__main__":
    pytest.main([__file__, "-v"])
