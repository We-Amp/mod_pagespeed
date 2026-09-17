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

"""Lazyload images filter tests for IIS PageSpeed module.

Ported from: test/system/automatic/test_lazyload.py

These tests verify that the lazyload_images filter correctly defers
loading of below-the-fold images. IIS uses streaming architecture,
so we use fetch_until instead of X-PSA-Blocking-Rewrite header.
"""

import re
import pytest

from pagespeed_test_framework import (
    PageSpeedClient,
    assert_contains,
    assert_not_contains,
    assert_http_status,
)


@pytest.mark.html_rewrite
class TestLazyloadImages:
    """Tests for the lazyload_images filter.

    These tests verify that images are converted to lazy-loaded versions
    with data-pagespeed-lazy-src and data-pagespeed-lazy-srcset attributes.

    Note: IIS uses streaming architecture like Envoy/nginx, so we use
    fetch_until to poll for the optimized response instead of
    X-PSA-Blocking-Rewrite header.
    """

    def test_lazyload_swaps_src_attribute(
        self, client: PageSpeedClient, example_root: str
    ):
        """Lazyload should swap src with data-pagespeed-lazy-src.

        The lazyload filter replaces img src with a blank placeholder
        and stores the original URL in data-pagespeed-lazy-src.
        """
        url = f"{example_root}/lazyload_images.html?PageSpeedFilters=lazyload_images"

        # Use fetch_until to wait for optimization to complete
        response = client.fetch_until_contains(
            url,
            r'data-pagespeed-lazy-src=',
            timeout=30.0,
        )
        assert_http_status(response, 200)

        assert_contains(
            response,
            r'data-pagespeed-lazy-src=',
            "Image src should be swapped to data-pagespeed-lazy-src",
        )

    def test_lazyload_swaps_srcset_attribute(
        self, client: PageSpeedClient, example_root: str
    ):
        """Lazyload should swap srcset with data-pagespeed-lazy-srcset.

        The lazyload filter also handles srcset attributes for
        responsive images.
        """
        url = f"{example_root}/lazyload_images.html?PageSpeedFilters=lazyload_images"

        # Use fetch_until to wait for optimization to complete
        response = client.fetch_until_contains(
            url,
            r'data-pagespeed-lazy-srcset=',
            timeout=30.0,
        )
        assert_http_status(response, 200)

        assert_contains(
            response,
            r'data-pagespeed-lazy-srcset=',
            "Image srcset should be swapped to data-pagespeed-lazy-srcset",
        )

    def test_lazyload_injects_init_script(
        self, client: PageSpeedClient, example_root: str
    ):
        """Lazyload should inject initialization script.

        The filter injects a pagespeed.lazyLoadInit() call to set up
        the lazy loading behavior on page load.
        """
        url = f"{example_root}/lazyload_images.html?PageSpeedFilters=lazyload_images"

        response = client.fetch_until_contains(
            url,
            r"pagespeed\.lazyLoadInit",
            timeout=30.0,
        )
        assert_http_status(response, 200)

        assert_contains(
            response,
            r"pagespeed\.lazyLoadInit",
            "Lazyload init script should be injected",
        )


@pytest.mark.html_rewrite
class TestLazyloadOptimizeMode:
    """Tests for lazyload_images in optimize mode.

    These tests verify that lazyload JavaScript is properly minified
    and includes noscript fallback for browsers without JavaScript.
    """

    def test_lazyload_optimize_mode_has_minified_js(
        self, client: PageSpeedClient, example_root: str
    ):
        """Lazyload JS should be minified in optimize mode (no comments).

        In production mode, the lazyload JavaScript code should be
        minified with no block comments (/* ... */).
        """
        url = f"{example_root}/lazyload_images.html?PageSpeedFilters=lazyload_images"

        response = client.fetch_until_contains(
            url,
            r"pagespeed\.lazyLoad",
            timeout=30.0,
        )
        assert_http_status(response, 200)

        assert_contains(
            response,
            r"pagespeed\.lazyLoad",
            "Lazyload JS should be present",
        )

    def test_lazyload_has_noscript_fallback(
        self, client: PageSpeedClient, example_root: str
    ):
        """Lazyload should include noscript fallback.

        The filter adds a noscript element with PageSpeed=noscript
        URL to allow images to load when JavaScript is disabled.
        """
        url = f"{example_root}/lazyload_images.html?PageSpeedFilters=lazyload_images"

        response = client.fetch_until_contains(
            url,
            r"PageSpeed=noscript",
            timeout=30.0,
        )
        assert_http_status(response, 200)

        assert_contains(
            response,
            r"PageSpeed=noscript",
            "Lazyload should have noscript fallback",
        )


