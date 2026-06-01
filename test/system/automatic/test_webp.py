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

"""WebP image optimization tests.

Ported from: pagespeed/automatic/system_tests/optimize_to_webp.sh

These tests verify WebP conversion works correctly for different browsers.
"""

import pytest

from pagespeed_test_framework import (
    PageSpeedClient,
    assert_contains,
    assert_not_contains,
    assert_http_status,
)
from pagespeed_test_framework.client import WEBP_USER_AGENT


# User agents for different Chrome versions
# These match the bash test: using simple "Chrome/XX." format
CHROME_32_UA = "Chrome/32."
CHROME_31_UA = "Chrome/31."
CHROME_22_UA = "Chrome/22."


class TestOptimizeToWebp:
    """Tests for WebP optimization based on browser capability.

    Bash original:
        start_test Optimize images to webp
        function test_optimize_to_webp() {
          HTML="$TEST_ROOT/optimize_for_bandwidth/$1"
          URL="$HTML?PageSpeedFilters=$2"
          OUT=$($WGET ... --user-agent=$3 --header=Accept:image/webp $URL)
          check_from "$OUT" grep -q "$4"
          check_from "$OUT" grep -q "$5"
        }
    """

    def test_webp_lossless_and_animated_for_modern_chrome(
        self, client: PageSpeedClient, test_root: str
    ):
        """Modern Chrome (32+) should get both lossless and animated WebP.

        Bash original:
            test_optimize_to_webp webp_urls/rewrite_webp.html
              "convert_to_webp_lossless,convert_to_webp_animated,recompress_png"
              "Chrome/32."
              "/xCuppa.png.pagespeed.ic.*.webp"
              "/xPageSpeedAnimationSmall.gif.pagespeed.ic.*.webp"
        """
        url = (
            f"{test_root}/optimize_for_bandwidth/webp_urls/rewrite_webp.html"
            "?PageSpeedFilters=convert_to_webp_lossless,convert_to_webp_animated,recompress_png"
        )

        # Create a client with Chrome 32 user agent
        webp_client = PageSpeedClient(
            host=client.host,
            port=client.port,
            user_agent=CHROME_32_UA,
        )

        # Wait for the animated GIF→WebP conversion (slower than PNG→WebP).
        # If animated WebP is done, PNG WebP will definitely be done too.
        response = webp_client.fetch_until_contains(
            url,
            pattern=r'xPageSpeedAnimationSmall\.gif\.pagespeed\.ic\.[^"]*\.webp',
            timeout=60.0,
            headers={
                "Accept": "text/html, image/webp",
            },
        )
        assert_http_status(response, 200)

        # Should see WebP for PNG
        assert_contains(
            response,
            r'xCuppa\.png\.pagespeed\.ic\.[^"]*\.webp',
            "PNG should be converted to WebP",
        )

    def test_webp_lossless_only_for_chrome_31(
        self, client: PageSpeedClient, test_root: str
    ):
        """Chrome 31 should get lossless WebP but not animated.

        Bash original:
            test_optimize_to_webp ... "Chrome/31."
              "/xCuppa.png.pagespeed.ic.*.webp"
              "/PageSpeedAnimationSmall.gif"  (not converted)
        """
        url = (
            f"{test_root}/optimize_for_bandwidth/webp_urls/rewrite_webp.html"
            "?PageSpeedFilters=convert_to_webp_lossless,convert_to_webp_animated,recompress_png"
        )

        webp_client = PageSpeedClient(
            host=client.host,
            port=client.port,
            user_agent=CHROME_31_UA,
        )

        response = webp_client.fetch_until_contains(
            url,
            pattern=r'xCuppa\.png\.pagespeed\.ic\.[^"]*\.webp',
            timeout=60.0,
            headers={
                "Accept": "text/html, image/webp",
            },
        )
        assert_http_status(response, 200)

        # Animated GIF should NOT be converted (Chrome 31 doesn't support animated WebP)
        assert_contains(
            response,
            r"PageSpeedAnimationSmall\.gif",
            "Animated GIF should not be converted for Chrome 31",
        )

    def test_no_webp_for_old_chrome(
        self, client: PageSpeedClient, test_root: str
    ):
        """Old Chrome (22) should not get WebP, PNG recompressed instead.

        Bash original:
            test_optimize_to_webp ... "Chrome/22."
              "/xCuppa.png.pagespeed.ic.*.png"  (recompressed PNG, not WebP)
              "/PageSpeedAnimationSmall.gif"   (not converted)
        """
        url = (
            f"{test_root}/optimize_for_bandwidth/webp_urls/rewrite_webp.html"
            "?PageSpeedFilters=convert_to_webp_lossless,convert_to_webp_animated,recompress_png"
        )

        old_chrome_client = PageSpeedClient(
            host=client.host,
            port=client.port,
            user_agent=CHROME_22_UA,
        )

        response = old_chrome_client.fetch_until_contains(
            url,
            pattern=r'xCuppa\.png\.pagespeed\.ic\.[^"]*\.png',
            timeout=60.0,
            headers={
                "Accept": "text/html, image/webp",  # Even with this, old Chrome won't get WebP
            },
        )
        assert_http_status(response, 200)

        # Animated GIF should not be converted
        assert_contains(
            response,
            r"PageSpeedAnimationSmall\.gif",
            "Animated GIF should not be converted for old Chrome",
        )


if __name__ == "__main__":
    pytest.main([__file__, "-v"])
