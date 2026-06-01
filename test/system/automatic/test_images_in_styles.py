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

"""Images in style attributes tests.

Ported from: pagespeed/automatic/system_tests/images_in_styles.sh

These tests verify that images referenced in inline style attributes
are correctly rewritten.
"""

import pytest

from pagespeed_test_framework import (
    PageSpeedClient,
    assert_contains,
    assert_http_status,
)


class TestImagesInStyles:
    """Tests for rewriting images in style attributes.

    Bash original::

        start_test rewrite_images,rewrite_css,rewrite_style_attributes_with_url optimizes images in style.
        FILE=rewrite_style_attributes.html?PageSpeedFilters=rewrite_images,rewrite_css,rewrite_style_attributes_with_url
        URL=$EXAMPLE_ROOT/$FILE
        fetch_until $URL 'grep -c BikeCrashIcn.png.pagespeed.ic.' 1
    """

    def test_images_in_style_attributes_rewritten(
        self, client: PageSpeedClient, example_root: str
    ):
        """Images in style attributes should be rewritten."""
        url = (
            f"{example_root}/rewrite_style_attributes.html"
            "?PageSpeedFilters=rewrite_images,rewrite_css,rewrite_style_attributes_with_url"
        )

        response = client.fetch_until_count(
            url,
            pattern=r"BikeCrashIcn\.png\.pagespeed\.ic\.",
            expected_count=1,
            timeout=30.0,
        )
        assert_http_status(response, 200)


class TestMultipleImagesInStyles:
    """Tests for multiple images in the same style block.

    Bash original::

        start_test two images in the same style block
        FILE="rewrite_style_attributes_dual.html?PageSpeedFilters="
        FILE+="rewrite_images,rewrite_css,rewrite_style_attributes_with_url"
        URL=$EXAMPLE_ROOT/$FILE
        PATTERN="BikeCrashIcn.png.pagespeed.ic.*BikeCrashIcn.png.pagespeed.ic"
        fetch_until $URL "grep -c $PATTERN" 1
    """

    def test_two_images_in_same_style(
        self, client: PageSpeedClient, example_root: str
    ):
        """Two images in the same style block should both be rewritten."""
        url = (
            f"{example_root}/rewrite_style_attributes_dual.html"
            "?PageSpeedFilters=rewrite_images,rewrite_css,rewrite_style_attributes_with_url"
        )

        # Both images should be rewritten
        response = client.fetch_until_contains(
            url,
            pattern=r"BikeCrashIcn\.png\.pagespeed\.ic\..*BikeCrashIcn\.png\.pagespeed\.ic\.",
            timeout=30.0,
        )
        assert_http_status(response, 200)


if __name__ == "__main__":
    pytest.main([__file__, "-v"])