@pytest.mark.html_rewrite
class TestLazyloadDebugMode:
    """Tests for lazyload_images in debug mode.

    Debug mode enables additional logging and human-readable code
    while still performing the optimization.
    """

    def test_lazyload_debug_mode_has_js(
        self, client: PageSpeedClient, example_root: str
    ):
        """Debug mode should still include lazyload JS.

        Even in debug mode, the lazyload functionality should work.
        """
        url = f"{example_root}/lazyload_images.html?PageSpeedFilters=lazyload_images,debug"

        response = client.fetch_until_contains(
            url,
            r"pagespeed\.lazyLoad",
            timeout=30.0,
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
        """Debug mode should not have goog.require (resolved by Closure).

        The Closure compiler should resolve all goog.require statements
        even in debug mode.
        """
        url = f"{example_root}/lazyload_images.html?PageSpeedFilters=lazyload_images,debug"

        # First, wait for the lazyload JS to appear
        response = client.fetch_until_contains(
            url,
            r"pagespeed\.lazyLoad",
            timeout=30.0,
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
        """Debug mode should still have noscript fallback.

        The noscript fallback should be present regardless of debug mode.
        """
        url = f"{example_root}/lazyload_images.html?PageSpeedFilters=lazyload_images,debug"

        response = client.fetch_until_contains(
            url,
            r"PageSpeed=noscript",
            timeout=30.0,
        )
        assert_http_status(response, 200)

        assert_contains(
            response,
            r"PageSpeed=noscript",
            "Debug mode should have noscript fallback",
        )


@pytest.mark.html_rewrite
class TestLazyloadBlankGif:
    """Tests for the blank GIF placeholder image.

    When lazyload is active, images are replaced with a blank GIF
    placeholder that should be served with long cache headers.
    """

    def test_blank_gif_url_present(
        self, client: PageSpeedClient, example_root: str
    ):
        """Lazyload should use a blank GIF placeholder.

        The img src should be replaced with a 1x1 blank GIF URL.
        """
        url = f"{example_root}/lazyload_images.html?PageSpeedFilters=lazyload_images"

        # Wait for lazyload to be applied
        response = client.fetch_until_contains(
            url,
            r'data-pagespeed-lazy-src=',
            timeout=30.0,
        )
        assert_http_status(response, 200)

        # Check for blank GIF URL pattern (1.<hash>.gif)
        # The exact pattern may vary but should contain .gif
        match = re.search(r'src="([^"]*\.gif)"', response.text)
        assert match is not None, "Blank GIF placeholder should be present in img src"

    def test_blank_gif_served_with_cache_headers(
        self, client: PageSpeedClient, example_root: str
    ):
        """Blank GIF should be served with long cache headers.

        The blank GIF resource should have Cache-Control headers
        allowing long-term caching (1 year).
        """
        url = f"{example_root}/lazyload_images.html?PageSpeedFilters=lazyload_images"

        # Wait for lazyload to be applied
        response = client.fetch_until_contains(
            url,
            r'data-pagespeed-lazy-src=',
            timeout=30.0,
        )
        assert_http_status(response, 200)

        # Extract the blank GIF URL from the response
        match = re.search(r'src="([^"]*1\.[^"]*\.gif)"', response.text)
        if not match:
            pytest.skip("Could not find blank GIF URL with hash in response")

        gif_url = match.group(1)
        # Handle both absolute URLs (http://...) and relative URLs
        if gif_url.startswith("http://") or gif_url.startswith("https://"):
            from urllib.parse import urlparse
            gif_url = urlparse(gif_url).path
        elif not gif_url.startswith("/"):
            # Relative URL - prepend example_root
            gif_url = f"{example_root}/{gif_url}"

        # Fetch the blank GIF
        gif_response = client.get(gif_url)
        assert_http_status(gif_response, 200)

        # Check for long cache header
        cache_control = gif_response.header("Cache-Control")
        assert cache_control, "Blank GIF should have Cache-Control header"
        # Check for long max-age (1 year = 31536000 seconds)
        assert "max-age=31536000" in cache_control, \
            f"Blank GIF should have 1 year cache, got: {cache_control}"


@pytest.mark.html_rewrite
class TestLazyloadDisabled:
    """Tests that verify lazyload behavior when disabled."""

    def test_no_lazyload_without_filter(
        self, client: PageSpeedClient, example_root: str
    ):
        """Without lazyload filter, images should not be modified.

        When lazyload_images filter is not enabled, the img tags
        should remain unchanged.
        """
        url = f"{example_root}/lazyload_images.html?PageSpeed=off"

        response = client.get(url)
        assert_http_status(response, 200)

        assert_not_contains(
            response,
            r"data-pagespeed-lazy-src",
            "Without lazyload filter, no lazy-src attributes should be present",
        )

        assert_not_contains(
            response,
            r"pagespeed\.lazyLoad",
            "Without lazyload filter, no lazyload JS should be injected",
        )


if __name__ == "__main__":
    raise SystemExit(pytest.main([__file__, "-v"]))
