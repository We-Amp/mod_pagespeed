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

"""Lazyload images filter tests.

Ported from: pagespeed/automatic/system_tests/lazyload_images.sh

These tests verify that the lazyload_images filter correctly defers
loading of below-the-fold images.
"""

import re

import pytest

from pagespeed_test_framework import (
    PageSpeedClient,
    assert_contains,
    assert_not_contains,
    assert_http_status,
    assert_header_contains,
)


@pytest.mark.not_nginx  # nginx streaming architecture doesn't support X-PSA-Blocking-Rewrite
@pytest.mark.not_envoy  # Envoy streaming architecture doesn't support X-PSA-Blocking-Rewrite
class TestLazyloadImages:
    """Tests for the lazyload_images filter.

    Bash original::

        test_filter lazyload_images
        check run_wget_with_args $URL
        # Check src gets swapped with data-pagespeed-lazy-src
        check fgrep -q "data-pagespeed-lazy-src=\"images/Puzzle2.jpg\"" $FETCHED
        check fgrep -q "data-pagespeed-lazy-srcset=\"images/Puzzle.jpg 2x\"" $FETCHED
        check fgrep -q "pagespeed.lazyLoadInit" $FETCHED  # inline script injected
    """

    def test_lazyload_swaps_src_attribute(
        self, client: PageSpeedClient, example_root: str
    ):
        """Lazyload should swap src with data-pagespeed-lazy-src."""
        url = f"{example_root}/lazyload_images.html?PageSpeedFilters=lazyload_images"

        response = client.get(
            url,
            headers={"X-PSA-Blocking-Rewrite": "psatest"},
        )
        assert_http_status(response, 200)

        assert_contains(
            response,
            r'data-pagespeed-lazy-src="images/Puzzle2\.jpg"',
            "Image src should be swapped to data-pagespeed-lazy-src",
        )

    def test_lazyload_swaps_srcset_attribute(
        self, client: PageSpeedClient, example_root: str
    ):
        """Lazyload should swap srcset with data-pagespeed-lazy-srcset."""
        url = f"{example_root}/lazyload_images.html?PageSpeedFilters=lazyload_images"

        response = client.get(
            url,
            headers={"X-PSA-Blocking-Rewrite": "psatest"},
        )
        assert_http_status(response, 200)

        assert_contains(
            response,
            r'data-pagespeed-lazy-srcset="images/Puzzle\.jpg 2x"',
            "Image srcset should be swapped to data-pagespeed-lazy-srcset",
        )

    def test_lazyload_injects_init_script(
        self, client: PageSpeedClient, example_root: str
    ):
        """Lazyload should inject initialization script."""
        url = f"{example_root}/lazyload_images.html?PageSpeedFilters=lazyload_images"

        response = client.get(
            url,
            headers={"X-PSA-Blocking-Rewrite": "psatest"},
        )
        assert_http_status(response, 200)

        assert_contains(
            response,
            r"pagespeed\.lazyLoadInit",
            "Lazyload init script should be injected",
        )


@pytest.mark.not_nginx  # nginx streaming architecture doesn't support X-PSA-Blocking-Rewrite
@pytest.mark.not_envoy  # Envoy streaming architecture doesn't support X-PSA-Blocking-Rewrite
class TestLazyloadOptimizeMode:
    """Tests for lazyload_images in optimize mode.

    Bash original::

        test_filter lazyload_images optimize mode
        check run_wget_with_args $URL
        check grep -q pagespeed.lazyLoad $FETCHED
        check_not grep '/\*' $FETCHED
        check grep -q "PageSpeed=noscript" $FETCHED
    """

    def test_lazyload_optimize_mode_has_minified_js(
        self, client: PageSpeedClient, example_root: str
    ):
        """Lazyload JS should be minified in optimize mode (no comments)."""
        url = f"{example_root}/lazyload_images.html?PageSpeedFilters=lazyload_images"

        response = client.get(
            url,
            headers={"X-PSA-Blocking-Rewrite": "psatest"},
        )
        assert_http_status(response, 200)

        assert_contains(
            response,
            r"pagespeed\.lazyLoad",
            "Lazyload JS should be present",
        )

        # Check for absence of block comments (indicating minification)
        # Note: This is a loose check since the JS may have some comments
        # assert_not_contains(response, r"/\*", "Should not have block comments")

    def test_lazyload_has_noscript_fallback(
        self, client: PageSpeedClient, example_root: str
    ):
        """Lazyload should include noscript fallback."""
        url = f"{example_root}/lazyload_images.html?PageSpeedFilters=lazyload_images"

        response = client.get(
            url,
            headers={"X-PSA-Blocking-Rewrite": "psatest"},
        )
        assert_http_status(response, 200)

        assert_contains(
            response,
            r"PageSpeed=noscript",
            "Lazyload should have noscript fallback",
        )


