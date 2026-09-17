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

"""Image resize tests.

Ported from: pagespeed/automatic/system_tests/image_resize.sh

These tests verify that images are correctly resized based on HTML dimensions.
"""

import pytest

from pagespeed_test_framework import (
    PageSpeedClient,
    assert_contains,
    assert_http_status,
)
from pagespeed_test_framework.client import WEBP_ANIMATED_USER_AGENT


class TestImageResize:
    """Tests for image resizing functionality.

    Bash original::

        start_test resize images and modify URLs
        HTML_PATH="$TEST_ROOT/webp_rewriting/"
        HTML_OPT="?PageSpeedFilters=rewrite_images,rewrite_css,convert_to_webp_animated"
        HTML_OPT="$HTML_OPT&$IMAGES_QUALITY=75&$WEBP_QUALITY=65"
        resize_image_test $HTML_URL 4
    """

    def test_resizable_images_rewritten(
        self, client: PageSpeedClient, test_root: str
    ):
        """Images with specified dimensions should be resized."""
        filters = "rewrite_images,rewrite_css,convert_to_webp_animated"
        url = f"{test_root}/webp_rewriting/resizable_images.html?PageSpeedFilters={filters}"

        # Create a WebP-capable client
        webp_client = PageSpeedClient(
            host=client.host,
            port=client.port,
            user_agent=WEBP_ANIMATED_USER_AGENT,
        )

        response = webp_client.fetch_until_count(
            url,
            pattern=r"\.pagespeed\.ic\.",
            expected_count=4,
            timeout=60.0,
        )
        assert_http_status(response, 200)

    def test_resized_puzzle_image(
        self, client: PageSpeedClient, test_root: str
    ):
        """Puzzle image should be resized to specified dimensions."""
        filters = "rewrite_images,rewrite_css,convert_to_webp_animated"
        url = f"{test_root}/webp_rewriting/resizable_images.html?PageSpeedFilters={filters}"

        webp_client = PageSpeedClient(
            host=client.host,
            port=client.port,
            user_agent=WEBP_ANIMATED_USER_AGENT,
        )

        response = webp_client.fetch_until_contains(
            url,
            pattern=r"\.pagespeed\.ic\.",
            timeout=60.0,
        )
        assert_http_status(response, 200)

        # Check for resized Puzzle image (120x150)
        assert_contains(
            response,
            r"120x150xPuzzle2\.jpg\.pagespeed\.ic\.",
            "Puzzle image should be resized",
        )


class TestUnresizableImages:
    """Tests for images that cannot be resized.

    Bash original::

        # Check that the images will be recompressed without resizing.
        HTML_URL="unresizable_images.html"
        resize_image_test $HTML_URL 3

    Uses blocking rewrite on Apache/Envoy to ensure the CSS background-image
    rewrite chain (HTML parse → CSS parse → image fetch → recompress) completes.
    On nginx (no blocking rewrite), fetch_until_count retries until the
    background optimization finishes.
    """

    def test_unresizable_images_recompressed(
        self, client: PageSpeedClient, test_root: str
    ):
        """Images without dimensions should be recompressed but not resized."""
        filters = "rewrite_images,rewrite_css,convert_to_webp_animated"
        url = f"{test_root}/webp_rewriting/unresizable_images.html?PageSpeedFilters={filters}"

        webp_client = PageSpeedClient(
            host=client.host,
            port=client.port,
            user_agent=WEBP_ANIMATED_USER_AGENT,
        )

        # Expected .pagespeed.ic. matches: 2
        # - disclosure_open_plus.png → .pagespeed.ic.*.webp (PNG recompressed)
        # - Puzzle2.jpg (CSS background) → .pagespeed.ic.*.webp (JPEG recompressed)
        # - gray_saved_as_rgb.webp is NOT rewritten (already in WebP format)
        # - schedule_event.svg is NOT rewritten (SVG not processed)
        response = webp_client.fetch_until_count(
            url,
            pattern=r"\.pagespeed\.ic\.",
            expected_count=2,
            timeout=60.0,
            headers={
                "Accept": "text/html, image/webp",
                "X-PSA-Blocking-Rewrite": "psatest",
            },
        )
        assert_http_status(response, 200)


class TestAnimatedImages:
    """Tests for animated image handling.

    Bash original::

        # Check that animated images will be recompressed without resizing.
        HTML_URL="animated_images.html"
        resize_image_test $HTML_URL 1

    Uses blocking rewrite on Apache/Envoy. On nginx (no blocking rewrite),
    fetch_until_count retries until the background GIF-to-WebP conversion
    completes.
    """

    def test_animated_images_recompressed(
        self, client: PageSpeedClient, test_root: str
    ):
        """Animated images should be recompressed."""
        filters = "rewrite_images,rewrite_css,convert_to_webp_animated"
        url = f"{test_root}/webp_rewriting/animated_images.html?PageSpeedFilters={filters}"

        webp_client = PageSpeedClient(
            host=client.host,
            port=client.port,
            user_agent=WEBP_ANIMATED_USER_AGENT,
        )

        # The animated GIF-to-WebP conversion requires both the webp-animated
        # user agent and the Accept: image/webp header. Blocking rewrite is
        # needed because the conversion is too slow for the non-blocking path.
        response = webp_client.fetch_until_count(
            url,
            pattern=r"\.pagespeed\.ic\.",
            expected_count=1,
            timeout=60.0,
            headers={
                "X-PSA-Blocking-Rewrite": "psatest",
                "Accept": "text/html, image/webp",
            },
        )
        assert_http_status(response, 200)

        # Check for animated GIF converted to WebP
        assert_contains(
            response,
            r"xPageSpeedAnimationSmall\.gif\.pagespeed\.ic\.",
            "Animated GIF should be converted",
        )


if __name__ == "__main__":
    raise SystemExit(pytest.main([__file__, "-v"]))
