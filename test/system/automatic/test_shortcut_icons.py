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

"""Shortcut icons tests.

Ported from: pagespeed/automatic/system_tests/shortcut_icons.sh

These tests verify that shortcut/favicon icons are optimized but not inlined.
"""

import re

import pytest

from pagespeed_test_framework import (
    PageSpeedClient,
    assert_contains,
    assert_not_contains,
    assert_http_status,
)


class TestShortcutIcons:
    """Tests for shortcut icon handling.

    Bash original::

        start_test "shortcut icons aren't inlined"
        URL="$TEST_ROOT/shortcut_icons.html?PageSpeedFilters=debug,rewrite_images"
        # We expect to see just one inline image, because of the img tag.
        fetch_until -save $URL 'grep -c data:image/png' 1
        # All four icon link tags should be optimized but not inlined.
        check_from "$OUT" grep \
          '<link rel="icon" href="[^<]*xCuppa.png.pagespeed.ic'
    """

    def test_shortcut_icons_not_inlined(
        self, client: PageSpeedClient, test_root: str
    ):
        """Shortcut icons should be optimized but not inlined."""
        url = f"{test_root}/shortcut_icons.html?PageSpeedFilters=debug,rewrite_images"

        # Only one data:image (the regular img tag) should be inlined
        response = client.fetch_until_count(
            url,
            pattern=r"data:image/png",
            expected_count=1,
            timeout=30.0,
        )
        assert_http_status(response, 200)

    def test_icon_link_optimized(
        self, client: PageSpeedClient, test_root: str
    ):
        """rel=icon link should be optimized but not inlined."""
        url = f"{test_root}/shortcut_icons.html?PageSpeedFilters=debug,rewrite_images"

        response = client.fetch_until_count(
            url,
            pattern=r"data:image/png",
            expected_count=1,
            timeout=30.0,
        )
        assert_http_status(response, 200)

        assert_contains(
            response,
            r'<link rel="icon" href="[^<]*xCuppa\.png\.pagespeed\.ic',
            "Icon link should be optimized but not inlined",
        )

    def test_apple_touch_icon_optimized(
        self, client: PageSpeedClient, test_root: str
    ):
        """rel=apple-touch-icon should be optimized but not inlined."""
        url = f"{test_root}/shortcut_icons.html?PageSpeedFilters=debug,rewrite_images"

        response = client.fetch_until_count(
            url,
            pattern=r"data:image/png",
            expected_count=1,
            timeout=30.0,
        )
        assert_http_status(response, 200)

        assert_contains(
            response,
            r'<link rel="apple-touch-icon" href="[^<]*xCuppa\.png\.pagespeed\.ic',
            "Apple touch icon should be optimized but not inlined",
        )

    def test_apple_touch_icon_precomposed_optimized(
        self, client: PageSpeedClient, test_root: str
    ):
        """rel=apple-touch-icon-precomposed should be optimized but not inlined."""
        url = f"{test_root}/shortcut_icons.html?PageSpeedFilters=debug,rewrite_images"

        response = client.fetch_until_count(
            url,
            pattern=r"data:image/png",
            expected_count=1,
            timeout=30.0,
        )
        assert_http_status(response, 200)

        assert_contains(
            response,
            r'<link rel="apple-touch-icon-precomposed" href="[^<]*xCuppa\.png\.pagespeed\.ic',
            "Apple touch icon precomposed should be optimized but not inlined",
        )

    def test_apple_touch_startup_image_optimized(
        self, client: PageSpeedClient, test_root: str
    ):
        """rel=apple-touch-startup-image should be optimized but not inlined."""
        url = f"{test_root}/shortcut_icons.html?PageSpeedFilters=debug,rewrite_images"

        response = client.fetch_until_count(
            url,
            pattern=r"data:image/png",
            expected_count=1,
            timeout=30.0,
        )
        assert_http_status(response, 200)

        assert_contains(
            response,
            r'<link rel="apple-touch-startup-image" href="[^<]*xCuppa\.png\.pagespeed\.ic',
            "Apple touch startup image should be optimized but not inlined",
        )

    def test_regular_img_inlined(
        self, client: PageSpeedClient, test_root: str
    ):
        """Regular img tags should be inlined, not just optimized.

        Bash original::

            # The image tag should have been inlined instead.
            check_not_from "$OUT" grep \
              '<img src="[^<]*xCuppa.png.pagespeed.ic'
        """
        url = f"{test_root}/shortcut_icons.html?PageSpeedFilters=debug,rewrite_images"

        response = client.fetch_until_count(
            url,
            pattern=r"data:image/png",
            expected_count=1,
            timeout=30.0,
        )
        assert_http_status(response, 200)

        # Regular img should be inlined (not have pagespeed.ic reference)
        assert_not_contains(
            response,
            r'<img src="[^<]*xCuppa\.png\.pagespeed\.ic',
            "Regular img should be inlined, not just optimized",
        )

    def test_shortcut_icon_debug_message(
        self, client: PageSpeedClient, test_root: str
    ):
        """Should have debug message explaining why icons aren't inlined.

        Bash original::

            check_from "$OUT" appears_exactly_four_times \
              "The image was not inlined because it is a shortcut icon."
        """
        url = f"{test_root}/shortcut_icons.html?PageSpeedFilters=debug,rewrite_images"

        response = client.fetch_until_count(
            url,
            pattern=r"data:image/png",
            expected_count=1,
            timeout=30.0,
        )
        assert_http_status(response, 200)

        # Should have 4 debug messages about shortcut icons
        debug_message = "The image was not inlined because it is a shortcut icon"
        count = response.text.count(debug_message)
        assert count == 4, \
            f"Expected 4 shortcut icon debug messages, found {count}"


if __name__ == "__main__":
    raise SystemExit(pytest.main([__file__, "-v"]))