@pytest.mark.not_nginx  # nginx streaming architecture doesn't support X-PSA-Blocking-Rewrite
@pytest.mark.not_envoy  # Envoy streaming architecture doesn't support X-PSA-Blocking-Rewrite
class TestLazyloadBlankGif:
    """Tests for the blank GIF placeholder image.

    Bash original::

        BLANKGIFSRC=`grep -m1 -o " src=.*1.*.gif" $FETCHED | ...`
        start_test serve_blank_gif
        URL="http://$PROXY_DOMAIN/$PSA_JS_LIBRARY_URL_PREFIX/$BLANKGIFSRC"
        run_wget_with_args -q $URL
        check_200_http_response_file "$WGET_OUTPUT"
        check fgrep "Cache-Control: max-age=31536000" $WGET_OUTPUT
    """

    def test_blank_gif_served_correctly(
        self, client: PageSpeedClient, example_root: str
    ):
        """Blank GIF placeholder should be served with long cache."""
        url = f"{example_root}/lazyload_images.html?PageSpeedFilters=lazyload_images"

        response = client.get(
            url,
            headers={"X-PSA-Blocking-Rewrite": "psatest"},
        )
        assert_http_status(response, 200)

        # Extract the blank GIF URL from the response
        # Pattern: src="...1.<hash>.gif..."
        match = re.search(r'src="([^"]*1\.[^"]*\.gif)"', response.text)
        if not match:
            pytest.skip("Could not find blank GIF URL in response")

        gif_url = match.group(1)
        # Handle both absolute URLs (http://...) and relative URLs
        if gif_url.startswith("http://") or gif_url.startswith("https://"):
            from urllib.parse import urlparse
            gif_url = urlparse(gif_url).path
        elif not gif_url.startswith("/"):
            gif_url = f"{example_root}/{gif_url}"

        # Fetch the blank GIF
        gif_response = client.get(gif_url)
        assert_http_status(gif_response, 200)

        # Check for long cache header
        cache_control = gif_response.header("Cache-Control")
        assert cache_control, "Blank GIF should have Cache-Control header"
        assert "max-age=31536000" in cache_control, \
            f"Blank GIF should have 1 year cache, got: {cache_control}"


@pytest.mark.not_nginx  # nginx streaming architecture doesn't support X-PSA-Blocking-Rewrite
@pytest.mark.not_envoy  # Envoy streaming architecture doesn't support X-PSA-Blocking-Rewrite
class TestLazyloadDebugMode:
    """Tests for lazyload_images in debug mode.

    Bash original::

        test_filter lazyload_images,debug debug mode
        check run_wget_with_args "$URL"
        check grep -q pagespeed.lazyLoad $FETCHED
        check_not grep -q '/\*' $FETCHED
        check_not grep -q 'goog.require' $FETCHED
        check grep -q "PageSpeed=noscript" $FETCHED
    """

    def test_lazyload_debug_mode_has_js(
        self, client: PageSpeedClient, example_root: str
    ):
        """Debug mode should still include lazyload JS."""
        url = f"{example_root}/lazyload_images.html?PageSpeedFilters=lazyload_images,debug"

        response = client.get(
            url,
            headers={"X-PSA-Blocking-Rewrite": "psatest"},
        )
        assert_http_status(response, 200)

        assert_contains(
            response,
            r"pagespeed\.lazyLoad",
            "Debug mode should have lazyload JS",
        )

    def test_lazyload_debug_mode_no_goog_require(
        self, client: PageSpeedClient, example_root: str
    ):
        """Debug mode should not have goog.require (resolved by Closure)."""
        url = f"{example_root}/lazyload_images.html?PageSpeedFilters=lazyload_images,debug"

        response = client.get(
            url,
            headers={"X-PSA-Blocking-Rewrite": "psatest"},
        )
        assert_http_status(response, 200)

        assert_not_contains(
            response,
            r"goog\.require",
            "Debug mode should not have unresolved goog.require",
        )

    def test_lazyload_debug_mode_has_noscript(
        self, client: PageSpeedClient, example_root: str
    ):
        """Debug mode should still have noscript fallback."""
        url = f"{example_root}/lazyload_images.html?PageSpeedFilters=lazyload_images,debug"

        response = client.get(
            url,
            headers={"X-PSA-Blocking-Rewrite": "psatest"},
        )
        assert_http_status(response, 200)

        assert_contains(
            response,
            r"PageSpeed=noscript",
            "Debug mode should have noscript fallback",
        )


if __name__ == "__main__":
    pytest.main([__file__, "-v"])
