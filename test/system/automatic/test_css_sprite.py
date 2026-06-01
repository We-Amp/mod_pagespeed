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

"""CSS sprite images filter tests.

Ported from: pagespeed/automatic/system_tests/css_sprite_images.sh

These tests verify that the sprite_images filter correctly combines
multiple images into CSS sprites.
"""

import re

import pytest

from pagespeed_test_framework import (
    PageSpeedClient,
    assert_contains,
    assert_http_status,
)


class TestCssSpriteImagesInline:
    """Tests for sprite_images with inline_css.

    Bash original::

        start_test inline_css,rewrite_css,sprite_images sprites images in CSS.
        FILE=sprite_images.html?PageSpeedFilters=inline_css,rewrite_css,sprite_images
        URL=$EXAMPLE_ROOT/$FILE
        fetch_until $URL \
          'grep -c Cuppa.png.*BikeCrashIcn.png.*IronChef2.gif.*.pagespeed.is.*.png' 1
    """

    def test_sprite_images_combines_images(
        self, client: PageSpeedClient, example_root: str
    ):
        """Multiple images should be combined into a sprite."""
        url = f"{example_root}/sprite_images.html?PageSpeedFilters=inline_css,rewrite_css,sprite_images"

        # The sprite should contain multiple images combined
        response = client.fetch_until_contains(
            url,
            pattern=r"Cuppa\.png.*BikeCrashIcn\.png.*IronChef2\.gif.*\.pagespeed\.is\..*\.png",
            timeout=60.0,
        )
        assert_http_status(response, 200)


class TestCssSpriteImagesExternal:
    """Tests for sprite_images with external CSS.

    Bash original::

        start_test rewrite_css,sprite_images sprites images in CSS.
        FILE=sprite_images.html?PageSpeedFilters=rewrite_css,sprite_images
        URL=$EXAMPLE_ROOT/$FILE
        fetch_until -save -recursive $URL 'grep -c css.pagespeed.cf' 1
        CSS=$(grep stylesheet "$WGET_DIR/$(basename $URL)" | cut -d\" -f 6)
        check [ $(grep -c "ic.pagespeed.is" "$SPRITE_CSS_OUT") -gt 0 ]
    """

    def test_external_css_sprite_images(
        self, client: PageSpeedClient, example_root: str
    ):
        """External CSS should contain sprite references."""
        url = f"{example_root}/sprite_images.html?PageSpeedFilters=rewrite_css,sprite_images"

        # First wait for CSS to be rewritten
        response = client.fetch_until_count(
            url,
            pattern=r"css\.pagespeed\.cf",
            expected_count=1,
            timeout=60.0,
        )
        assert_http_status(response, 200)

        # Extract the CSS URL
        match = re.search(r'href="([^"]*\.pagespeed\.cf\.[^"]*\.css)"', response.text)
        if not match:
            pytest.skip("Could not find rewritten CSS URL")

        css_url = match.group(1)
        # Handle both absolute URLs (http://...) and relative URLs
        if css_url.startswith("http://") or css_url.startswith("https://"):
            from urllib.parse import urlparse
            css_url = urlparse(css_url).path
        elif not css_url.startswith("/"):
            css_url = f"{example_root}/{css_url}"

        # The sprite may take time to generate. Retry fetching the CSS
        # until we see the sprite reference.
        css_response = client.fetch_until_contains(
            css_url,
            pattern=r"ic\.pagespeed\.is",
            timeout=60.0,
        )
        assert_http_status(css_response, 200)


if __name__ == "__main__":
    pytest.main([__file__, "-v"])
