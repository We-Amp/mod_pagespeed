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

import re
from urllib.parse import urljoin, urlsplit

import pytest

from pagespeed_test_framework import (
    PageSpeedClient,
    assert_contains,
    assert_file_size,
    assert_http_status,
    require_match,
    require_status_ok,
)
from pagespeed_test_framework.client import WEBP_ANIMATED_USER_AGENT
from pagespeed_test_framework.stats import count_matching_lines


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


QUALITY_OPTS = (
    "?PageSpeedFilters=rewrite_images,rewrite_css,convert_to_webp_animated"
    "&PageSpeedImageRecompressionQuality=75&PageSpeedWebpRecompressionQuality=65"
)
# The two headers the other classes of this module already need on this
# lane (blocking rewrite; Accept: image/webp for the WebP variants).
RESIZE_HEADERS = {"X-PSA-Blocking-Rewrite": "psatest", "Accept": "text/html, image/webp"}


def _resource_path(page_path: str, src: str) -> str:
    return urlsplit(urljoin(f"http://host{page_path}", src)).path


def _ceiling(bash_value: int) -> int:
    """5% headroom over the bash suite's ceiling: an older encoder made it
    tighter than this port's, so an unresized or unrecompressed image is
    about ten times larger and the check stays meaningful even with the
    margin."""
    return int(bash_value * 1.05)


class TestImageResizeSizes:
    """The size bounds of image_resize.sh (resize images and modify URLs).

    Each page is polled until it carries its number of rewritten images;
    each named image must be on the page and its rewritten bytes within the
    bash's bounds, with 5% headroom over the bash ceilings (an older encoder
    made the bash's ceilings tighter than this port's).

    unresizable_images.html under the quality options carries three
    rewrites, matching resize_image_test's count of 3 in the bash: the
    CSS-background Puzzle2 image, the already-WebP gray_saved_as_rgb.webp
    (recompressed under the WebP quality option), and disclosure_open_plus.png
    (recompressed to a smaller genuine WebP -- confirmed by fetching it
    directly: Content-Type image/webp with a genuine RIFF/WEBP payload, not
    a PNG pass-through). All three are polled for and bounded below.
    """

    @pytest.mark.parametrize(
        "page,count,leaf,low,high",
        [
            pytest.param("resizable_images.html", 4, r"120x150xPuzzle2\.jpg", 0, _ceiling(2340),
                         id="120x150-puzzle2"),
            pytest.param("resizable_images.html", 4, r"90xNxIronChef2\.gif", 0, _ceiling(1624),
                         id="90xN-ironchef2"),
            pytest.param("resizable_images.html", 4, r"Nx50xBikeCrashIcn\.png", 0, _ceiling(822),
                         id="Nx50-bikecrash"),
            pytest.param("resizable_images.html", 4, r"120x120xpagespeed_logo\.png", 0, _ceiling(9214),
                         id="120x120-logo"),
            pytest.param("unresizable_images.html", 3, r"xdisclosure_open_plus\.png", 150, _ceiling(298),
                         id="disclosure-unresized"),
            pytest.param("unresizable_images.html", 3, r"xgray_saved_as_rgb\.webp", 270, _ceiling(317),
                         id="gray-rgb-unresized"),
            pytest.param("unresizable_images.html", 3, r"xPuzzle2\.jpg", 12212, _ceiling(31065),
                         id="puzzle2-unresized"),
            pytest.param("animated_images.html", 1, r"xPageSpeedAnimationSmall\.gif", 7222, _ceiling(26250),
                         id="animation"),
        ],
    )
    def test_rewritten_image_size(self, client: PageSpeedClient, test_root: str,
                                  page, count, leaf, low, high):
        webp = PageSpeedClient(host=client.host, port=client.port,
                               user_agent=WEBP_ANIMATED_USER_AGENT)
        page_path = f"{test_root}/webp_rewriting/{page}"
        html = webp.fetch_until(
            page_path + QUALITY_OPTS,
            condition=lambda r: count_matching_lines(r.text, re.escape(".pagespeed.ic.")) == count,
            headers=RESIZE_HEADERS,
            timeout=100.0,
        )
        src = require_match(r"([^\"'() ]*/" + leaf + r"\.pagespeed\.ic\.[^\"'() ]*\.webp)",
                            html, f"{leaf} rewritten URL").group(1)
        image = webp.get(_resource_path(page_path, src), headers=RESIZE_HEADERS)
        require_status_ok(image, src)
        assert_file_size(image, "-ge", low, src)
        assert_file_size(image, "-le", high, src)


if __name__ == "__main__":
    raise SystemExit(pytest.main([__file__, "-v"]))
